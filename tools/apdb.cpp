// AnnPocketDb offline maintenance CLI.
//
//   apdb dump   <db> <out.apdump>
//   apdb load   <dump.apdump> <db> [--force]
//   apdb verify <db>
//   apdb repair <src-db> <dst-db> [--force]
//
// Paths may contain any Unicode characters. On Windows the narrow argv is in
// the ANSI code page (lossy), so the arguments are re-read as UTF-16 from
// GetCommandLineW() and carried as UTF-8 internally; console output is
// switched to UTF-8 for the duration of the run so error messages that echo
// a path display correctly.
//
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "annpocketdb/backup/maintenance.hpp"
#include "annpocketdb/util/path.hpp"

// After the project headers, so windows.h macros can't leak into them.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>  // CommandLineToArgvW
#endif

namespace {

using Args = std::vector<std::string>;  // UTF-8 on Windows, raw bytes on POSIX

#if defined(_WIN32)
std::string wideToUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// Restores the console output code page on exit (it belongs to the console,
// not this process, so it would otherwise outlive us).
struct Utf8Console {
    UINT prev = GetConsoleOutputCP();
    Utf8Console() { SetConsoleOutputCP(CP_UTF8); }
    ~Utf8Console() { SetConsoleOutputCP(prev); }
};
#endif

Args collectArgs(int argc, char** argv) {
    Args args;
#if defined(_WIN32)
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 0; i < wargc; ++i) args.push_back(wideToUtf8(wargv[i]));
        LocalFree(wargv);
        return args;
    }
#endif
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    return args;
}

// Arguments that the Windows path above produced are valid UTF-8 by
// construction, so this does not throw there.
std::filesystem::path toPath(const std::string& arg) {
    return annpocketdb::util::pathFromUtf8(arg);
}

bool hasFlag(const Args& args, std::size_t from, const char* flag) {
    for (std::size_t i = from; i < args.size(); ++i) {
        if (args[i] == flag) return true;
    }
    return false;
}

void usage(const std::string& argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " dump   <db-path> <out.apdump>\n"
        << "  " << argv0 << " load   <dump.apdump> <db-path> [--force]\n"
        << "  " << argv0 << " verify <db-path>\n"
        << "  " << argv0 << " repair <src-db> <dst-db> [--force]\n"
        << "\n"
        << "Paths may contain non-ASCII (Unicode) characters on all platforms.\n";
}

int cmdDump(const Args& args) {
    if (args.size() < 4) {
        usage(args[0]);
        return 2;
    }
    auto r = annpocketdb::backup::dumpFile(toPath(args[2]), toPath(args[3]));
    if (!r) {
        std::cerr << "dump failed: " << r.status().toString() << "\n";
        return 1;
    }
    std::cout << "dumped " << r.value().entries << " entries\n";
    return 0;
}

int cmdLoad(const Args& args) {
    if (args.size() < 4) {
        usage(args[0]);
        return 2;
    }
    annpocketdb::backup::LoadOptions opts;
    opts.forceOverwrite = hasFlag(args, 4, "--force");
    auto r = annpocketdb::backup::loadFromFile(toPath(args[2]), toPath(args[3]), opts);
    if (!r) {
        std::cerr << "load failed: " << r.status().toString() << "\n";
        return 1;
    }
    std::cout << "loaded " << r.value().entries << " entries\n";
    return 0;
}

int cmdVerify(const Args& args) {
    if (args.size() < 3) {
        usage(args[0]);
        return 2;
    }
    auto r = annpocketdb::backup::verifyFile(toPath(args[2]));
    if (!r) {
        std::cerr << "verify failed: " << r.status().toString() << "\n";
        return 1;
    }
    const auto& rep = r.value();
    std::cout << "pages scanned:   " << rep.pagesScanned << "\n"
              << "leaf entries:    " << rep.leafEntries << "\n"
              << "branch entries:  " << rep.branchEntries << "\n"
              << "overflow pages:  " << rep.overflowPages << "\n"
              << "free-list pages: " << rep.freeListPages << "\n"
              << "max tree depth:  " << rep.treeDepthMax << "\n"
              << "issues:          " << rep.issues.size() << "\n";
    for (const auto& issue : rep.issues) {
        std::cout << "  page " << issue.pageId << ": " << issue.message << "\n";
    }
    return rep.ok() ? 0 : 1;
}

int cmdRepair(const Args& args) {
    if (args.size() < 4) {
        usage(args[0]);
        return 2;
    }
    annpocketdb::backup::RepairOptions opts;
    opts.forceOverwrite = hasFlag(args, 4, "--force");
    auto r = annpocketdb::backup::repair(toPath(args[2]), toPath(args[3]), opts);
    if (!r) {
        std::cerr << "repair failed: " << r.status().toString() << "\n";
        return 1;
    }
    const auto& rep = r.value();
    std::cout << "salvaged: " << rep.salvaged << "\n"
              << "skipped:  " << rep.skipped << "\n";
    for (const auto& n : rep.notes) std::cout << "  note: " << n << "\n";
    if (!rep.sourceVerify.ok()) {
        std::cout << "source had " << rep.sourceVerify.issues.size() << " verify issues\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    Utf8Console utf8Console;
#endif
    const Args args = collectArgs(argc, argv);
    if (args.size() < 2) {
        usage(args.empty() ? std::string("apdb") : args[0]);
        return 2;
    }
    const std::string& cmd = args[1];
    if (cmd == "dump") return cmdDump(args);
    if (cmd == "load") return cmdLoad(args);
    if (cmd == "verify") return cmdVerify(args);
    if (cmd == "repair") return cmdRepair(args);
    usage(args[0]);
    return 2;
}
