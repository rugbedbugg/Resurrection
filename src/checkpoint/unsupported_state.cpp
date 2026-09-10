#include "adaptive_restore/checkpoint/unsupported_state.hpp"

#include <limits.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::checkpoint {

namespace {

std::string read_link(const std::string& path) {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink(path.c_str(), buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
}

}  // namespace

std::string UnsupportedState::describe() const {
    std::ostringstream out;
    out << "the target holds state this checkpoint cannot represent:";
    for (const auto& r : reasons) out << "\n  - " << r;
    out << "\n\nNone of it is in the image format, so a restored process would "
           "silently inherit\narctl's values instead of the target's. Refusing "
           "rather than producing a checkpoint\nthat restores into a subtly "
           "different process.";
    return out.str();
}

UnsupportedState find_unsupported_state(pid_t pid) {
    UnsupportedState out;

    const std::string status = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream f(status);
    if (!f) throw CheckpointError("capture: cannot read " + status);

    std::string line;
    while (std::getline(f, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());

        // Signal masks and dispositions are hex bitmaps; anything non-zero is a
        // handler or a block that a restored process would not have.
        const auto nonzero_mask = [&value]() {
            return value.find_first_not_of("0") != std::string::npos;
        };
        if (key == "SigBlk" && nonzero_mask())
            out.reasons.push_back("blocked signals (SigBlk " + value +
                                  "); the restored process would start with none blocked");
        else if (key == "SigIgn" && nonzero_mask())
            out.reasons.push_back("ignored signals (SigIgn " + value + ")");
        else if (key == "SigCgt" && nonzero_mask())
            out.reasons.push_back("installed signal handlers (SigCgt " + value +
                                  "); handler addresses are captured as memory but the "
                                  "dispositions that point at them are not");
        else if (key == "Threads" && value != "1")
            out.reasons.push_back("multiple threads (" + value +
                                  "); Tier 1 captures only the leader's registers");
        else if (key == "Seccomp" && value != "0")
            out.reasons.push_back("a seccomp filter (mode " + value + ")");
        else if (key == "NoNewPrivs" && value != "0")
            out.reasons.push_back("no_new_privs is set");
    }

    // umask is inherited from arctl, so a target that changed it would create
    // files with different permissions after restore.
    {
        std::ifstream st2(status);
        std::string l;
        std::string target_umask, our_umask;
        while (std::getline(st2, l))
            if (l.rfind("Umask:", 0) == 0) target_umask = l.substr(6);
        std::ifstream st3("/proc/self/status");
        while (std::getline(st3, l))
            if (l.rfind("Umask:", 0) == 0) our_umask = l.substr(6);
        if (!target_umask.empty() && target_umask != our_umask)
            out.reasons.push_back("a umask (" + target_umask + ") different from arctl's (" +
                                  our_umask + ")");
    }

    // Resource limits are inherited too. Compared rather than checked against
    // defaults, since "default" depends on how arctl itself was launched.
    {
        const auto read_limits = [](const std::string& p) {
            std::ifstream f2(p);
            std::ostringstream ss;
            ss << f2.rdbuf();
            return ss.str();
        };
        const std::string theirs = read_limits("/proc/" + std::to_string(pid) + "/limits");
        const std::string ours = read_limits("/proc/self/limits");
        if (!theirs.empty() && !ours.empty() && theirs != ours)
            out.reasons.push_back("resource limits differing from arctl's");
    }

    // A restored process inherits arctl's cwd, so a target that moved has
    // relative paths that would resolve differently.
    const std::string target_cwd = read_link("/proc/" + std::to_string(pid) + "/cwd");
    const std::string our_cwd = read_link("/proc/self/cwd");
    if (!target_cwd.empty() && !our_cwd.empty() && target_cwd != our_cwd)
        out.reasons.push_back("a working directory (" + target_cwd +
                              ") different from arctl's (" + our_cwd + ")");

    return out;
}

}  // namespace adaptive_restore::checkpoint
