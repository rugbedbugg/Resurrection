// arctl -- command-line front end for adaptive-restore.
//
// Command surface follows docs/design-draft-v2.md §18. Every subcommand is
// currently a stub returning ENOSYS; they are filled in phase by phase (§20):
//   Phase 1: checkpoint, inspect  (implemented)
//   Phase 2: restore --policy eager
//   Phase 3: restore --policy lazy
//   Phase 4: restore --policy prefetch|cost, profile show/reset
//   Phase 5: benchmark, visualize

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
    os << "arctl " << adaptive_restore::kProjectVersion << " -- adaptive region-aware process restoration\n\n"
       << "usage: arctl <command> [options]\n\n"
       << "commands:\n"
       << "  checkpoint [--verify] [--env K=V] --output <image> -- <cmd> [args...]\n"
       << "  restore    --image <image> --policy eager|lazy|prefetch|cost [--profile <db>]\n"
       << "  inspect    --image <image>\n"
       << "  profile    show|reset --image <image>\n"
       << "  benchmark  --suite <suite.yaml> --output <results-dir>\n"
       << "  visualize  --run <run-id>\n\n"
       << "common options: --region-size --prefetch-budget --cold-cache --seed\n"
       << "                --disable-learning --record-trace\n";
    return 2;
}

int not_implemented(std::string_view cmd) {
    std::cerr << "arctl: '" << cmd << "' is not implemented yet (" << std::strerror(ENOSYS) << ")\n";
    return ENOSYS;
}

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

    if (cmd == "restore" || cmd == "profile" || cmd == "benchmark" || cmd == "visualize")
        return not_implemented(cmd);

    std::cerr << "arctl: unknown command '" << cmd << "'\n\n";
    return usage(std::cerr);
}
