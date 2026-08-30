// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// rtl-designdb — read a VCS-style filelist, elaborate, write a queryable
// design database.
//
// Deliberately small. It takes a filelist and defines, the way `vcs -f` does,
// and nothing else: configuration, project layout and output formatting belong
// to whatever drives it, and this binary does the one job that has to be fast.

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/driver/SourceLoader.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/numeric/Time.h"
#include "slang/util/Bag.h"
#include "slang/util/ThreadPool.h"

#include "DesignDb.h"
#include "Extractor.h"

using namespace slang;
namespace fs = std::filesystem;

namespace {

struct Options {
    std::vector<std::string> filelists;
    std::vector<std::string> files;
    std::vector<std::string> defines;
    std::vector<std::string> includeDirs;
    std::string top;
    std::string output = "design.db";
    bool quiet = false;
    // 0 = off, -1 = every diagnostic, N > 0 = the first N. Unlimited is the
    // default for --diag: the counts above already say how many there are,
    // and a cap meant the summary sent a reader somewhere that showed a
    // fraction of them -- on a large design, 15 errors out of 829.
    int showDiags = 0;
    bool singleUnit = false;
    /// Write the elaboration log. On by default: a database says what the
    /// design is, and nothing about what could not be read to build it --
    /// and the terminal that said so is gone by the time anyone opens the
    /// file.
    bool log = true;
    /// Where to write it. Empty means beside the database, under kLogName.
    /// An explicit path is a contract: a caller that named a file usually
    /// greps it in the next step, so failing to open THAT is fatal where
    /// failing to open the default one is not.
    std::string logPath;
    /// Report how long each phase took. Which phase dominates is not
    /// guessable from the outside -- on a large design the export spends
    /// most of its time inside SQLite, not in the walk -- and knowing that
    /// is what tells an optimisation attempt where to go.
    bool timeReport = false;
};

/// Phase timer: prints on destruction so a phase is timed by scope.
struct Phase {
    const char* name;
    bool on;
    std::chrono::steady_clock::time_point t0;
    Phase(const char* name, bool on) :
        name(name), on(on), t0(std::chrono::steady_clock::now()) {}
    ~Phase() { stop(); }
    /// For a phase whose result outlives the scope it is timed in.
    void stop() {
        if (!on)
            return;
        on = false;
        std::fprintf(stderr, "[timing] %-14s %6.0f ms\n", name,
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count());
    }
};

/// The name a run's log is written under beside the database, when --log
/// does not name one of its own.
constexpr const char* kLogName = "rtldbgdb-elab.log";

/// The elaboration log: what the terminal said, kept.
///
/// Every line leads with its producer and severity, so the file answers grep
/// rather than an eye scrolling it:
///
///     slang        error: fifo.sv:39:5: error: unknown module 'ghost'
///     slang        error|     ghost #(.MODE(2)) u_g (.clk(clk));
///     slang        error|     ^~~~~
///     rtl-designdb note: 2 instantiation(s) name a module that could not ...
///
/// Three things decide that shape. The producers are apart because slang's
/// diagnostics are about the SOURCE and this tool's findings are about the
/// EXPORT -- the same line number means different things in each. The
/// severity is a column rather than a word somewhere in the sentence, so
/// `^slang  *error:` asks about severity and not about whichever message
/// happens to contain the word (slang's own text is kept verbatim, its
/// severity word included; only this tool's is lifted out). And a multi-line
/// item marks exactly one line `:` -- the one carrying the message, which is
/// not always the first (see headLine) -- and the rest `|`, so
/// `^slang  *error` returns each diagnostic WITH its source snippet while
/// `^slang  *error:` is one line per diagnostic, each naming its file and
/// what went wrong. The caret stays under the token slang put it under, the
/// prefix being constant down the block.
///
/// --quiet does not reach here. The terminal is a summary someone is
/// watching; the log is the record they read afterwards, and the two are
/// silenced by different things -- --nolog for this one.
class ElabLog {
public:
    ~ElabLog() {
        if (file)
            std::fclose(file);
    }

    /// Opens the log at `path`, false when it could not be. Says nothing
    /// either way: how bad that is depends on whether the caller named the
    /// path or the tool derived it, and only the caller knows which.
    bool open(const std::string& path) {
        at = path;
        file = std::fopen(path.c_str(), "w");
        return file != nullptr;
    }

    bool active() const { return file != nullptr; }

    /// Where it was opened, for a message that would otherwise name the
    /// default spelling of a path `--log` may have moved.
    const std::string& path() const { return at; }

    /// One item of one producer, however many lines it holds.
    void put(const char* producer, std::string_view severity,
             std::string_view text) {
        if (!file)
            return;
        const size_t head = headLine(text);
        for (size_t index = 0; !text.empty(); index++) {
            const size_t nl = text.find('\n');
            const std::string_view line = text.substr(0, nl);
            std::fprintf(file, "%-12s %.*s%c %.*s\n", producer,
                         int(severity.size()), severity.data(),
                         index == head ? ':' : '|', int(line.size()),
                         line.data());
            if (nl == std::string_view::npos)
                break;
            text.remove_prefix(nl + 1);
        }
    }

private:
    /// Which line of an item carries what a reader came for.
    ///
    /// Not always the first: slang puts the include stack and the instance
    /// path BEFORE the diagnostic, so heading on line zero made the one-line
    /// view of a recursive design read `in instance: recursion.u_chain` and
    /// name neither the file nor the error. Those two prefixes are the only
    /// ones it writes ahead of the message (TextDiagnosticClient::report), and
    /// a spelling this does not know falls back to the first line.
    static size_t headLine(std::string_view text) {
        for (size_t index = 0; !text.empty(); index++) {
            const size_t nl = text.find('\n');
            const std::string_view line = text.substr(0, nl);
            if (!line.starts_with("in file included from ") &&
                !line.starts_with("  in "))
                return index;
            if (nl == std::string_view::npos)
                break;
            text.remove_prefix(nl + 1);
        }
        return 0;
    }

    std::FILE* file = nullptr;
    std::string at;
};

/// This tool's own producer name in the log, and the one in db_info.tool.
constexpr const char* kToolName = "rtl-designdb";

std::string formatted(const char* fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n <= 0)
        return {};
    std::string out(size_t(n), '\0');
    std::vsnprintf(out.data(), size_t(n) + 1, fmt, ap);
    return out;
}

/// This tool's messages lead with their severity, which the terminal wants in
/// the sentence and the log wants in a column. Split there: the word, and the
/// message without it.
std::pair<std::string_view, std::string_view> splitSeverity(std::string_view text) {
    for (std::string_view word : {"error", "warning", "note"}) {
        if (text.size() > word.size() + 2 &&
            text.compare(0, word.size(), word) == 0 &&
            text.compare(word.size(), 2, ": ") == 0) {
            return {word, text.substr(word.size() + 2)};
        }
    }
    return {"note", text};
}

/// One of this tool's own findings: to the log always, to the terminal when
/// nothing silenced it.
void logged(ElabLog& log, bool terminal, const std::string& text) {
    const auto [severity, rest] = splitSeverity(text);
    log.put(kToolName, severity, rest);
    if (terminal)
        std::fputs(text.c_str(), stderr);
}

/// A finding: something the run went on past, so -q may keep it off the
/// terminal.
void finding(const Options& opt, ElabLog& log, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = formatted(fmt, ap);
    va_end(ap);
    logged(log, !opt.quiet, text);
}

/// A failure: the run stops after it, so --quiet does not get to hide it.
void failure(ElabLog& log, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const std::string text = formatted(fmt, ap);
    va_end(ap);
    logged(log, true, text);
}

/// Where a message should send someone who wants the diagnostics themselves.
///
/// The log already holds every one of them, so naming it beats naming an
/// option: `--diag` re-runs the whole export to print what the run just
/// wrote. It is the answer only when there is no log to send them to.
std::string diagnosticsAt(const ElabLog& log) {
    return log.active() ? "see " + log.path() : std::string("run with --diag");
}

void usage() {
    std::puts(
        "rtl-designdb — export an elaborated SystemVerilog design to SQLite\n"
        "\n"
        "usage: rtl-designdb -f <filelist> [-f ...] [options]\n"
        "\n"
        "  -f/-file <file>  VCS-style filelist (+define+, +incdir+, nested -f/-file)\n"
        "                   $VAR and ${VAR} in it are expanded from the environment\n"
        "  +define+A=B      preprocessor define (repeatable, '+'-separated like VCS)\n"
        "  +incdir+<dir>    include directory (repeatable, '+'-separated like VCS)\n"
        "  -I <dir>         include directory\n"
        "  --top <module>   top module (default: slang's own selection)\n"
        "  -o <file.db>     output database (default: design.db)\n"
        "  --single-unit    compile the whole filelist as ONE compilation unit, so a\n"
        "                   leading defines file reaches every later file (VCS and\n"
        "                   Verilator behave this way; slang defaults to per-file units)\n"
        "  --quiet          only report errors\n"
        "  --version        print the version, schema version and build revision\n"
        "  --time-report    report how long each phase took\n"
        "  --diag [N]       print elaboration diagnostics (all of them; N caps it)\n"
        "  --log <file>     write the elaboration log here instead of beside the\n"
        "                   database (relative paths resolve against the cwd, as -o's do)\n"
        "  --nolog          do not write the elaboration log\n"
        "\n"
        "Bare paths are taken as source files.\n"
        "\n"
        "Every run writes an elaboration log — rtldbgdb-elab.log beside the\n"
        "database unless --log names another path — one line per producer and\n"
        "severity so it answers grep: slang's own diagnostics under `slang`,\n"
        "this tool's findings under `rtl-designdb`, and a multi-line item's\n"
        "continuations marked `|` where its first line has `:`. --quiet quiets\n"
        "the terminal, not the log. A log named by --log that cannot be opened\n"
        "stops the run; the default one only warns.\n"
        "\n"
        "exit: 0 complete, 3 partial, 4 hierarchy only (all three wrote a\n"
        "      database, and say what db_info.analysis_status says); 2 the input\n"
        "      or the options were unusable, 1 the export itself failed --\n"
        "      neither wrote one.");
}

/// Expand `$VAR` and `${VAR}` from the environment.
///
/// Real filelists are written against a project root held in an environment
/// variable, which is how one checked-in `.f` serves every user's checkout.
/// An unset variable expands to nothing and the resulting path then fails to
/// open, which names the file that was wanted; substituting the literal `$VAR`
/// instead would report a path nobody wrote.
std::string expandVars(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '$') {
            out += in[i++];
            continue;
        }
        size_t j = i + 1;
        bool braced = j < in.size() && in[j] == '{';
        if (braced)
            j++;
        size_t start = j;
        while (j < in.size() && (std::isalnum((unsigned char)in[j]) || in[j] == '_'))
            j++;
        if (start == j) {           // a bare '$'
            out += in[i++];
            continue;
        }
        std::string name = in.substr(start, j - start);
        if (braced && j < in.size() && in[j] == '}')
            j++;
        if (const char* val = std::getenv(name.c_str()))
            out += val;
        i = j;
    }
    return out;
}

/// Split a VCS `+define+A=1+B=2` / `+incdir+a+b` token on '+'.
std::vector<std::string> splitPlus(std::string_view tok, std::string_view prefix) {
    std::vector<std::string> out;
    tok.remove_prefix(prefix.size());
    size_t start = 0;
    while (start <= tok.size()) {
        size_t plus = tok.find('+', start);
        auto piece = tok.substr(start, plus == std::string_view::npos ? plus
                                                                      : plus - start);
        if (!piece.empty())
            out.emplace_back(piece);
        if (plus == std::string_view::npos)
            break;
        start = plus + 1;
    }
    return out;
}

/// Read a filelist. Understands the subset every VCS-style `.f` actually uses:
/// comments, `+define+`, `+incdir+`, a nested `-f`, and bare paths. Relative
/// paths resolve against the filelist's own directory, which is what makes a
/// checked-in `.f` portable.
bool readFilelist(const fs::path& path, Options& opt, ElabLog& log, int depth = 0) {
    if (depth > 16) {
        failure(log, "error: filelist nesting too deep at %s\n",
                path.string().c_str());
        return false;
    }
    std::ifstream in(path);
    if (!in) {
        failure(log, "error: cannot read filelist %s\n", path.string().c_str());
        return false;
    }
    const fs::path base = path.parent_path();
    auto resolve = [&](const std::string& p) {
        fs::path q(expandVars(p));
        return q.is_absolute() ? q : (base.empty() ? q : base / q);
    };

    // A filelist may wrap, putting an option on one line and its argument on
    // the next. Tracking it across lines matters: without this a trailing `-f`
    // fell through to the unknown-option branch and its argument was taken as a
    // *source file*, so the nested list was never read and the export came back
    // empty with a zero exit status.
    enum class Pending { None, Filelist, LibFile, LibDir } pending = Pending::None;

    std::string line;
    while (std::getline(in, line)) {
        // Strip a `//` comment, but only where one can start: at the beginning
        // of a token. Erasing at the first `//` anywhere corrupts a path that
        // legitimately contains it -- a `$VAR//rel` join, a `//host/share`
        // path, a `+define+URL="http://..."`.
        for (size_t c = line.find("//"); c != std::string::npos;
             c = line.find("//", c + 1)) {
            if (c == 0 || std::isspace((unsigned char)line[c - 1])) {
                line.erase(c);
                break;
            }
        }
        // Tokenise on whitespace: a `.f` may put several entries on one line.
        size_t i = 0;
        std::vector<std::string> toks;
        while (i < line.size()) {
            while (i < line.size() && std::isspace((unsigned char)line[i])) i++;
            size_t s = i;
            while (i < line.size() && !std::isspace((unsigned char)line[i])) i++;
            if (i > s)
                toks.push_back(line.substr(s, i - s));
        }
        for (size_t t = 0; t < toks.size(); t++) {
            const std::string& tok = toks[t];
            if (tok.empty() || tok[0] == '#')
                break;
            if (pending != Pending::None) {
                const auto what = pending;
                pending = Pending::None;
                if (what == Pending::Filelist) {
                    if (!readFilelist(resolve(tok), opt, log, depth + 1))
                        return false;
                }
                else if (what == Pending::LibFile) {
                    opt.files.push_back(resolve(tok).string());
                }
                continue;   // LibDir: consumed and ignored, warned when seen
            }
            if (tok.rfind("+define+", 0) == 0) {
                for (auto& d : splitPlus(tok, "+define+"))
                    opt.defines.push_back(expandVars(d));
            }
            else if (tok.rfind("+incdir+", 0) == 0) {
                for (auto& d : splitPlus(tok, "+incdir+"))
                    opt.includeDirs.push_back(resolve(d).string());
            }
            else if (tok == "-f" || tok == "-file") {
                if (t + 1 < toks.size()) {
                    if (!readFilelist(resolve(toks[++t]), opt, log, depth + 1))
                        return false;
                }
                else {
                    pending = Pending::Filelist;
                }
            }
            else if (tok == "-v" && t + 1 >= toks.size()) {
                pending = Pending::LibFile;
            }
            else if (tok == "-y" && t + 1 >= toks.size()) {
                pending = Pending::LibDir;
                finding(opt, log, "note: -y library directories are not searched\n");
            }
            else if (tok == "-v" && t + 1 < toks.size()) {
                // A VCS library file: module definitions compiled on demand.
                // Ignoring it loses those modules, which shows up as an
                // "unknown module" error deep in a vendor PHY. slang has no
                // on-demand rule, so it goes in as an ordinary source and only
                // what is actually instantiated gets elaborated.
                opt.files.push_back(resolve(toks[++t]).string());
            }
            else if (tok == "-y" && t + 1 < toks.size()) {
                t++;    // library *directory*: needs a name-to-file rule; unused here
                finding(opt, log, "note: -y library directories are not searched\n");
            }
            else if (tok[0] == '+' || tok[0] == '-') {
                // An option this tool does not model. Skipping silently would
                // change what gets compiled without saying so.
                finding(opt, log, "note: ignoring filelist option %s\n", tok.c_str());
            }
            else {
                opt.files.push_back(resolve(tok).string());
            }
        }
    }
    // The wrap tracking above carries an option across a line ending; the end
    // of the FILE is where it has to be settled. A list ending in a bare `-f`
    // names a nested list nobody read, and taking that for a complete input
    // exported a database that reports analysis_status='complete' over a
    // truncated design -- worse than refusing the input, because nothing in
    // the file says a source is missing.
    if (pending != Pending::None) {
        const char* option = pending == Pending::Filelist ? "-f"
                             : pending == Pending::LibFile ? "-v"
                                                           : "-y";
        failure(log, "error: %s ends with %s and no argument for it\n",
                path.string().c_str(), option);
        return false;
    }
    return true;
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(); std::exit(0); }
        // Everything db_info would say about the producer, without exporting a
        // database to read it back: a release asset is downloaded on its own,
        // and the schema version is what decides whether a consumer can read
        // what this binary writes.
        else if (a == "--version") {
            std::printf("rtl-designdb %s (schema v%d, slang %s, built from %s)\n",
                        RTLDESIGNDB_VERSION, designdb::SchemaVersion,
                        RTLDESIGNDB_SLANG_TAG, RTLDESIGNDB_PRODUCER_REVISION);
            std::exit(0);
        }
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--single-unit") opt.singleUnit = true;
        else if (a == "--time-report") opt.timeReport = true;
        else if (a == "--nolog") opt.log = false;
        else if (a == "--diag") {
            opt.showDiags = -1;
            // The WHOLE token has to be a number, not just its first
            // character. Files named `8bit_alu.sv` are ordinary, and testing
            // one character consumed the next source file as the cap: atoi
            // read 8, the file was never compiled, and its module came out as
            // an unresolved instantiation with nothing saying a named source
            // had been eaten by an option.
            if (i + 1 < argc) {
                const char* n = argv[i + 1];
                bool allDigits = *n != '\0';
                for (const char* c = n; *c && allDigits; c++)
                    allDigits = std::isdigit((unsigned char)*c) != 0;
                if (allDigits)
                    opt.showDiags = std::atoi(argv[++i]);
            }
        }
        else if (a == "-f" || a == "-file") { auto v = next(a.c_str()); if (!v) return false; opt.filelists.emplace_back(v); }
        else if (a == "-o")      { auto v = next("-o");      if (!v) return false; opt.output = v; }
        else if (a == "--top")   { auto v = next("--top");   if (!v) return false; opt.top = v; }
        else if (a == "--log") {
            auto v = next("--log");
            if (!v)
                return false;
            // An empty path would be indistinguishable from not passing the
            // option at all, so `--log "$UNSET"` would quietly write the
            // default log and `--log "$UNSET" --nolog` would quietly write
            // none, both reporting success.
            if (!*v) {
                std::fprintf(stderr, "error: --log needs a path\n");
                return false;
            }
            opt.logPath = v;
        }
        else if (a == "-I")      { auto v = next("-I");      if (!v) return false; opt.includeDirs.emplace_back(v); }
        else if (a.rfind("+define+", 0) == 0) {
            for (auto& d : splitPlus(a, "+define+")) opt.defines.push_back(d);
        }
        else if (a.rfind("+incdir+", 0) == 0) {
            for (auto& d : splitPlus(a, "+incdir+")) opt.includeDirs.push_back(d);
        }
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option %s (see --help)\n", a.c_str());
            return false;
        }
        else opt.files.push_back(a);
    }
    // Order-insensitive on purpose: a last-one-wins rule would make the
    // meaning of a filelist-driven command line depend on where a wrapper
    // script happened to append its own flag.
    if (!opt.log && !opt.logPath.empty()) {
        std::fprintf(stderr, "error: --log names a file and --nolog asks for "
                             "none; pass one or the other\n");
        return false;
    }
    return true;
}

/// The option bag slang is driven with.
Bag buildOptionBag(const Options& opt) {
    Bag optionBag;

    parsing::PreprocessorOptions ppOpts;
    for (auto& d : opt.defines)
        ppOpts.predefines.push_back(d);
    for (auto& inc : opt.includeDirs)
        ppOpts.additionalIncludePaths.emplace_back(inc);
    optionBag.set(ppOpts);

    ast::CompilationOptions compOpts;
    if (!opt.top.empty())
        compOpts.topModules.emplace(opt.top);
    // Two settings that decide whether this tool works on real IP at all.
    //
    // slang short-circuits its elaboration walk once `errorLimit` errors
    // pile up and marks the compilation fatally errored; AnalysisManager
    // then returns *silently* without analysing anything. The result is a
    // database with a full hierarchy and zero dataflow, which reads like a
    // design made entirely of wires rather than like a failure. The default
    // limit is 64, and 0 means no limit.
    compOpts.errorLimit = 0;
    //
    // The errors that blow that limit on real IP are overwhelmingly
    // `MissingTimeScale`: slang declares it an error, while VCS and Questa
    // compile and simulate the same sources without comment. A vendor PHY
    // that mixes timescaled and untimescaled files is completely ordinary.
    // Supplying a default is what the language itself provides for, and the
    // exporter has no interest in timing.
    if (auto ts = TimeScale::fromString("1ns/1ps"))
        compOpts.defaultTimeScale = *ts;
    optionBag.set(compOpts);

    return optionBag;
}

/// Loads and parses every source into `trees`. False when a source that was
/// named could not be read: the export would then not be of the design that
/// was asked for, and a database that looks complete is worse than a failure.
///
/// The loader is the caller's, not this function's, and deliberately so. It owns
/// the SourceLibrary objects, and both SourceManager::FileInfo::library and
/// SyntaxTree::library keep non-owning pointers into that map -- so it has to
/// outlive the compilation, not the parse. Nothing names a library today
/// (addFiles passes none, and addSeparateUnit's empty library name resolves to
/// none), which is the only reason a loader scoped to this call would not
/// already be a use-after-free.
bool parseSources(const Options& opt, ElabLog& log, driver::SourceLoader& loader,
                  const Bag& optionBag, slang::ThreadPool& pool,
                  std::vector<std::shared_ptr<syntax::SyntaxTree>>& trees) {
    for (auto& inc : opt.includeDirs)
        loader.addSearchDirectories(inc);

    if (opt.singleUnit) {
        // One compilation unit for the whole list. Designs that put their
        // configuration in a leading `defines file need this: slang gives
        // each file its own unit by default, so those macros would not
        // reach anything after them and the design elaborates with the
        // wrong widths -- reported as "dimension requires a constant
        // range", far from the actual cause.
        loader.addSeparateUnit(opt.files, opt.includeDirs, opt.defines, "", {});
    }
    else {
        for (auto& f : opt.files)
            loader.addFiles(f);
    }

    { Phase p("parse", opt.timeReport);
      trees = loader.loadAndParseSources(optionBag, &pool); }

    if (!loader.getErrors().empty()) {
        for (auto& err : loader.getErrors())
            failure(log, "error: %s\n", err.c_str());
        return false;
    }
    return true;
}

struct DiagCounts {
    size_t errors = 0;
    size_t warnings = 0;
};

/// Counts the elaboration diagnostics and, with --diag, prints them.
///
/// Deliberately not fatal, and deliberately not suppressed. An individual
/// error costs only the construct it is on: slang records it and carries on,
/// so the enclosing scope still gets analysed and still contributes dataflow.
/// Verified against a module whose `$fopen` call slang rejects and which
/// exports 95 edges regardless.
///
/// The temptation is to silence the ones that look harmless. That is the wrong
/// trade for an exporter: some errors do mean a scope elaborated with the wrong
/// widths, and a database that quietly carries wrong connectivity is worse than
/// one that says something went wrong. So they are reported, `--diag` shows
/// them, and only the *limit* is lifted -- what must never happen is the silent
/// whole-design bail that hitting the limit would otherwise cause.
DiagCounts reportDiagnostics(const Options& opt, ElabLog& log,
                             const Diagnostics& diags,
                             SourceManager& sourceManager) {
    DiagCounts counts;
    for (auto& d : diags) {
        if (d.isError())
            counts.errors++;
        else
            counts.warnings++;
    }

    // slang's own text, rendered one diagnostic at a time so each enters the
    // log as its own item under its own severity. The log takes every one of
    // them whatever the terminal is showing -- that is the whole point of
    // having a log -- and nothing is rendered when neither reader exists.
    std::string shown;
    if (log.active() || opt.showDiags) {
        slang::DiagnosticEngine engine(sourceManager);
        auto client = std::make_shared<slang::TextDiagnosticClient>();
        engine.addClient(client);
        int rendered = 0;
        for (auto& d : diags) {
            client->clear();
            engine.issue(d);   // warnings included: one can delete a block
            const std::string text = client->getString();
            log.put("slang", d.isError() ? "error" : "warning", text);
            if (opt.showDiags < 0 || rendered++ < opt.showDiags)
                shown += text;
        }
    }

    if (counts.warnings && opt.showDiags == 0) {
        // Worth saying even though warnings are usually noise: slang marks
        // the node bad for some of them, and a bad statement takes its
        // enclosing block out of the export.
        finding(opt, log, "note: %zu elaboration warning(s); %s\n",
                counts.warnings, diagnosticsAt(log).c_str());
    }
    if (opt.showDiags)
        std::fputs(shown.c_str(), stderr);
    if (counts.errors) {
        finding(opt, log, "warning: %zu elaboration error(s); %s\n",
                counts.errors, diagnosticsAt(log).c_str());
    }
    return counts;
}

/// False when --top named something that did not elaborate as a top module.
bool checkTopElaborated(const Options& opt, ElabLog& log,
                        ast::Compilation& compilation) {
    if (opt.top.empty())
        return true;
    for (auto inst : compilation.getRoot().topInstances) {
        if (inst->name == opt.top)
            return true;
    }
    failure(log,
            "error: --top '%s' did not elaborate as a top module; "
            "check the name and that its source is in the filelist\n",
            opt.top.c_str());
    return false;
}

/// A digest of everything that decides what the export contains.
///
/// Each item carries its category and its byte length, so no run of one
/// category can be read as a run of another. Joining the lists with newlines
/// alone did not survive contact: `+incdir+examples +define+src` and
/// `+incdir+examples+src` put `src` in different lists -- one is a macro, the
/// other an include directory, and they elaborate differently -- yet both
/// flattened to the same bytes and so to the same digest. The length prefix
/// also means an item containing a newline or a colon cannot forge a boundary.
std::string configDigest(const Options& opt, ast::Compilation& compilation) {
    std::string cfg;
    auto put = [&cfg](const char* tag, std::string_view v) {
        cfg += tag;
        cfg += ':';
        cfg += std::to_string(v.size());
        cfg += ':';
        cfg += v;
        cfg += '\n';
    };
    for (auto& f : opt.files) put("file", f);
    for (auto& i : opt.includeDirs) put("incdir", i);
    for (auto& d : opt.defines) put("define", d);
    put("mode", opt.singleUnit ? "single-unit" : "multi-unit");
    for (auto inst : compilation.getRoot().topInstances) put("top", inst->name);
    put("timescale", "1ns/1ps");
    put("tool", RTLDESIGNDB_VERSION);
    put("slang", RTLDESIGNDB_SLANG_TAG);
    return designdb::digest(cfg);
}

/// How complete the export is, in one word.
///
/// The seal's value and the process's exit code are this one call, so a caller
/// branching on the return value and a caller reading `v_db_info` cannot
/// disagree about the same run.
///
/// A duplicated hierarchical path makes `partial` for the same reason a
/// skipped procedure does: the database is missing something it would
/// otherwise hold. It is not a dataflow gap but a *naming* one -- two
/// instances answer to one path, so a path lookup can resolve to the
/// wrong subtree. Only the terminal warning said so, and `-q` silenced
/// even that, which left the condition invisible to anyone holding the
/// file.
///
/// `unresolved` deliberately does not: an unresolved instantiation is a
/// black box, and a design that instantiates a vendor macro it has no
/// source for is complete as far as this tool can be. The count is
/// recorded so a consumer can decide for itself.
///
/// `hierarchy_only` has one cause, and this is it: `hasFatalErrors()`. It is
/// NOT `numScopes == 0` -- AnalysisManager::analyze() returns early only on
/// hasFatalErrors(), and otherwise it enters every compilation unit before it
/// reaches an instance, with Stats::numScopes counting those units too. A file
/// of nothing but a comment reports 1 scope, a file holding one package
/// reports 2, and main() has already refused an empty file list, so
/// numScopes == 0 could only mean the `fatal` beside it, no cause of its own.
/// A separate condition -- the analysis ran and some module got no dataflow
/// out of it -- needs a counter of its own.
///
/// stats.unanalysedInsts is that counter, and it enters here as
/// `partial` rather than `hierarchy_only` because the condition is per
/// module and the rest of the design is unaffected. It is a guard, not a
/// branch this design takes: while slang's analysis descends what the
/// template walk descends, an occurrence is stamped from an unanalysed
/// body only when the compilation is fatally errored, and `fatal` above
/// has already answered for that. See designdb::Stats for the two places
/// the descents differ and why neither is reachable in the pinned slang.
const char* analysisStatusOf(const designdb::Stats& stats, size_t numErrors,
                             bool fatal) {
    if (fatal)
        return "hierarchy_only";
    if (numErrors || stats.emptyProcedures || stats.duplicatePaths ||
        stats.truncatedCalls || stats.unanalysedInsts)
        return "partial";
    return "complete";
}

/// What the process returns.
///
/// `complete`, `partial` and `hierarchy_only` all wrote a database and are
/// worth telling apart without opening it -- a caller that wants dataflow can
/// stop at 4, one that only wants the tree cannot. The two that wrote nothing
/// are apart for the same reason: 2 is answered by fixing the invocation or
/// the sources, 1 by looking at this tool or the filesystem under it.
enum ExitCode {
    ExitComplete = 0,
    ExitFailed = 1,
    ExitBadInput = 2,
    ExitPartial = 3,
    ExitHierarchyOnly = 4,
};

/// The run's last word, to the log and not to the terminal: which database
/// it left behind, or that it left none.
///
/// A log is read beside a database, and the two can disagree -- a run that
/// fails before publishing keeps the PREVIOUS export while replacing its log
/// -- so the log ends by saying which of the two it is talking about.
int finished(ElabLog& log, const Options& opt, int code,
             const char* status = nullptr) {
    if (status)
        log.put(kToolName, "note", "wrote " + opt.output + " (" + status + ")");
    else
        log.put(kToolName, "note",
                "no database was written (exit " + std::to_string(code) + ")");
    return code;
}

int exitCodeFor(std::string_view analysisStatus) {
    if (analysisStatus == "hierarchy_only")
        return ExitHierarchyOnly;
    if (analysisStatus == "partial")
        return ExitPartial;
    return ExitComplete;
}

/// Writes the whole database to `tmpPath` and returns what went into it.
///
/// The writer is scoped to this function: when it returns, the file is closed
/// and complete, which is what makes the caller's atomic rename safe.
designdb::Stats writeDatabase(const Options& opt, const std::string& tmpPath,
                              ast::Compilation& compilation,
                              analysis::AnalysisManager& analysis,
                              SourceManager& sourceManager,
                              size_t numErrors, bool fatal) {
    designdb::Stats stats;
    designdb::Writer writer(tmpPath);
    // Every buffer the source manager actually opened, not the list that
    // was asked for. That covers globs after expansion and, more to the
    // point, headers pulled in by `include -- a `define changed in one of
    // those is exactly the case a digest exists to catch, and the filelist
    // does not change when it happens.
    //
    // Sorted by path, and not taken in the order slang hands the buffers
    // back: the source loader reads files on a thread pool, so buffer ids
    // fall in whatever order the reads finish. Interning in that order gave
    // `src_file.id` -- and every `file.src_file_id` pointing at it -- a
    // different value on every export of an unchanged design: 44 of
    // tinyriscv's 15,773 rows moved between two runs of one binary.
    //
    // The rest of the exporter already pays for this property, since it is
    // the one that lets two databases be compared at all: TemplateBuilder's
    // group key is a source location rather than a pointer so that module
    // ids do not follow an address, and `config_digest` exists so two
    // exports can be told apart on their inputs. One table opting out was
    // enough to make a whole-database diff answer "the exporter ran twice"
    // where it was asked "did the design change". Path order is also the
    // order a reader would expect to find the rows in.
    //
    // Deduplicated as well: one path can back more than one buffer -- 45 of
    // them for tinyriscv's 28 files -- and `INSERT OR IGNORE` dropped the
    // repeats only after fileDigest had re-read and re-hashed the file.
    std::vector<std::string> sourcePaths;
    for (auto id : sourceManager.getAllBuffers()) {
        auto name = sourceManager.getFullPath(id);
        if (!name.empty())
            sourcePaths.push_back(name.string());
    }
    std::sort(sourcePaths.begin(), sourcePaths.end());
    sourcePaths.erase(std::unique(sourcePaths.begin(), sourcePaths.end()),
                      sourcePaths.end());
    for (auto& path : sourcePaths) {
        // slang names its synthesized buffers `<unnamed_bufferN>`; they are
        // not files and would land as rows with no digest, which reads as
        // "a source we could not hash" rather than "not a source".
        auto digest = designdb::fileDigest(path);
        if (digest.empty())
            continue;
        writer.addSourceFile(path, digest);
    }

    { Phase p("extract+write", opt.timeReport);
      stats = designdb::extract(compilation, analysis, writer); }

    { Phase p("graph+index+views", opt.timeReport); writer.finish(); }

    // The seal is written after finish() so the data and the indexes are
    // complete before the row that says the export ran to completion exists.
    // It lands in ONE statement, outside the batch transaction: the atomic
    // rename in the caller means a consumer never sees an intermediate state
    // regardless, and this ordering is the extra defence that lets a reader
    // of the temp file tell a finished export from an abandoned one.
    designdb::DbInfoRow info;
    info.schemaVersion = designdb::SchemaVersion;
    info.tool = kToolName;
    info.toolVersion = RTLDESIGNDB_VERSION;
    info.slangVersion = RTLDESIGNDB_SLANG_TAG;
    // Which build produced this, at commit granularity. `tool_version` alone
    // cannot answer it: the edge dedup key and the seal both changed while
    // the version string stayed 0.1.0, so two databases agreeing on
    // tool_version, slang_version and config_digest could still have been
    // written by exporters that disagree.
    info.producerRevision = RTLDESIGNDB_PRODUCER_REVISION;
    // The *elaborated* tops, not the --top argument: slang picks tops even
    // when none is asked for, and a consumer mounting the database against a
    // waveform needs the name either way. Space-separated when the design
    // elaborates several; empty when it elaborates none, which the column
    // holds as NULL.
    for (auto inst : compilation.getRoot().topInstances) {
        if (!info.top.empty())
            info.top += ' ';
        info.top += inst->name;
    }
    info.analysisStatus = analysisStatusOf(stats, numErrors, fatal);
    info.errorCount = int64_t(numErrors);
    info.unresolvedCount = stats.unresolved;
    info.emptyProcedureCount = stats.emptyProcedures;
    info.duplicatePathCount = stats.duplicatePaths;
    // A recursive hierarchy is stamped one level deep and the rest of the
    // tree is simply absent, which `hierarchy_only` does not say -- it says
    // there is no dataflow, not that the tree is a PREFIX. On stderr alone
    // `-q` silenced it, and two databases of one design, one cut and one
    // whole, read alike to anyone holding the files.
    info.recursionCount = stats.recursiveInstances;
    // These two choose `partial` while every other count is zero, so without
    // them a consumer is told the export is incomplete and given nothing to
    // look at. The status must agree with the counts beside it, an agreement
    // the schema's CHECK holds.
    info.truncatedCallCount = stats.truncatedCalls;
    info.unanalysedInstCount = stats.unanalysedInsts;
    // Not a cause of `partial`: a checker is a construct this tool does not
    // model, not a walk that fell short. Published so its absence can be read
    // rather than guessed at.
    info.checkerInstCount = stats.checkerInsts;
    info.configDigest = configDigest(opt, compilation);
    writer.setDbInfo(info);

    return stats;
}

/// Removes the half-written temp database if the export does not finish.
/// Without it a failed run leaves a stray `design.db.tmp` beside the good
/// database -- which reads as a second, broken export rather than as a run
/// that did not finish.
struct TempGuard {
    const std::string& path;
    bool armed = true;
    ~TempGuard() {
        if (armed) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }
};

/// Atomically replaces `output` with the finished export, so a crash
/// mid-export never leaves a partial database under the real name.
///
/// `rename` is specified to behave as POSIX rename(), which replaces an
/// existing destination; MSVC implements it with MOVEFILE_REPLACE_EXISTING, so
/// the replace itself is portable. What is *not* portable is replacing a
/// destination another process holds open: POSIX unlinks it happily, Windows
/// refuses. That is a real failure a user meets by leaving the database open in
/// a viewer, so it is reported rather than thrown -- the message has to say
/// which file and why, and a filesystem_error's what() does not.
bool publish(ElabLog& log, const std::string& tmpPath, const std::string& output) {
    std::error_code ec;
    std::filesystem::rename(tmpPath, output, ec);
    if (ec) {
        failure(log,
                "error: could not replace '%s' with the finished export: %s\n"
                "       the previous database is untouched; if it is open in "
                "another program, close it and retry\n",
                output.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

/// What the run found, and what it could not.
void reportStats(const Options& opt, ElabLog& log,
                 const designdb::Stats& stats) {
    if (!opt.quiet) {
        std::printf("%s: %lld modules, %lld instances, %lld nets, %lld terminals, "
                    "%lld connections, %lld statements, %lld dependencies\n",
                    opt.output.c_str(), (long long)stats.modules,
                    (long long)stats.instances, (long long)stats.nets,
                    (long long)stats.terms, (long long)stats.conns,
                    (long long)stats.stmts, (long long)stats.deps);
    }
    if (stats.emptyProcedures) {
        finding(opt, log,
                "warning: %lld procedure(s) drive a signal but yielded no "
                "dataflow; a statement in them was rejected and its whole "
                "block skipped -- %s\n",
                (long long)stats.emptyProcedures, diagnosticsAt(log).c_str());
    }
    if (stats.unresolved) {
        finding(opt, log,
                "note: %lld instantiation(s) name a module that could not "
                "be resolved; recorded as unresolved tree nodes\n",
                (long long)stats.unresolved);
    }
    if (stats.anonymous) {
        finding(opt, log,
                "note: %lld instantiation(s) carry no instance name; "
                "each holds a synthesised $def$n path segment rather "
                "than its parent's name. A module instantiation must be "
                "named, so a macro may not have expanded\n",
                (long long)stats.anonymous);
    }
    if (stats.external) {
        finding(opt, log,
                "note: %lld reference(s) to symbols outside their own "
                "module (hierarchical, interface or package items); "
                "those written as a path are recorded in hier_ref\n",
                (long long)stats.external);
    }
    if (stats.truncatedCalls) {
        finding(opt, log,
                "warning: %lld call site(s) exceeded the "
                "subroutine expansion budget; their bodies were "
                "not walked, so dataflow through them is "
                "incomplete\n",
                (long long)stats.truncatedCalls);
    }
    if (stats.duplicatePaths) {
        finding(opt, log,
                "warning: %lld instances share a hierarchical path with "
                "another; the design did not fully elaborate, so a path "
                "lookup may be ambiguous\n",
                (long long)stats.duplicatePaths);
    }
    if (stats.recursiveInstances) {
        finding(opt, log,
                "warning: %lld instance(s) re-enter a module that is "
                "already one of their own ancestors; the instantiation "
                "is infinitely recursive, so the tree stops there -- %s\n",
                (long long)stats.recursiveInstances,
                diagnosticsAt(log).c_str());
    }
    if (stats.unanalysedBodies && !stats.unanalysedInsts) {
        // Only worth saying when the templates are the whole of it. When
        // occurrences inherited the gap the warning below says so, and the
        // fatally-errored run that produces it has already been reported.
        finding(opt, log,
                "note: %lld module body group(s) had no analysed body, "
                "so the templates built from them hold no procedure; "
                "nothing is stamped from them, and no row is missing\n",
                (long long)stats.unanalysedBodies);
    }
    if (stats.unanalysedInsts) {
        finding(opt, log,
                "warning: %lld of %lld instance(s) were stamped from a "
                "module body the analysis never reached; their procedures "
                "are absent, so they carry hierarchy and connections and "
                "no procedural dataflow\n",
                (long long)stats.unanalysedInsts,
                (long long)stats.stampedBodies);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return ExitBadInput;
    // Opened before anything can fail with something worth recording, and by
    // default beside the database rather than beside the sources: the pair is
    // what a consumer holds, and the log answers for the run that wrote that
    // file.
    //
    // A path the caller named is a contract -- a pipeline step that greps it
    // next reads an empty result as "no errors" -- so failing to open it
    // stops the run, here, before an export has cost anything or replaced
    // the database a previous run left. The derived path is a convenience,
    // and losing it is not a reason to lose the export.
    ElabLog log;
    if (opt.log) {
        const std::string at =
            opt.logPath.empty()
                ? (fs::path(opt.output).parent_path() / kLogName).string()
                : opt.logPath;
        if (!log.open(at)) {
            if (!opt.logPath.empty()) {
                failure(log, "error: cannot write the log %s\n", at.c_str());
                return ExitBadInput;
            }
            std::fprintf(stderr, "warning: could not write %s; the export "
                                 "continues without a log\n", at.c_str());
        }
    }
    for (auto& f : opt.filelists) {
        if (!readFilelist(f, opt, log))
            return finished(log, opt, ExitBadInput);
    }
    if (opt.files.empty()) {
        failure(log, "error: no source files (pass -f <filelist> or paths)\n");
        return finished(log, opt, ExitBadInput);
    }

    try {
        SourceManager sourceManager;
        const Bag optionBag = buildOptionBag(opt);

        // Both the parser and the analysis manager take a thread pool and
        // run serially without one, which is what they were doing: slang is
        // built with threading on, and neither was being given a pool. The
        // parser splits per file (so a single compilation unit stays
        // serial), the analysis manager per scope.
        auto pool = std::make_shared<slang::ThreadPool>();
        // Outlives the compilation on purpose -- see parseSources.
        driver::SourceLoader loader(sourceManager);
        std::vector<std::shared_ptr<syntax::SyntaxTree>> trees;
        if (!parseSources(opt, log, loader, optionBag, *pool, trees))
            return finished(log, opt, ExitBadInput);

        ast::Compilation compilation(optionBag);
        for (auto& tree : trees)
            compilation.addSyntaxTree(tree);

        // Forces elaboration. `getRoot()` alone is not enough — the analysis
        // manager checks `isElaborated()`, which only getSemanticDiagnostics()
        // sets. The diagnostics are collected to be counted and, with --diag,
        // printed -- not to gate the export: a design that does not fully
        // elaborate still exports what it has, which is what makes the tool
        // usable mid-bringup.
        // getAllDiagnostics, not getSemanticDiagnostics: the latter excludes
        // *parse* errors, which are the ones that matter most here. A file with
        // a syntax error is recovered from by the parser and its salvaged
        // fragments are exported, so without this a malformed source produced a
        // database indistinguishable from a correct one, silently.
        Phase elab("elaborate", opt.timeReport);
        auto& diags = compilation.getAllDiagnostics();
        elab.stop();
        const DiagCounts counts = reportDiagnostics(opt, log, diags, sourceManager);

        if (!checkTopElaborated(opt, log, compilation))
            return finished(log, opt, ExitBadInput);

        // Checked before analysing, not inferred afterwards. slang sets this on
        // three conditions -- the error limit exceeded, instantiation deeper
        // than maxInstanceDepth (128), or an infinitely recursive hierarchy --
        // and `AnalysisManager::analyze()` then returns without a word. Reading
        // it directly is the difference between saying why the dataflow is
        // missing and guessing from an empty result.
        const bool fatal = compilation.hasFatalErrors();
        if (fatal) {
            finding(opt, log,
                    "warning: the compilation is fatally errored, so no dataflow "
                    "can be analysed; the database holds hierarchy only.\n"
                    "         %s -- it says why (too many errors, instantiation "
                    "deeper than 128, or a recursive hierarchy)\n",
                    diagnosticsAt(log).c_str());
        }

        analysis::AnalysisManager analysis({}, pool);
        // The analysis contract: the compilation is frozen while the manager's
        // worker threads read it, and unfrozen after, because extraction still
        // elaborates lazily. slang's own driver does exactly this pair; a
        // Release build never noticed the missing half because the check is
        // an assert (AnalysisManager.cpp, "compilation.isFrozen()").
        compilation.freeze();
        { Phase p("analyze", opt.timeReport); analysis.analyze(compilation); }
        compilation.unfreeze();
        // Informational only. What the analysis actually yielded per module
        // is not knowable here -- it is counted during extraction and
        // reported by reportStats below.
        if (!opt.quiet) {
            auto astats = analysis.getStats();
            std::fprintf(stderr, "analysis: %zu scopes, %zu procedures, %.1f MB\n",
                         astats.numScopes, astats.numProcedures,
                         astats.memoryUsage / 1e6);
        }

        const std::string tmpPath = opt.output + ".tmp";
        TempGuard tempGuard{tmpPath};

        const designdb::Stats stats =
            writeDatabase(opt, tmpPath, compilation, analysis, sourceManager,
                          counts.errors, fatal);
        // The writer is destroyed with writeDatabase's frame, so the database
        // file is closed and complete before this runs.
        if (!publish(log, tmpPath, opt.output))
            return finished(log, opt, ExitFailed);
        tempGuard.armed = false;

        reportStats(opt, log, stats);
        const char* status = analysisStatusOf(stats, counts.errors, fatal);
        return finished(log, opt, exitCodeFor(status), status);
    }
    catch (const std::exception& e) {
        failure(log, "error: %s\n", e.what());
        return finished(log, opt, ExitFailed);
    }
}
