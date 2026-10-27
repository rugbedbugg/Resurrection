#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "adaptive_restore/bench/cache.hpp"
#include "adaptive_restore/bench/suite.hpp"
#include "adaptive_restore/cli/commands.hpp"
#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::cli {

namespace {

int fail(const std::string& msg) {
    std::cerr << "arctl benchmark: " << msg << '\n';
    return 1;
}

double now_ms() {
    ::timespec t{};
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) / 1e6;
}

int run(const std::string& cmd) {
    const int rc = std::system(cmd.c_str());
    return (rc != -1 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
}

std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

double num_after(const std::string& log, const std::string& key) {
    const auto at = log.find(key);
    return at == std::string::npos ? 0.0 : std::atof(log.c_str() + at + key.size());
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

int cmd_benchmark(std::span<const std::string_view> args) {
    std::string suite_path, out_dir;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view flag = args[i];   // captured before i advances
        if (flag == "--suite" || flag == "--output") {
            if (i + 1 >= args.size()) return fail(std::string(flag) + " needs a value");
            (flag == "--suite" ? suite_path : out_dir) = std::string(args[++i]);
        } else {
            return fail("unknown option '" + std::string(flag) + "'");
        }
    }
    if (suite_path.empty() || out_dir.empty())
        return fail("usage: arctl benchmark --suite <file> --output <results-dir>");

    try {
        const auto suite = bench::Suite::parse(suite_path);
        std::filesystem::create_directories(out_dir);

        // Raw rows are written immutably, one per run, so the reduction below
        // can be redone without repeating hours of measurement.
        const std::string raw_path = out_dir + "/raw.tsv";
        std::ofstream raw(raw_path);
        raw << "workload\tpolicy\tcache\trep\trestore_ms\ttotal_ms\tfaults\toutput_ok\n";

        const std::string self = ARCTL_SELF_PATH;
        std::map<std::pair<std::string, std::string>, std::vector<double>> series;

        for (const auto& wl : suite.workloads) {
            const std::string name = std::filesystem::path(wl).filename();
            const std::string base = (suite.image_dir / ("bench_" + name)).string();
            const std::string img = base + ".arimg";
            const std::string prof = base + ".sqlite";
            std::filesystem::remove(prof);

            std::cout << "== " << name << " ==" << std::endl;
            const std::string ck_log = base + ".checkpoint.log";
            if (run(self + " checkpoint --output " + img + " -- " + wl + " > " + ck_log +
                    " 2>&1") != 0)
                return fail("checkpoint failed for " + wl + "; its output is in " + ck_log);
            const std::string baseline = slurp(img + ".stdout");

            for (int t = 0; t < suite.teach_reps; ++t)
                run(self + " restore --image " + img + " --policy lazy --profile " + prof +
                    " >/dev/null 2>&1");

            // Interleaved: all policies per rep, not all reps per policy. In
            // blocks, drift over the run lands unequally on whichever policy
            // went last and the margin swings by 3x.
            for (int rep = 0; rep < suite.reps; ++rep) {
                for (const auto& cache : suite.caches) {
                    for (const auto& pol : suite.policies) {
                        { std::ofstream trunc(img + ".stdout", std::ios::trunc); }
                        const std::string log = out_dir + "/last.log";
                        std::string extra = "--" + cache + "-cache";
                        if (pol == "prefetch" || pol == "cost")
                            extra += " --profile " + prof + " --no-learn";

                        // "cold" has to be made true, not just declared.
                        // --cold-cache only tells the cost model which
                        // calibration to use; without this the image stays in
                        // the page cache from the previous run and the row is
                        // a warm measurement wearing a cold label.
                        if (cache == "cold") {
                            std::string why;
                            if (!bench::evict_from_page_cache(img, why) ||
                                !bench::evict_from_page_cache(wl, why))
                                return fail("cannot produce a cold cache: " + why +
                                            "\n       Refusing to record cold rows that are "
                                            "warm measurements.");
                        }

                        const double t0 = now_ms();
                        run(self + " restore --image " + img + " --policy " + pol + " " + extra +
                            " > " + log + " 2>&1");
                        const double total = now_ms() - t0;

                        const std::string text = slurp(log);
                        const double restore = num_after(text, "resume     ") +
                                               num_after(text, "latency    ") *
                                                   num_after(text, "faults     ") / 1e6;
                        const bool ok = slurp(img + ".stdout") == baseline;

                        raw << name << '\t' << pol << '\t' << cache << '\t' << rep << '\t'
                            << restore << '\t' << total << '\t'
                            << static_cast<long>(num_after(text, "faults     ")) << '\t'
                            << (ok ? "yes" : "NO") << '\n';
                        series[{name + "/" + cache, pol}].push_back(restore);
                        if (!ok)
                            std::cerr << "  !! " << name << '/' << pol << '/' << cache
                                      << " produced wrong output\n";
                    }
                }
            }
            std::filesystem::remove(img);
            std::filesystem::remove(img + ".stdout");
            std::filesystem::remove(img + ".stderr");
            std::filesystem::remove(ck_log);
        }
        raw.close();

        // Medians, not means. Cache state is stated on every row, because
        // a number without it is not interpretable.
        std::cout << "\nmedian restore cost, ms\n";
        std::cout << "  workload/cache            ";
        for (const auto& p : suite.policies) std::printf("%10s", p.c_str());
        std::cout << "\n";
        std::string last;
        for (const auto& [k, _] : series) {
            if (k.first == last) continue;
            last = k.first;
            std::printf("  %-26s", k.first.c_str());
            for (const auto& p : suite.policies) {
                const auto it = series.find({k.first, p});
                std::printf("%10.1f", it == series.end() ? 0.0 : median(it->second));
            }
            std::cout << "\n";
        }
        std::cout << "\nraw rows: " << raw_path << '\n';
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    return 0;
}

}  // namespace adaptive_restore::cli
