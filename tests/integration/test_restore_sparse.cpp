// Phase 3 exit gate, second half (§20): lazy restoration must load only the
// pages a sparse workload actually accesses. The first half (correctness parity
// with eager) is test_restore_mini_lazy.
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace std::string_literals;

static int run(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    return (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
}

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Pull "<n> " out of a summary line, e.g. "faults     25 served".
static long field_before(const std::string& log, const std::string& marker) {
    const auto at = log.find(marker);
    if (at == std::string::npos) return -1;
    auto end = log.rfind(' ', at);
    while (end != std::string::npos && log[end] == ' ') --end;
    auto begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(log[begin - 1]))) --begin;
    return std::atol(log.c_str() + begin);
}

int main() {
    const std::string base = "/tmp/ar_sparse_" + std::to_string(::getpid());
    const std::string img = base + ".arimg";
    const std::string out = img + ".stdout";

    assert(run(ARCTL_PATH + " checkpoint --verify --output "s + img + " -- " + SPARSE_PATH +
               " > " + base + ".log 2>&1") == 0);
    const std::string baseline = slurp(out);
    // Assert the shape, not the numbers: hardcoding "touched=16 of 512" broke
    // the moment the workload was resized for the benchmark, which is a change
    // to the experiment's scale and not to what this test checks.
    assert(!baseline.empty());
    const auto at = baseline.find("touched=");
    assert(at != std::string::npos && "the sparse workload must report what it touched");
    const long touched = std::atol(baseline.c_str() + at + 8);
    const auto of = baseline.find(" of ", at);
    assert(of != std::string::npos);
    const long total = std::atol(baseline.c_str() + of + 4);
    assert(touched > 0 && total > 0 && touched * 4 < total &&
           "the workload must touch only a small fraction, or it is not sparse");

    const auto restore = [&](const std::string& policy) {
        { std::ofstream f(out, std::ios::trunc); }
        const std::string log = base + "." + policy + ".log";
        assert(run(ARCTL_PATH + " restore --image "s + img + " --policy " + policy + " > " + log +
                   " 2>&1") == 0);
        return std::pair{slurp(out), slurp(log)};
    };

    const auto [eager_out, eager_log] = restore("eager");
    const auto [lazy_out, lazy_log] = restore("lazy");

    // Correctness first: the policy must not change what the program computes.
    assert(eager_out == baseline);
    assert(lazy_out == baseline);

    // The gate: lazy must load dramatically less than it registers.
    const long registered = field_before(lazy_log, "pages across");
    const long served = field_before(lazy_log, "served (");
    const long undemanded = field_before(lazy_log, "of " + std::to_string(registered) +
                                                       " deferred pages");
    assert(registered > 100 && "the sparse workload should register a large region");
    assert(served > 0 && "a resuming process must fault on something");
    assert(served * 4 < registered &&
           "lazy must serve far fewer pages than it registers on a sparse workload");
    assert(undemanded == registered - served);

    // And it must beat eager on pages actually written.
    const long eager_written = field_before(eager_log, "written,");
    assert(eager_written > served &&
           "eager writes the whole region; lazy must touch less than that");

    std::printf("  sparse: eager wrote %ld pages; lazy served %ld of %ld registered\n",
                eager_written, served, registered);

    for (const char* suffix : {".arimg", ".arimg.stdout", ".arimg.stderr", ".log", ".eager.log",
                               ".lazy.log"})
        ::unlink((base + suffix).c_str());

    std::puts("test_restore_sparse: ok");
    return 0;
}
