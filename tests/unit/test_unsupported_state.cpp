#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <string>

#include "adaptive_restore/checkpoint/spawn.hpp"
#include "adaptive_restore/checkpoint/unsupported_state.hpp"

using namespace adaptive_restore::checkpoint;

// Spawn the helper in the given mode, stopped at its marker, and report what
// the guard makes of it.
static UnsupportedState inspect(const char* kind) {
    const int devnull = ::open("/dev/null", O_RDWR);
    auto t = spawn_to_marker({STATEFUL_PATH, kind}, EnvPolicy{}, devnull, devnull);
    const auto found = find_unsupported_state(t.pid);
    ::ptrace(PTRACE_CONT, t.pid, nullptr, nullptr);
    int st = 0;
    ::waitpid(t.pid, &st, 0);
    ::close(devnull);
    return found;
}

static bool mentions(const UnsupportedState& s, const char* needle) {
    for (const auto& r : s.reasons)
        if (r.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    // A plain single-threaded process with default signal state must pass, or
    // the guard would refuse every ordinary workload.
    const auto clean = inspect("clean");
    assert(clean.empty() && "an ordinary target must not be refused");

    // Blocked signals: the restored process would start with none blocked.
    const auto blocked = inspect("blocked");
    assert(!blocked.empty() && mentions(blocked, "blocked signals"));

    // An installed handler: the address is captured as memory, the disposition
    // pointing at it is not.
    const auto handler = inspect("handler");
    assert(!handler.empty() && mentions(handler, "signal handlers"));

    // A working directory the restored process would not inherit.
    const auto moved = inspect("chdir");
    assert(!moved.empty() && mentions(moved, "working directory"));

    // The message must name what was found, not just that something was.
    const std::string text = blocked.describe();
    assert(text.find("SigBlk") != std::string::npos);
    assert(text.find("silently inherit") != std::string::npos &&
           "the refusal should explain why it matters, not just that it happened");

    std::puts("test_unsupported_state: ok");
    return 0;
}
