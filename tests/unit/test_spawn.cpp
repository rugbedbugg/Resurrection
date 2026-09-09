#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/checkpoint/maps.hpp"
#include "adaptive_restore/checkpoint/spawn.hpp"
#include "adaptive_restore/common/error.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::checkpoint;

// Third field of /proc/<pid>/stat is the state, but comm (field 2) is
// parenthesised and may contain spaces, so scan from the last ')'.
static char proc_state(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/stat";
    FILE* f = std::fopen(path.c_str(), "r");
    assert(f);
    std::string text;
    char chunk[512];
    while (std::size_t n = std::fread(chunk, 1, sizeof chunk, f)) text.append(chunk, n);
    std::fclose(f);
    const std::size_t close_paren = text.rfind(')');
    assert(close_paren != std::string::npos);
    return text[close_paren + 2];
}

static std::uint64_t tagged_start(const std::vector<Mapping>& v, const char* tag) {
    for (const auto& m : v)
        if (m.tag == tag) return m.start;
    return 0;
}

int main() {
    // --- spawn, stop at the marker, resume, verify the target completed -----
    char out_tmpl[] = "/tmp/arspawnXXXXXX";
    const int out_fd = ::mkstemp(out_tmpl);
    assert(out_fd >= 0);

    auto t = spawn_to_marker({HELPER_PATH}, EnvPolicy{}, out_fd, out_fd);
    assert(t.pid > 0);
    assert(t.argv.size() == 1 && t.argv[0] == HELPER_PATH);

    const char state = proc_state(t.pid);
    assert((state == 't' || state == 'T') && "target must be stopped at the marker");

    // Stopped at the marker means the increment has not run yet, so nothing has
    // been printed. Resuming must produce the post-marker value.
    const auto maps_first = read_maps(t.pid);
    assert(!maps_first.empty());

    ::ptrace(PTRACE_CONT, t.pid, nullptr, nullptr);
    int st = 0;
    assert(::waitpid(t.pid, &st, 0) == t.pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    ::lseek(out_fd, 0, SEEK_SET);
    char buf[64] = {};
    const ssize_t n = ::read(out_fd, buf, sizeof buf - 1);
    assert(n > 0);
    assert(std::strstr(buf, "x=42") && "target must resume past the marker");
    ::close(out_fd);
    ::unlink(out_tmpl);

    // --- ASLR must be off: two runs land at identical addresses -------------
    const int devnull = ::open("/dev/null", O_WRONLY);
    assert(devnull >= 0);
    auto t2 = spawn_to_marker({HELPER_PATH}, EnvPolicy{}, devnull, devnull);
    const auto maps_second = read_maps(t2.pid);

    assert(tagged_start(maps_first, "[stack]") != 0);
    assert(tagged_start(maps_first, "[stack]") == tagged_start(maps_second, "[stack]"));
    assert(tagged_start(maps_first, "[heap]") == tagged_start(maps_second, "[heap]"));
    assert(maps_first.size() == maps_second.size());
    for (std::size_t i = 0; i < maps_first.size(); ++i)
        assert(maps_first[i].start == maps_second[i].start &&
               maps_first[i].end == maps_second[i].end);

    ::ptrace(PTRACE_CONT, t2.pid, nullptr, nullptr);
    assert(::waitpid(t2.pid, &st, 0) == t2.pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    ::close(devnull);

    // --- a target that cannot be executed must be reported, not hang --------
    bool threw = false;
    try {
        spawn_to_marker({"/nonexistent/ar-no-such-binary"}, EnvPolicy{}, -1, -1);
    } catch (const CheckpointError&) {
        threw = true;
    }
    assert(threw && "unexecutable target must throw CheckpointError");

    // --- the launch digest is deterministic and input sensitive -------------
    const auto d1 = launch_digest({"a", "b"}, {"PATH=/bin"});
    const auto d2 = launch_digest({"a", "b"}, {"PATH=/bin"});
    const auto d3 = launch_digest({"a", "b"}, {"PATH=/usr/bin"});
    const auto d4 = launch_digest({"ab"}, {"PATH=/bin"});
    assert(d1 == d2);
    assert(d1 != d3);
    assert(d1 != d4 && "NUL separators must keep argv boundaries significant");

    // The curated environment is exactly what the policy asked for.
    EnvPolicy only_extra;
    only_extra.passthrough_names.clear();
    only_extra.extra = {"AR_TEST=1"};
    const auto env = build_env(only_extra);
    assert(env.size() == 1 && env[0] == "AR_TEST=1");

    std::puts("test_spawn: ok");
    return 0;
}
