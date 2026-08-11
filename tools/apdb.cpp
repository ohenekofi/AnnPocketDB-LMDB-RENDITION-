// AnnPocketDb offline maintenance CLI.
//
//   apdb dump   <db> <out.apdump>
//   apdb load   <dump.apdump> <db> [--force]
//   apdb verify <db>
//   apdb repair <src-db> <dst-db> [--force]
//
#include <cstdlib>
#include <iostream>
#include <string>

#include "annpocketdb/backup/maintenance.hpp"

namespace {

void usage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " dump   <db-path> <out.apdump>\n"
        << "  " << argv0 << " load   <dump.apdump> <db-path> [--force]\n"
        << "  " << argv0 << " verify <db-path>\n"
        << "  " << argv0 << " repair <src-db> <dst-db> [--force]\n";
}

int cmdDump(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }
    auto r = annpocketdb::backup::dumpFile(argv[2], argv[3]);
    if (!r) {
        std::cerr << "dump failed: " << r.status().toString() << "\n";
        return 1;
    }
    std::cout << "dumped " << r.value().entries << " entries\n";
    return 0;
}

int cmdLoad(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }
    annpocketdb::backup::LoadOptions opts;
    for (int i = 4; i < argc; ++i) {
        if (std::string(argv[i]) == "--force") opts.forceOverwrite = true;
    }
    auto r = annpocketdb::backup::loadFromFile(argv[2], argv[3], opts);
    if (!r) {
        std::cerr << "load failed: " << r.status().toString() << "\n";
        return 1;
    }
    std::cout << "loaded " << r.value().entries << " entries\n";
    return 0;
}

int cmdVerify(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 2;
    }
    auto r = annpocketdb::backup::verifyFile(argv[2]);
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

int cmdRepair(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }
    annpocketdb::backup::RepairOptions opts;
    for (int i = 4; i < argc; ++i) {
        if (std::string(argv[i]) == "--force") opts.forceOverwrite = true;
    }
    auto r = annpocketdb::backup::repair(argv[2], argv[3], opts);
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
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "dump") return cmdDump(argc, argv);
    if (cmd == "load") return cmdLoad(argc, argv);
    if (cmd == "verify") return cmdVerify(argc, argv);
    if (cmd == "repair") return cmdRepair(argc, argv);
    usage(argv[0]);
    return 2;
}
