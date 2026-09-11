#include "adaptive_restore/checkpoint/unsupported_state.hpp"

#include <limits.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

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
        // Pending-but-undelivered signals are dropped on the floor by a
        // checkpoint: the image has nowhere to put them, so the restored
        // process never receives what the original was about to.
        else if ((key == "SigPnd" || key == "ShdPnd") && nonzero_mask())
            out.reasons.push_back(std::string("pending signals (") + key + " " + value +
                                  "); they would never be delivered to the restored process");
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

    // POSIX interval timers are kernel state with no memory footprint, so
    // nothing in the image hints they existed; a restored process simply never
    // fires them again.
    {
        std::ifstream timers("/proc/" + std::to_string(pid) + "/timers");
        std::string l;
        int n = 0;
        while (std::getline(timers, l))
            if (l.rfind("ID:", 0) == 0) ++n;
        if (n > 0)
            out.reasons.push_back(std::to_string(n) +
                                  " POSIX interval timer(s); they are not captured and would "
                                  "never fire again");
    }

    // File locks live in the kernel against the opening fd. The fd collector
    // reopens regular files by path, which silently drops any lock held on
    // them: the restored process would believe it holds a lock it does not.
    {
        std::ifstream locks("/proc/locks");
        std::string l;
        int n = 0;
        while (std::getline(locks, l)) {
            std::istringstream ls(l);
            std::vector<std::string> tok;
            std::string t;
            while (ls >> t) tok.push_back(t);
            // "-> " marks a blocked waiter; the columns are otherwise the same.
            std::size_t base = (!tok.empty() && tok[0] == "->") ? 1 : 0;
            if (tok.size() < base + 5) continue;
            if (tok[base + 4] == std::to_string(pid)) ++n;
        }
        if (n > 0)
            out.reasons.push_back(std::to_string(n) +
                                  " file lock(s) held; descriptors are reopened by path on "
                                  "restore, which does not carry the lock with them");
    }

    // A target that unshared a namespace is no longer in arctl's, and the
    // restored process would silently land back in arctl's.
    {
        for (const char* ns : {"cgroup", "ipc", "mnt", "net", "pid", "time", "user", "uts"}) {
            const std::string theirs = read_link("/proc/" + std::to_string(pid) + "/ns/" + ns);
            const std::string ours = read_link(std::string("/proc/self/ns/") + ns);
            if (!theirs.empty() && !ours.empty() && theirs != ours)
                out.reasons.push_back(std::string("a ") + ns +
                                      " namespace different from arctl's; the restored process "
                                      "would run in arctl's instead");
        }
    }

    return out;
}

}  // namespace adaptive_restore::checkpoint
