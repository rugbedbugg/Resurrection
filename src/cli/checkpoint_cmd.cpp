#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "adaptive_restore/checkpoint/capture.hpp"
#include "adaptive_restore/checkpoint/spawn.hpp"
#include "adaptive_restore/cli/commands.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_writer.hpp"

namespace adaptive_restore::cli {

namespace {

int fail(const std::string& msg) {
    std::cerr << "arctl checkpoint: " << msg << '\n';
    return 1;
}

// Redirect the target's stdout/stderr into files beside the image, so its
// output is captured and its fd table holds regular files rather than whatever
// terminal or pipe arctl was launched with.
// Owner-only, like the image: the target's output is as private as its memory.
int open_stream(const std::string& path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::fchmod(fd, 0600);  // O_TRUNC on a leftover file keeps its old mode
    return fd;
}

}  // namespace

int cmd_checkpoint(std::span<const std::string_view> args) {
    std::string output = "checkpoint.arimg";
    bool verify = false;
    bool allow_unsupported = false;
    checkpoint::EnvPolicy policy;
    std::vector<std::string> target_argv;

    std::size_t i = 0;
    for (; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--") {
            ++i;
            break;
        }
        if (a == "--verify") {
            verify = true;
        } else if (a == "--allow-unsupported-state") {
            allow_unsupported = true;
        } else if (a == "--output" || a == "--env") {
            if (i + 1 >= args.size()) return fail(std::string(a) + " needs a value");
            const std::string value(args[++i]);
            if (a == "--output")
                output = value;
            else
                policy.extra.push_back(value);
        } else {
            return fail("unknown option '" + std::string(a) + "'");
        }
    }
    for (; i < args.size(); ++i) target_argv.emplace_back(args[i]);

    if (target_argv.empty())
        return fail("no target given; use: arctl checkpoint [--verify] [--env K=V] "
                    "--output <image> [--allow-unsupported-state] -- <command> [args...]");

    const std::string out_path = output + ".stdout";
    const std::string err_path = output + ".stderr";
    const int out_fd = open_stream(out_path);
    if (out_fd < 0) return fail("cannot create " + out_path + ": " + std::strerror(errno));
    const int err_fd = open_stream(err_path);
    if (err_fd < 0) return fail("cannot create " + err_path + ": " + std::strerror(errno));

    pid_t target = -1;
    try {
        const auto spawned = checkpoint::spawn_to_marker(target_argv, policy, out_fd, err_fd);
        target = spawned.pid;

        image::ImageWriter writer;
        checkpoint::CaptureOptions opts;
        opts.verify = verify;
        opts.allow_unsupported_state = allow_unsupported;
        const auto res = checkpoint::capture(
            target, checkpoint::launch_digest(spawned.argv, spawned.envp), opts, writer);

        if (verify && res.verify_mismatches > 0) {
            std::cerr << "arctl checkpoint: " << res.verify_mismatches
                      << " page(s) did not match on re-read; image not written\n";
            ::ptrace(PTRACE_KILL, target, nullptr, nullptr);
            int st = 0;
            ::waitpid(target, &st, 0);
            return 1;
        }

        writer.finish(output);

        std::cout << "checkpoint written: " << output << '\n'
                  << "  mappings   " << res.mappings_saved << " saved / " << res.mappings_total
                  << " total\n"
                  << "  regions    " << res.regions_created << '\n'
                  << "  pages      " << res.pages_saved << " (" << res.zero_pages << " zero, "
                  << res.file_pages << " from file)\n"
                  << "  page data  " << res.bytes_saved << " bytes stored\n"
                  << "  fds        " << res.fds_saved << '\n'
                  << "  verify     " << (verify ? "0 mismatches" : "skipped") << '\n';

        // Phase 1 captures without disturbing the run, so let the target finish.
        ::ptrace(PTRACE_CONT, target, nullptr, nullptr);
        int st = 0;
        if (::waitpid(target, &st, 0) == target && WIFEXITED(st))
            std::cout << "  target     exited " << WEXITSTATUS(st) << '\n';
    } catch (const std::exception& e) {
        if (target > 0) {
            ::ptrace(PTRACE_KILL, target, nullptr, nullptr);
            int st = 0;
            ::waitpid(target, &st, 0);
        }
        return fail(e.what());
    }

    ::close(out_fd);
    ::close(err_fd);
    return 0;
}

}  // namespace adaptive_restore::cli
