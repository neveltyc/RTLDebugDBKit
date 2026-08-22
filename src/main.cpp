// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// rtl-designdb — read a VCS-style filelist, elaborate, write a queryable
// design database.
//
// Deliberately small. It takes a filelist and defines, the way `vcs -f` does,
// and nothing else: configuration, project layout and output formatting belong
// to whatever drives it, and this binary does the one job that has to be fast.

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
    // and a cap meant "run with --diag to see them" showed a fraction of
    // them -- on a large design, 15 errors out of 829.
    int showDiags = 0;
    bool singleUnit = false;
    /// Report how long each phase took. Which phase dominates is not
    /// guessable from the outside -- on a large design the export spends
    /// most of its time inside SQLite, not in the walk -- and knowing that
    /// is what tells an optimisation attempt where to go.
    bool timing = false;
    /// Keep the enum-domain CHECK clauses in the schema. Off by default:
    /// see Writer's constructor for why.
    bool checkConstraints = false;
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
        "  -q               only report errors\n"
        "  --timing         report how long each phase took\n"
        "  --check-constraints  keep enum CHECK clauses in the schema (slower;\n"
        "                   verify-designdb.py checks the same domains anyway)\n"
        "  --diag [N]       print elaboration diagnostics (all of them; N caps it)\n"
        "\n"
        "Bare paths are taken as source files.");
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
bool readFilelist(const fs::path& path, Options& opt, int depth = 0) {
    if (depth > 16) {
        std::fprintf(stderr, "error: filelist nesting too deep at %s\n",
                     path.string().c_str());
        return false;
    }
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "error: cannot read filelist %s\n", path.string().c_str());
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
                    if (!readFilelist(resolve(tok), opt, depth + 1))
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
                    if (!readFilelist(resolve(toks[++t]), opt, depth + 1))
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
                std::fprintf(stderr, "note: -y library directories are not searched\n");
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
                std::fprintf(stderr, "note: -y library directories are not searched\n");
            }
            else if (tok[0] == '+' || tok[0] == '-') {
                // An option this tool does not model. Skipping silently would
                // change what gets compiled without saying so.
                std::fprintf(stderr, "note: ignoring filelist option %s\n", tok.c_str());
            }
            else {
                opt.files.push_back(resolve(tok).string());
            }
        }
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
        else if (a == "-q") opt.quiet = true;
        else if (a == "--single-unit") opt.singleUnit = true;
        else if (a == "--timing") opt.timing = true;
        else if (a == "--check-constraints") opt.checkConstraints = true;
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
bool parseSources(const Options& opt, driver::SourceLoader& loader,
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

    { Phase p("parse", opt.timing);
      trees = loader.loadAndParseSources(optionBag, &pool); }

    if (!loader.getErrors().empty()) {
        for (auto& err : loader.getErrors())
            std::fprintf(stderr, "error: %s\n", err.c_str());
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
DiagCounts reportDiagnostics(const Options& opt, const Diagnostics& diags,
                             SourceManager& sourceManager) {
    DiagCounts counts;
    for (auto& d : diags) {
        if (d.isError())
            counts.errors++;
        else
            counts.warnings++;
    }

    if (counts.warnings && !opt.quiet && opt.showDiags == 0) {
        // Worth saying even though warnings are usually noise: slang marks
        // the node bad for some of them, and a bad statement takes its
        // enclosing block out of the export.
        std::fprintf(stderr, "note: %zu elaboration warning(s); --diag shows them\n",
                     counts.warnings);
    }
    if (opt.showDiags) {
        slang::DiagnosticEngine engine(sourceManager);
        auto client = std::make_shared<slang::TextDiagnosticClient>();
        engine.addClient(client);
        int shown = 0;
        for (auto& d : diags) {
            if (opt.showDiags > 0 && shown++ >= opt.showDiags)
                break;
            engine.issue(d);   // warnings included: one can delete a block
        }
        std::fputs(client->getString().c_str(), stderr);
    }
    if (counts.errors && !opt.quiet) {
        std::fprintf(stderr,
                     "warning: %zu elaboration error(s); run with --diag to see them\n",
                     counts.errors);
    }
    return counts;
}

/// False when --top named something that did not elaborate as a top module.
bool checkTopElaborated(const Options& opt, ast::Compilation& compilation) {
    if (opt.top.empty())
        return true;
    for (auto inst : compilation.getRoot().topInstances) {
        if (inst->name == opt.top)
            return true;
    }
    std::fprintf(stderr,
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
    designdb::Writer writer(tmpPath, opt.checkConstraints);
    writer.setMeta("schema_version", std::to_string(designdb::SchemaVersion));
    writer.setMeta("tool", "rtl-designdb");
    // The *elaborated* tops, not the --top argument: slang picks tops even
    // when none is asked for, and a consumer mounting the database against
    // a waveform needs the name either way. Space-separated when the design
    // elaborates several -- the case that previously wrote nothing at all.
    {
        std::string tops;
        for (auto inst : compilation.getRoot().topInstances) {
            if (!tops.empty())
                tops += ' ';
            tops += inst->name;
        }
        if (!tops.empty())
            writer.setMeta("top", tops);
    }
    // Every buffer the source manager actually opened, not the list that
    // was asked for. That covers globs after expansion and, more to the
    // point, headers pulled in by `include -- a `define changed in one of
    // those is exactly the case a digest exists to catch, and the filelist
    // does not change when it happens.
    for (auto id : sourceManager.getAllBuffers()) {
        auto name = sourceManager.getFullPath(id);
        if (name.empty())
            continue;
        auto path = name.string();
        // slang names its synthesized buffers `<unnamed_bufferN>`; they are
        // not files and would land as rows with no digest, which reads as
        // "a source we could not hash" rather than "not a source".
        auto digest = designdb::fileDigest(path);
        if (digest.empty())
            continue;
        writer.addSourceFile(path, digest);
    }

    { Phase p("extract+write", opt.timing);
      stats = designdb::extract(compilation, analysis, writer); }

    { Phase p("index+views", opt.timing); writer.finish(); }

    // Status and versions are written after finish() so the data and
    // indexes are complete before the meta seal lands.  setMeta()
    // operates outside the batch transaction, so it works after
    // finish(); and the atomic rename in the caller means the consumer
    // never sees an intermediate state regardless -- this ordering is an
    // extra defence so that a reader of the temp file can tell whether
    // the export ran to completion.
    //
    // A duplicated hierarchical path makes `partial` for the same reason a
    // skipped procedure does: the database is missing something it would
    // otherwise hold. It is not a dataflow gap but a *naming* one -- two
    // instances answer to one path, so a path lookup can resolve to the
    // wrong subtree. Only the terminal warning said so, and `-q` silenced
    // even that, which left the condition invisible to anyone holding the
    // file.
    //
    // `unresolved` deliberately does not: an unresolved instantiation is a
    // black box, and a design that instantiates a vendor macro it has no
    // source for is complete as far as this tool can be. The count is
    // recorded so a consumer can decide for itself.
    //
    // `hierarchy_only` has one cause, and this is it. It used to read
    // `fatal || numScopes == 0`, which said there were two --
    // AnalysisManager::analyze() returns early only on hasFatalErrors(),
    // and otherwise it enters every compilation unit before it reaches an
    // instance, with Stats::numScopes counting those units too. A file of
    // nothing but a comment reports 1 scope, a file holding one package
    // reports 2, and main() has already refused an empty file list, so
    // numScopes == 0 could only mean the `fatal` beside it. The second
    // disjunct never chose anything, and the condition it was standing in
    // for -- the analysis ran and some module got no dataflow out of it --
    // had no test anywhere, nor a counter to build one from.
    //
    // stats.unanalysedInsts is that counter, and it enters here as
    // `partial` rather than `hierarchy_only` because the condition is per
    // module and the rest of the design is unaffected. It is a guard, not a
    // branch this design takes: while slang's analysis descends what the
    // template walk descends, an occurrence is stamped from an unanalysed
    // body only when the compilation is fatally errored, and `fatal` above
    // has already answered for that. See designdb::Stats for the two places
    // the descents differ and why neither is reachable in the pinned slang.
    const char* analysisStatus;
    if (fatal)
        analysisStatus = "hierarchy_only";
    else if (numErrors || stats.emptyProcedures || stats.duplicatePaths ||
             stats.truncatedCalls || stats.unanalysedInsts)
        analysisStatus = "partial";
    else
        analysisStatus = "complete";
    writer.setMeta("analysis_status", analysisStatus);
    writer.setMeta("error_count", std::to_string(numErrors));
    writer.setMeta("unresolved_count", std::to_string(stats.unresolved));
    writer.setMeta("empty_procedure_count", std::to_string(stats.emptyProcedures));
    writer.setMeta("duplicate_path_count", std::to_string(stats.duplicatePaths));
    writer.setMeta("tool_version", RTLDESIGNDB_VERSION);
    writer.setMeta("slang_version", RTLDESIGNDB_SLANG_TAG);
    // Which build produced this, at commit granularity. `tool_version`
    // alone cannot answer it: the edge dedup key and the meta seal both
    // changed while the version string stayed 0.1.0, so two databases
    // agreeing on tool_version, slang_version and config_digest could
    // still have been written by exporters that disagree.
    writer.setMeta("producer_revision", RTLDESIGNDB_PRODUCER_REVISION);
    writer.setMeta("config_digest", configDigest(opt, compilation));

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
bool publish(const std::string& tmpPath, const std::string& output) {
    std::error_code ec;
    std::filesystem::rename(tmpPath, output, ec);
    if (ec) {
        std::fprintf(stderr,
                     "error: could not replace '%s' with the finished export: %s\n"
                     "       the previous database is untouched; if it is open in "
                     "another program, close it and retry\n",
                     output.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

/// What the run found, and what it could not.
void reportStats(const Options& opt, const designdb::Stats& stats) {
    if (opt.quiet)
        return;
    std::printf("%s: %lld modules, %lld instances, %lld nets, %lld terminals, "
                "%lld connections, %lld statements, %lld dependencies\n",
                opt.output.c_str(), (long long)stats.modules,
                (long long)stats.instances, (long long)stats.nets,
                (long long)stats.terms, (long long)stats.conns,
                (long long)stats.stmts, (long long)stats.deps);
    if (stats.emptyProcedures) {
        std::fprintf(stderr,
                     "warning: %lld procedure(s) drive a signal but yielded no "
                     "dataflow; a statement in them was rejected and its whole "
                     "block skipped -- run with --diag\n",
                     (long long)stats.emptyProcedures);
    }
    if (stats.unresolved) {
        std::fprintf(stderr,
                     "note: %lld instantiation(s) name a module that could not "
                     "be resolved; recorded as unresolved tree nodes\n",
                     (long long)stats.unresolved);
    }
    if (stats.external) {
        std::fprintf(stderr,
                     "note: %lld reference(s) to symbols outside their own "
                     "module (hierarchical, interface or package items); "
                     "those written as a path are recorded in hier_ref\n",
                     (long long)stats.external);
    }
    if (stats.truncatedCalls) {
        std::fprintf(stderr,
                     "warning: %lld call site(s) exceeded the "
                     "subroutine expansion budget; their bodies were "
                     "not walked, so dataflow through them is "
                     "incomplete\n",
                     (long long)stats.truncatedCalls);
    }
    if (stats.duplicatePaths) {
        std::fprintf(stderr,
                     "warning: %lld instances share a hierarchical path with "
                     "another; the design did not fully elaborate, so a path "
                     "lookup may be ambiguous\n",
                     (long long)stats.duplicatePaths);
    }
    if (stats.unanalysedBodies && !stats.unanalysedInsts) {
        // Only worth saying when the templates are the whole of it. When
        // occurrences inherited the gap the warning below says so, and the
        // fatally-errored run that produces it has already been reported.
        std::fprintf(stderr,
                     "note: %lld module body group(s) had no analysed body, "
                     "so the templates built from them hold no procedure; "
                     "nothing is stamped from them, and no row is missing\n",
                     (long long)stats.unanalysedBodies);
    }
    if (stats.unanalysedInsts) {
        std::fprintf(stderr,
                     "warning: %lld of %lld instance(s) were stamped from a "
                     "module body the analysis never reached; their procedures "
                     "are absent, so they carry hierarchy and connections and "
                     "no procedural dataflow\n",
                     (long long)stats.unanalysedInsts,
                     (long long)stats.instances);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt))
        return 2;
    for (auto& f : opt.filelists) {
        if (!readFilelist(f, opt))
            return 2;
    }
    if (opt.files.empty()) {
        std::fprintf(stderr, "error: no source files (pass -f <filelist> or paths)\n");
        return 2;
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
        if (!parseSources(opt, loader, optionBag, *pool, trees))
            return 2;

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
        Phase elab("elaborate", opt.timing);
        auto& diags = compilation.getAllDiagnostics();
        elab.stop();
        const DiagCounts counts = reportDiagnostics(opt, diags, sourceManager);

        if (!checkTopElaborated(opt, compilation))
            return 2;

        // Checked before analysing, not inferred afterwards. slang sets this on
        // three conditions -- the error limit exceeded, instantiation deeper
        // than maxInstanceDepth (128), or an infinitely recursive hierarchy --
        // and `AnalysisManager::analyze()` then returns without a word. Reading
        // it directly is the difference between saying why the dataflow is
        // missing and guessing from an empty result.
        const bool fatal = compilation.hasFatalErrors();
        if (fatal) {
            std::fprintf(stderr,
                         "warning: the compilation is fatally errored, so no dataflow "
                         "can be analysed; the database holds hierarchy only.\n"
                         "         --diag says why (too many errors, instantiation "
                         "deeper than 128, or a recursive hierarchy)\n");
        }

        analysis::AnalysisManager analysis({}, pool);
        { Phase p("analyze", opt.timing); analysis.analyze(compilation); }
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
        if (!publish(tmpPath, opt.output))
            return 1;
        tempGuard.armed = false;

        reportStats(opt, stats);
        return 0;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
