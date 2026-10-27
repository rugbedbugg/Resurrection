// One image, two working sets, selected from outside the checkpoint.
//
// The point is that NOTHING about the phase lives in the image. argv strings,
// environ and every pointer into them are ordinary memory and are restored
// verbatim, so an env-selected phase would be frozen at checkpoint time. A
// regular-file descriptor is the one supported channel that still reaches
// current external state: the image records path/dev/ino/pos/flags, restore
// reopens the path in the target, and pread() then reads the file as it is now.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int sh(const std::string& cmd) { return std::system(cmd.c_str()); }

void write_phase(const std::string& path, char c) {
    // Truncate in place: the inode must survive, or restore correctly refuses
    // the descriptor on the dev/ino check.
    std::ofstream f(path, std::ios::trunc);
    f << c;
}

// Distinct page addresses in the fault trace.
std::set<unsigned long long> touched(const std::string& tsv) {
    std::set<unsigned long long> out;
    std::ifstream f(tsv);
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        const auto tab = line.find('\t');
        if (tab != std::string::npos)
            out.insert(std::strtoull(line.substr(0, tab).c_str(), nullptr, 16));
    }
    return out;
}

}  // namespace

int main() {
    const std::string dir = std::string(getenv("HOME")) + "/.cache/ar-phase-test";
    sh("rm -rf " + dir + " && mkdir -p " + dir);
    const std::string ctl = dir + "/phase.ctl";
    const std::string img = dir + "/pc.arimg";
    const std::string arctl = ARCTL_PATH;

    write_phase(ctl, 'A');
    assert(sh(arctl + " checkpoint --output " + img + " -- " + PHASE_PATH + " " + ctl +
              " >/dev/null 2>&1") == 0);

    // The image must never change again; every phase below restores this file.
    const std::string before = slurp(img);

    const auto restore = [&](char phase, const std::string& tag) {
        write_phase(ctl, phase);
        const std::string tr = dir + "/tr_" + tag + ".tsv";
        assert(sh(arctl + " restore --image " + img + " --policy lazy --record-trace " + tr +
                  " >/dev/null 2>&1") == 0);
        return std::make_pair(slurp(img + ".stdout"), touched(tr));
    };

    const auto a1 = restore('A', "a1");
    const auto a2 = restore('A', "a2");
    const auto b1 = restore('B', "b1");
    const auto b2 = restore('B', "b2");
    const auto a3 = restore('A', "a3");

    // 1. The phase controller is genuinely external: same bytes, different runs.
    assert(slurp(img) == before && "the image must be untouched by any restore");

    // 2. The same image really executes different phases.
    assert(a1.first.find("phase=A") != std::string::npos);
    assert(b1.first.find("phase=B") != std::string::npos);
    assert(a1.first != b1.first && "A and B must produce different output");

    // 3. Each phase is deterministic, so a trace difference means a phase
    //    difference and never run-to-run noise.
    assert(a1.first == a2.first && a1.first == a3.first);
    assert(b1.first == b2.first);
    assert(a1.second == a2.second && a1.second == a3.second &&
           "phase A must touch exactly the same pages every time");
    assert(b1.second == b2.second);

    // 4. The access sets are essentially disjoint, which is what makes a
    //    working set learned under A worthless under B.
    std::size_t shared = 0;
    for (const auto v : a1.second)
        if (b1.second.count(v)) ++shared;
    assert(a1.second.size() > 3000 && b1.second.size() > 3000);
    // A handful of stack/heap-metadata pages are touched by both; the 64 MiB
    // buffer's quarters are disjoint by construction.
    assert(shared * 100 < a1.second.size() &&
           "A and B working sets must barely overlap");

    // 5. Output correctness still holds: the workload reports its own ground
    //    truth, and the digests differ between phases but not within one.
    assert(a1.first.find("touched=4096") != std::string::npos);
    assert(b1.first.find("touched=4096") != std::string::npos);

    sh("rm -rf " + dir);
    std::puts("test_phase_change: ok");
    return 0;
}
