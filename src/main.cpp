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
    int showDiags = 0;   // print the first N elaboration diagnostics
    bool singleUnit = false;
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
        "  --diag [N]       print the first N elaboration diagnostics (default 20)\n"
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
        else if (a == "--diag") {
            opt.showDiags = 20;
            if (i + 1 < argc && std::isdigit((unsigned char)argv[i + 1][0]))
                opt.showDiags = std::atoi(argv[++i]);
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

        driver::SourceLoader loader(sourceManager);
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

        auto trees = loader.loadAndParseSources(optionBag);
        if (!loader.getErrors().empty()) {
            // A source that was named and could not be read means the export is
            // not of the design that was asked for. Continuing would produce a
            // database that looks complete, so this fails instead.
            for (auto& err : loader.getErrors())
                std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }

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
        auto& diags = compilation.getAllDiagnostics();
        size_t numErrors = 0;
        size_t numWarnings = 0;
        for (auto& d : diags) {
            if (d.isError())
                numErrors++;
            else
                numWarnings++;
        }
        if (numWarnings && !opt.quiet && opt.showDiags == 0) {
            // Worth saying even though warnings are usually noise: slang marks
            // the node bad for some of them, and a bad statement takes its
            // enclosing block out of the export.
            std::fprintf(stderr, "note: %zu elaboration warning(s); --diag shows them\n",
                         numWarnings);
        }
        if (opt.showDiags) {
            slang::DiagnosticEngine engine(sourceManager);
            auto client = std::make_shared<slang::TextDiagnosticClient>();
            engine.addClient(client);
            int shown = 0;
            for (auto& d : diags) {
                if (shown++ >= opt.showDiags)
                    break;
                engine.issue(d);   // warnings included: one can delete a block
            }
            std::fputs(client->getString().c_str(), stderr);
        }
        if (numErrors && !opt.quiet) {
            // Deliberately not fatal, and deliberately not suppressed.
            //
            // An individual error costs only the construct it is on: slang
            // records it and carries on, so the enclosing scope still gets
            // analysed and still contributes dataflow. Verified against a
            // module whose `$fopen` call slang rejects and which exports 95
            // edges regardless.
            //
            // The temptation is to silence the ones that look harmless. That is
            // the wrong trade for an exporter: some errors do mean a scope
            // elaborated with the wrong widths, and a database that quietly
            // carries wrong connectivity is worse than one that says something
            // went wrong. So they are reported, `--diag` shows them, and only
            // the *limit* is lifted -- what must never happen is the silent
            // whole-design bail that hitting the limit would otherwise cause.
            std::fprintf(stderr,
                         "warning: %zu elaboration error(s); run with --diag to see them\n",
                         numErrors);
        }

        // Checked before analysing, not inferred afterwards. slang sets this on
        // three conditions -- the error limit exceeded, instantiation deeper
        // than maxInstanceDepth (128), or an infinitely recursive hierarchy --
        // and `AnalysisManager::analyze()` then returns without a word. Reading
        // it directly is the difference between saying why the dataflow is
        // missing and guessing from an empty result.
        if (!opt.top.empty()) {
            bool found = false;
            for (auto inst : compilation.getRoot().topInstances)
                found = found || inst->name == opt.top;
            if (!found) {
                std::fprintf(stderr,
                             "error: --top '%s' did not elaborate as a top module; "
                             "check the name and that its source is in the filelist\n",
                             opt.top.c_str());
                return 2;
            }
        }

        const bool fatal = compilation.hasFatalErrors();
        if (fatal) {
            std::fprintf(stderr,
                         "warning: the compilation is fatally errored, so no dataflow "
                         "can be analysed; the database holds hierarchy only.\n"
                         "         --diag says why (too many errors, instantiation "
                         "deeper than 128, or a recursive hierarchy)\n");
        }

        analysis::AnalysisManager analysis;
        analysis.analyze(compilation);
        auto astats = analysis.getStats();
        if (!fatal && astats.numScopes == 0) {
            // analyze() returns silently when the compilation is fatally
            // errored, so this is the only place the condition is visible.
            std::fprintf(stderr, "warning: no scopes were analysed; the database "
                                 "will have hierarchy but no dataflow\n");
        }
        else if (!opt.quiet) {
            std::fprintf(stderr, "analysis: %zu scopes, %zu procedures, %.1f MB\n",
                         astats.numScopes, astats.numProcedures,
                         astats.memoryUsage / 1e6);
        }

        const std::string tmpPath = opt.output + ".tmp";
        designdb::Stats stats;
        {
        designdb::Writer writer(tmpPath);
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

        stats = designdb::extract(compilation, analysis, writer);

        const char* analysisStatus;
        if (fatal || astats.numScopes == 0)
            analysisStatus = "hierarchy_only";
        else if (numErrors || stats.emptyProcedures)
            analysisStatus = "partial";
        else
            analysisStatus = "complete";
        writer.setMeta("analysis_status", analysisStatus);
        writer.setMeta("error_count", std::to_string(numErrors));
        writer.setMeta("warning_count", std::to_string(numWarnings));
        writer.setMeta("unresolved_count", std::to_string(stats.unresolved));
        writer.setMeta("empty_procedure_count", std::to_string(stats.emptyProcedures));
        writer.setMeta("tool_version", RTLDESIGNDB_VERSION);
        writer.setMeta("slang_version", RTLDESIGNDB_SLANG_TAG);
        {
            std::string cfg;
            for (auto& f : opt.files) { cfg += f; cfg += '\n'; }
            for (auto& d : opt.defines) { cfg += d; cfg += '\n'; }
            for (auto& i : opt.includeDirs) { cfg += i; cfg += '\n'; }
            cfg += opt.singleUnit ? "single-unit\n" : "multi-unit\n";
            for (auto inst : compilation.getRoot().topInstances) {
                cfg += inst->name;
                cfg += '\n';
            }
            cfg += "1ns/1ps\n";
            cfg += RTLDESIGNDB_VERSION "\n";
            cfg += RTLDESIGNDB_SLANG_TAG "\n";
            writer.setMeta("config_digest", designdb::digest(cfg));
        }

        writer.finish();
        }
        // Writer destroyed — the database file is closed and complete.
        // Atomic rename replaces the target so a crash mid-export never
        // leaves a partial database under the real name.
        std::filesystem::rename(tmpPath, opt.output);

        if (!opt.quiet) {
            std::printf("%s: %lld modules, %lld instances, %lld symbols, %lld edges, "
                        "%lld assignments, %lld children, %lld ports\n",
                        opt.output.c_str(), (long long)stats.modules,
                        (long long)stats.instances, (long long)stats.symbols, (long long)stats.edges,
                        (long long)stats.assignments, (long long)stats.children,
                        (long long)stats.ports);
            if (stats.stmtReads) {
                std::fprintf(stderr,
                             "note: %lld read(s) belong to a statement that writes "
                             "nothing this module can name (an assertion, or a "
                             "write to a signal outside it); see stmt_read\n",
                             (long long)stats.stmtReads);
            }
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
                             "be resolved; recorded with a null child_module\n",
                             (long long)stats.unresolved);
            }
            if (stats.external) {
                std::fprintf(stderr,
                             "note: %lld reference(s) to symbols outside their own "
                             "module (hierarchical, interface or package items); "
                             "those written as a path are recorded in hier_ref\n",
                             (long long)stats.external);
            }
            if (stats.duplicatePaths) {
                std::fprintf(stderr,
                             "warning: %lld instances share a hierarchical path with "
                             "another; the design did not fully elaborate, so a path "
                             "lookup may be ambiguous\n",
                             (long long)stats.duplicatePaths);
            }
        }
        return 0;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
