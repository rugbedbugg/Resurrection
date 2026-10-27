// Regression for the prefetch observation bug.
//
// Before: `--policy prefetch` on a fresh profile registered every region as
// StaticPrefetch, wrote nothing (no history to write), served thousands of real
// demand faults, and then discarded every one of them because observe_regions()
// inferred observability from the REQUESTED action. The run learned nothing and
// the next run was just as blind.
//
// After: a prefetch that writes nothing is reported as FullyDeferred, so its
// faults are evidence and reach the profile.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace {
std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
int sh(const std::string& c) { return std::system(c.c_str()); }

// "<key>   <number> ..." -> number
long after(const std::string& log, const std::string& key) {
    const auto at = log.find(key);
    return at == std::string::npos ? -1 : std::atol(log.c_str() + at + key.size());
}
}  // namespace

int main() {
    const std::string dir = std::string(getenv("HOME")) + "/.cache/ar-prefobs-test";
    sh("rm -rf " + dir + " && mkdir -p " + dir);
    const std::string img = dir + "/o.arimg", db = dir + "/o.db", log = dir + "/o.log";
    const std::string arctl = ARCTL_PATH;

    assert(sh(arctl + " checkpoint --output " + img + " -- " + SPARSE_PATH +
              " >/dev/null 2>&1") == 0);

    // A completely fresh profile: nothing to prefetch anywhere.
    assert(sh(arctl + " restore --image " + img + " --policy prefetch --profile " + db +
              " > " + log + " 2>&1") == 0);
    const std::string out = slurp(log);

    const long faults = after(out, "faults     ");
    const long observed = after(out, "observed   ");
    const long recorded = after(out, "profile    ");

    assert(faults > 0 && "a fresh-profile prefetch degenerates to lazy and must fault");
    assert(observed > 0 && "those faults are evidence about fully deferred regions");
    assert(recorded > 0 && "and that evidence must reach the profile, not be discarded");
    assert(recorded == observed &&
           "with nothing prefetched every observed region is fully observable");

    // The planner should now say Lazy up front rather than requesting a
    // prefetch it knows the provider cannot perform.
    assert(out.find("no history") == std::string::npos &&
           "requested and effective action must agree when the outcome is knowable");

    // A second run reads that history back and can finally prefetch.
    assert(sh(arctl + " restore --image " + img + " --policy prefetch --profile " + db +
              " > " + log + " 2>&1") == 0);
    const std::string out2 = slurp(log);
    assert(out2.find("prefetched") != std::string::npos &&
           "the first run must have taught enough to prefetch on the second");
    assert(after(out2, "faults     ") < faults &&
           "a taught prefetch must fault less than a blind one");

    sh("rm -rf " + dir);
    std::puts("test_prefetch_observes: ok");
    return 0;
}
