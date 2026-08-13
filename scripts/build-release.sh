#!/usr/bin/env bash
# Build release `rtl-designdb` binaries for the supported deployment platforms.
#
# The platform set is rwave's, deliberately: this database is read next to a
# waveform, so it ships everywhere the viewer does.
#
#   target           Toolchain                        Output                                Linking
#   --------------   ------------------------------   -----------------------------------  ----------
#   linux-amd64      Alpine container (gcc/musl)      dist/rtl-designdb-linux-amd64         fully static
#   linux-arm64      Alpine container (gcc/musl)      dist/rtl-designdb-linux-arm64         fully static
#   windows-amd64    MSVC on a Windows host           dist/rtl-designdb-windows-amd64.exe   static CRT (no DLLs required)
#   macos-arm64      native clang (Apple Silicon)     dist/rtl-designdb-macos-arm64         native
#
# Where rwave pins linux-amd64 to a glibc 2.17 baseline (its plugin backends
# dlopen vendor .so files, so it must stay glibc-dynamic), this exporter has no
# dlopen at all — SQLite is compiled in with loadable extensions omitted — so
# both Linux targets are musl and fully static: one file that runs on any
# distro, any glibc, including the CentOS 7-era farms EDA tools live on.
#
# Host support:
#   - Linux targets build natively when the host is already musl of the right
#     arch (an Alpine CI container), and through `docker run --platform` from
#     any other host. Cross-arch docker goes through emulation and is slow.
#   - macos-arm64 needs an Apple Silicon macOS host (Apple SDK; no cross).
#   - windows-amd64 needs a Windows host with Visual Studio (CI builds it).
#
# Usage:
#   scripts/build-release.sh                               # every target this host can build
#   scripts/build-release.sh --target linux-amd64          # one target
#   scripts/build-release.sh --target linux-amd64,macos-arm64
#   scripts/build-release.sh --run                         # smoke-test runnable outputs
set -euo pipefail

cd "$(dirname "$0")/.."

ALL_TARGETS=(linux-amd64 linux-arm64 windows-amd64 macos-arm64)

# ---- args ----------------------------------------------------------------
TARGETS_INPUT=""
RUN=0
while [ $# -gt 0 ]; do
  case "$1" in
    --target) TARGETS_INPUT="${2:-}"; shift 2 ;;
    --target=*) TARGETS_INPUT="${1#*=}"; shift ;;
    --run) RUN=1; shift ;;
    -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

info() { printf '>> %s\n' "$*"; }
ok()   { printf '   %s\n' "$*"; }
die()  { printf 'XX %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

HOST_OS="$(uname -s)"     # Linux | Darwin | MINGW64_NT-* | MSYS_NT-*
HOST_ARCH="$(uname -m)"   # x86_64 | arm64 | aarch64
case "$HOST_OS" in MINGW*|MSYS*|CYGWIN*) HOST_OS=Windows ;; esac

is_musl_host() {
  [ "$HOST_OS" = "Linux" ] || return 1
  [ -f /etc/alpine-release ] && return 0
  ldd --version 2>&1 | grep -qi musl
}

ncpu() {
  if have nproc; then nproc
  elif [ "$HOST_OS" = "Darwin" ]; then sysctl -n hw.ncpu
  else echo 4; fi
}

output_for() {
  case "$1" in
    windows-amd64) echo "dist/rtl-designdb-$1.exe" ;;
    *)             echo "dist/rtl-designdb-$1" ;;
  esac
}

arch_for() {
  case "$1" in
    linux-amd64) echo "x86_64" ;;
    linux-arm64) echo "aarch64" ;;
  esac
}

# Which targets can this host produce at all?
host_can_build() {
  case "$1" in
    linux-*)       [ "$HOST_OS" = "Linux" ] || [ "$HOST_OS" = "Darwin" ] ;;
    macos-arm64)   [ "$HOST_OS" = "Darwin" ] && { [ "$HOST_ARCH" = "arm64" ] || [ "$HOST_ARCH" = "aarch64" ]; } ;;
    windows-amd64) [ "$HOST_OS" = "Windows" ] ;;
    *) return 1 ;;
  esac
}

if [ -z "$TARGETS_INPUT" ]; then
  # Default to what the host can build, and say what was skipped.
  TARGETS=()
  for t in "${ALL_TARGETS[@]}"; do
    if host_can_build "$t"; then TARGETS+=("$t"); else info "Skipping $t: not buildable from $HOST_OS/$HOST_ARCH (CI covers it)."; fi
  done
  [ "${#TARGETS[@]}" -gt 0 ] || die "no target in (${ALL_TARGETS[*]}) is buildable from this host."
else
  IFS=',' read -r -a TARGETS <<< "$TARGETS_INPUT"
  for t in "${TARGETS[@]}"; do
    case " ${ALL_TARGETS[*]} " in
      *" $t "*) ;;
      *) die "unknown target '$t' (expected: ${ALL_TARGETS[*]})" ;;
    esac
    host_can_build "$t" || die "$t cannot be built from $HOST_OS/$HOST_ARCH (see the header of this script)."
  done
fi

# ---- prerequisite checks -------------------------------------------------
for t in "${TARGETS[@]}"; do
  case "$t" in
    linux-*)
      if is_musl_host && [ "$HOST_ARCH" = "$(arch_for "$t")" ]; then
        have cmake && have c++ && have python3 && have git ||
          die "musl host is missing build tools. Install them with:
   apk add build-base cmake ninja python3 git ca-certificates linux-headers"
      else
        have docker || die "building $t from $HOST_OS needs docker (an Alpine container does the build)."
      fi
      ;;
    macos-arm64)
      have cmake || die "cmake not found. Install it with:  brew install cmake"
      have c++   || die "no C++ compiler. Install the Xcode command line tools:  xcode-select --install"
      ;;
    windows-amd64)
      have cmake || die "cmake not found on this Windows host."
      ;;
  esac
done

mkdir -p dist

# ---- build ---------------------------------------------------------------
# One build tree per target so switching targets never poisons a cache.
# CPM_SOURCE_CACHE is honoured if the caller set one; the docker path uses a
# named volume so slang is cloned once, not per build.
cmake_fresh() { # <build-dir> <extra flags...>
  local bdir="$1"; shift
  # A glued -GNinja rather than an array: macOS ships bash 3.2, where
  # expanding an empty array trips `set -u`.
  local gen=""
  have ninja && gen="-GNinja"
  cmake -S . -B "$bdir" $gen -DCMAKE_BUILD_TYPE=Release \
    ${CPM_SOURCE_CACHE:+-DCPM_SOURCE_CACHE="$CPM_SOURCE_CACHE"} "$@"
}

build_linux_native() { # <target>
  local t="$1" bdir="build-release/$1" out; out="$(output_for "$1")"
  info "Building $t natively (musl, fully static) ..."
  cmake_fresh "$bdir" -DCMAKE_EXE_LINKER_FLAGS=-static
  cmake --build "$bdir" -j"$(ncpu)"
  cp "$bdir/rtl-designdb" "$out"
  strip "$out"
  # A dynamic binary here would defeat the point of the musl build; refuse
  # to ship it rather than let it surface as ENOENT on someone's farm.
  if ldd "$out" >/dev/null 2>&1; then
    die "$out is dynamically linked; expected fully static."
  fi
}

build_linux_docker() { # <target>
  local t="$1" arch platform
  arch="$(arch_for "$t")"
  case "$t" in linux-amd64) platform=linux/amd64 ;; linux-arm64) platform=linux/arm64 ;; esac
  if [ "$HOST_ARCH" != "$arch" ] && ! { [ "$HOST_ARCH" = "arm64" ] && [ "$arch" = "aarch64" ]; }; then
    info "Note: $t is cross-arch for this host; docker runs it under emulation, expect a slow build."
  fi
  info "Building $t in an Alpine container ..."
  docker run --rm --platform "$platform" \
    -v "$(pwd):/src" \
    -v rtl-designdb-cpm:/cpm \
    -v "rtl-designdb-build-$t:/build" \
    -w /src alpine:3.21 sh -ec '
      apk add --no-cache -q build-base cmake ninja python3 git ca-certificates linux-headers
      export CPM_SOURCE_CACHE=/cpm
      cmake -S . -B "/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCPM_SOURCE_CACHE=/cpm -DCMAKE_EXE_LINKER_FLAGS=-static
      cmake --build /build -j"$(nproc)"
      cp /build/rtl-designdb "'"$(output_for "$t")"'"
      strip "'"$(output_for "$t")"'"
      ldd "'"$(output_for "$t")"'" >/dev/null 2>&1 && { echo "XX binary is dynamic" >&2; exit 1; } || true
    '
}

build_macos() {
  local bdir="build-release/macos-arm64" out; out="$(output_for macos-arm64)"
  info "Building macos-arm64 natively ..."
  # 11.0 is the first macOS Apple Silicon ever ran; nothing older exists to support.
  cmake_fresh "$bdir" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
  cmake --build "$bdir" -j"$(ncpu)"
  cp "$bdir/rtl-designdb" "$out"
  strip "$out"
}

build_windows() {
  local bdir="build-release/windows-amd64" out; out="$(output_for windows-amd64)"
  info "Building windows-amd64 with MSVC ..."
  # Multi-config generator: -DCMAKE_BUILD_TYPE is ignored, --config picks it.
  # MultiThreaded = static CRT, so the exe needs no vcredist on the target box.
  cmake -S . -B "$bdir" -A x64 \
    ${CPM_SOURCE_CACHE:+-DCPM_SOURCE_CACHE="$CPM_SOURCE_CACHE"} \
    -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
  cmake --build "$bdir" --config Release -j"$(ncpu)"
  cp "$bdir/Release/rtl-designdb.exe" "$out"
}

build_one() {
  local t="$1"
  case "$t" in
    linux-*)
      if is_musl_host && [ "$HOST_ARCH" = "$(arch_for "$t")" ]; then
        build_linux_native "$t"
      else
        build_linux_docker "$t"
      fi
      ;;
    macos-arm64)   build_macos ;;
    windows-amd64) build_windows ;;
  esac
  local out; out="$(output_for "$t")"
  [ -f "$out" ] || die "build reported success but $out is missing."
  ok "Binary: $out"
  if have file; then ok "$(file "$out")"; fi
  ok "Size:   $(( $(wc -c < "$out") / 1024 )) KB"
}

for t in "${TARGETS[@]}"; do
  build_one "$t"
done

# ---- optional smoke test -------------------------------------------------
if [ "$RUN" = "1" ]; then
  for t in "${TARGETS[@]}"; do
    out="$(output_for "$t")"
    runnable=0
    case "$t" in
      linux-amd64)   [ "$HOST_OS" = "Linux" ] && [ "$HOST_ARCH" = "x86_64" ] && runnable=1 ;;
      linux-arm64)   [ "$HOST_OS" = "Linux" ] && [ "$HOST_ARCH" = "aarch64" ] && runnable=1 ;;
      macos-arm64)   [ "$HOST_OS" = "Darwin" ] && runnable=1 ;;
      windows-amd64) [ "$HOST_OS" = "Windows" ] && runnable=1 ;;
    esac
    if [ "$runnable" = "1" ]; then
      info "Smoke test: $out exports examples/basic/top.sv"
      tmpdb="$(mktemp -d)/design.db"
      "$out" examples/basic/top.sv --top top -o "$tmpdb"
      rm -f "$tmpdb"
    else
      info "Skipping --run for $t: not runnable on $HOST_OS/$HOST_ARCH."
    fi
  done
fi

info "Done."
