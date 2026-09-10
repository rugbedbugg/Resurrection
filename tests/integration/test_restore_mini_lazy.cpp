// Phase 3 correctness parity (§20 Phase 3, §25.2, §25.3): lazy restoration must
// pass the same correctness bar as eager, and produce byte-identical output.
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace std::string_literals;

// Established in Phase 1; a restore under any policy must reproduce it exactly.
static const char* kBaseline = "sum=11791276921336954880\n";

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

static void truncate_file(const std::string& p) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
}

// Restore under `policy`, returning what the restored process wrote. The
// target's stdout is cleared first, so a stale value cannot make this pass.
static std::string restore_and_capture(const std::string& base, const std::string& img,
                                       const std::string& policy, const std::string& extra,
                                       std::string* log_out) {
    const std::string out = img + ".stdout";
    truncate_file(out);
    assert(slurp(out).empty());

    const std::string log = base + "." + policy + ".log";
    const int rc = run(ARCTL_PATH + " restore --image "s + img + " --policy " + policy + " " +
                       extra + " > " + log + " 2>&1");
    assert(rc == 0 && "arctl restore must succeed");
    if (log_out) *log_out = slurp(log);
    return slurp(out);
}

int main() {
    const std::string base = "/tmp/ar_lazy_mini_" + std::to_string(::getpid());
    const std::string img = base + ".arimg";

    assert(run(ARCTL_PATH + " checkpoint --verify --output "s + img + " -- " + MINI_PATH +
               " > " + base + ".log 2>&1") == 0);
    assert(slurp(img + ".stdout") == kBaseline);

    // --- eager and lazy must agree, byte for byte ---------------------------
    std::string eager_log, lazy_log;
    const std::string eager_out = restore_and_capture(base, img, "eager", "--verify", &eager_log);
    const std::string lazy_out = restore_and_capture(base, img, "lazy", "", &lazy_log);

    assert(eager_out == kBaseline && "eager restore must reproduce the baseline");
    assert(lazy_out == kBaseline && "lazy restore must reproduce the baseline");
    assert(eager_out == lazy_out && "the restoration policy must not change the result");

    assert(eager_log.find("target     exited 0") != std::string::npos);
    assert(lazy_log.find("target     exited 0") != std::string::npos);

    // --- lazy must actually defer work, not quietly behave like eager -------
    assert(lazy_log.find(" lazy") != std::string::npos);
    assert(lazy_log.find("registered regions") != std::string::npos);
    assert(lazy_log.find("never demanded") != std::string::npos &&
           "a workload that touches only part of its memory must leave pages undemanded");

    // --- repeated restore from the same image (§25.2) -----------------------
    const std::string again = restore_and_capture(base, img, "lazy", "", nullptr);
    assert(again == kBaseline && "an image must be restorable more than once");

    // --- the fault trace is the touched set ---------------------------------
    const std::string trace = base + ".trace.tsv";
    restore_and_capture(base, img, "lazy", "--record-trace " + trace, nullptr);
    {
        std::ifstream f(trace);
        std::string line;
        assert(std::getline(f, line) && line.rfind("vaddr\tregion", 0) == 0);
        int rows = 0;
        while (std::getline(f, line))
            if (!line.empty()) ++rows;
        assert(rows > 0 && "a resuming process must fault on something");
    }

    // --- an unknown policy is refused, not silently treated as eager --------
    assert(run(ARCTL_PATH + " restore --image "s + img + " --policy prefetch > /dev/null 2>&1") != 0);

    for (const char* suffix : {".arimg", ".arimg.stdout", ".arimg.stderr", ".log", ".eager.log",
                               ".lazy.log", ".trace.tsv"})
        ::unlink((base + suffix).c_str());

    std::puts("test_restore_mini_lazy: ok");
    return 0;
}
