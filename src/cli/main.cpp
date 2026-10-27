// arctl -- command-line front end for adaptive-restore.
//
// Commands: checkpoint, inspect, restore, profile, benchmark, visualize.

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "adaptive_restore/cli/commands.hpp"
#include "adaptive_restore/common/version.hpp"

namespace {

int usage(std::ostream& os) {
    // Every option below is taken from the command's own parser. If a flag is
    // added there, add it here; a help text that overstates the CLI is worse
    // than no help text.
    os << "arctl " << adaptive_restore::kProjectVersion
       << " -- adaptive region-aware process restoration\n\n"
          "usage: arctl <command> [options]\n\n"
          "  checkpoint --output <image> [--verify] [--env K=V]\n"
          "             [--allow-unsupported-state] -- <command> [args...]\n"
          "      Spawn <command>, run it to its ar_checkpoint_here() marker and write\n"
          "      the image. Its stdout/stderr go to <image>.stdout / <image>.stderr.\n\n"
          "  inspect    --image <image>\n"
          "      Print the image header, mappings, regions and descriptors.\n\n"
          "  restore    --image <image> [--policy eager|lazy|prefetch|cost]\n"
          "             [--profile <db>] [--verify] [--record-trace <path>]\n"
          "             [--cold-cache|--warm-cache] [--no-learn]\n"
          "      Rebuild the image into a new process and resume it. Policy defaults\n"
          "      to eager; prefetch and cost additionally require --profile. Under a\n"
          "      deferring policy arctl stays alive as the target's page server.\n\n"
          "  profile    show|reset|export --image <image> --profile <db>\n"
          "             [--output <csv>]\n"
          "      Inspect or clear learned region history. export requires --output.\n\n"
          "  benchmark  --suite <file> --output <results-dir>\n"
          "      Run a workload suite over all four policies; writes raw.tsv.\n\n"
          "  visualize  [--image <image> [--profile <db>] [--policy P]]\n"
          "             [--trace <faults.tsv>] [--results <dir|raw.tsv>]\n"
          "             [--width N] [--no-color] [--cold-cache]\n"
          "      Draw the region-map plan (--image), the demand-fault timeline\n"
          "      (--trace) or the policy comparison (--results); at least one of the\n"
          "      three. P is eager|lazy|prefetch|cost, default cost.\n\n"
          "  help, --help, -h    this text          --version    print the version\n";
    return 2;
}

#ifndef AR_HAVE_COMPEL
int no_restore_support() {
    std::cerr << "arctl: 'restore' is unavailable: this build was configured without compel.\n"
                 "       Install criu/compel and reconfigure to enable it.\n";
    return ENOSYS;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) return usage(std::cerr);

    const std::string_view cmd = args.front();
    if (cmd == "-h" || cmd == "--help" || cmd == "help") return usage(std::cout), 0;
    if (cmd == "--version") { std::cout << adaptive_restore::kProjectVersion << '\n'; return 0; }

    const std::span<const std::string_view> rest(args.data() + 1, args.size() - 1);
    if (cmd == "checkpoint") return adaptive_restore::cli::cmd_checkpoint(rest);
    if (cmd == "inspect") return adaptive_restore::cli::cmd_inspect(rest);

    if (cmd == "restore") {
#ifdef AR_HAVE_COMPEL
        return adaptive_restore::cli::cmd_restore(rest);
#else
        return no_restore_support();
#endif
    }
    if (cmd == "profile") {
#ifdef AR_HAVE_SQLITE
        return adaptive_restore::cli::cmd_profile(rest);
#else
        std::cerr << "arctl: 'profile' is unavailable: this build was configured without "
                     "SQLite.\n";
        return ENOSYS;
#endif
    }
    if (cmd == "benchmark") return adaptive_restore::cli::cmd_benchmark(rest);
    if (cmd == "visualize") return adaptive_restore::cli::cmd_visualize(rest);

    std::cerr << "arctl: unknown command '" << cmd << "'\n\n";
    return usage(std::cerr);
}
