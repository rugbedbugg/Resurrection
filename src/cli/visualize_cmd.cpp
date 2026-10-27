// arctl visualize. Reads what a restore left behind and draws it.
//
// Strictly a consumer: it opens images, fault traces and benchmark results, and
// never starts a process. It never participates in the restore critical
// path, and it is also why this command builds in every
// configuration, including the one without compel.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "adaptive_restore/cli/commands.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/policy/plan.hpp"
#include "adaptive_restore/tui/render.hpp"

#ifdef AR_HAVE_SQLITE
#include "adaptive_restore/profile/profile_store.hpp"
#endif

namespace adaptive_restore::cli {

namespace {

int fail(const std::string& msg) {
    std::cerr << "arctl visualize: " << msg << '\n';
    return 1;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(line);
    while (std::getline(ss, cur, '\t')) out.push_back(cur);
    return out;
}

// Both readers below skip the header row by name rather than by position, so a
// column added to either file does not silently shift what is parsed.
bool read_faults(const std::string& path, std::vector<tui::FaultRow>& out, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "cannot read the fault trace " + path; return false; }
    std::string line;
    if (!std::getline(f, line)) { err = path + " is empty"; return false; }
    if (line.rfind("vaddr\t", 0) != 0) { err = path + " is not a fault trace"; return false; }
    while (std::getline(f, line)) {
        const auto c = split_tabs(line);
        if (c.size() < 6) continue;
        tui::FaultRow r;
        r.vaddr = std::strtoull(c[0].c_str(), nullptr, 0);
        r.region = static_cast<std::uint32_t>(std::strtoul(c[1].c_str(), nullptr, 10));
        r.offset_ns = std::strtoull(c[2].c_str(), nullptr, 10);
        r.service_ns = std::strtoull(c[3].c_str(), nullptr, 10);
        r.bytes = static_cast<std::uint32_t>(std::strtoul(c[4].c_str(), nullptr, 10));
        r.encoding = static_cast<std::uint32_t>(std::strtoul(c[5].c_str(), nullptr, 10));
        out.push_back(r);
    }
    return true;
}

bool read_results(const std::string& path, std::vector<tui::BenchRow>& out, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "cannot read the results file " + path; return false; }
    std::string line;
    if (!std::getline(f, line)) { err = path + " is empty"; return false; }
    if (line.rfind("workload\t", 0) != 0) { err = path + " is not a benchmark raw.tsv"; return false; }
    while (std::getline(f, line)) {
        const auto c = split_tabs(line);
        if (c.size() < 8) continue;
        tui::BenchRow r;
        r.workload = c[0];
        r.policy = c[1];
        r.cache = c[2];
        r.restore_ms = std::atof(c[4].c_str());
        r.faults = std::strtoull(c[6].c_str(), nullptr, 10);
        r.output_ok = c[7] == "yes";
        out.push_back(r);
    }
    return true;
}

int usage() {
    std::cerr << "usage: arctl visualize [--image <img> [--profile <db>] [--policy P]]\n"
                 "                       [--trace <faults.tsv>] [--results <dir|raw.tsv>]\n"
                 "                       [--cold-cache] [--width N] [--no-color]\n\n"
                 "  --image    draw the region map of the plan a restore would choose\n"
                 "  --trace    draw the demand-fault timeline from --record-trace output\n"
                 "  --results  draw the policy comparison from `arctl benchmark` output\n"
                 "  --policy   which plan to draw: eager|lazy|prefetch|cost (default cost)\n";
    return 2;
}

}  // namespace

int cmd_visualize(std::span<const std::string_view> args) {
    std::string image_path, profile_path, trace_path, results_path, policy_name = "cost";
    bool cold_cache = false, color = ::isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR");
    std::size_t width = 100;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) return {};
            return std::string(args[++i]);
        };
        if (a == "--image") image_path = value();
        else if (a == "--profile") profile_path = value();
        else if (a == "--trace") trace_path = value();
        else if (a == "--results") results_path = value();
        else if (a == "--policy") policy_name = value();
        else if (a == "--cold-cache") cold_cache = true;
        else if (a == "--no-color") color = false;
        else if (a == "--width") width = static_cast<std::size_t>(std::atoi(value().c_str()));
        else if (a == "-h" || a == "--help") return usage();
        else return fail("unknown option '" + std::string(a) + "'"), usage();
    }
    if (image_path.empty() && trace_path.empty() && results_path.empty()) return usage();
    if (width < 40) return fail("--width must be at least 40");

    policy::RestorePolicy policy = policy::RestorePolicy::Cost;
    if (policy_name == "eager") policy = policy::RestorePolicy::Eager;
    else if (policy_name == "lazy") policy = policy::RestorePolicy::Lazy;
    else if (policy_name == "prefetch") policy = policy::RestorePolicy::Prefetch;
    else if (policy_name != "cost") return fail("unknown policy '" + policy_name + "'");

    const tui::Style style{color, width};

    try {
        if (!image_path.empty()) {
            auto image = image::ImageReader::open(image_path);
            std::vector<profile::RegionHistory> history;
            if (!profile_path.empty()) {
#ifdef AR_HAVE_SQLITE
                profile::ProfileKey pkey;
                pkey.image_digest = image.identity_digest();
                pkey.region_layout_version = kRegionLayoutVersion;
                pkey.feature_schema_version = kFeatureSchemaVersion;
                auto store = profile::ProfileStore::open(profile_path);
                history = store.load(pkey);
                if (history.empty())
                    std::cerr << "arctl visualize: note: the profile holds no history for this "
                                 "image; the map shows the no-history plan.\n";
#else
                return fail("--profile needs SQLite, which this build was configured without");
#endif
            }
            const auto plan = policy::plan_restore(image, history, policy, cold_cache);
            tui::render_region_map(std::cout, image, plan, style);
        }

        if (!trace_path.empty()) {
            std::vector<tui::FaultRow> faults;
            std::string err;
            if (!read_faults(trace_path, faults, err)) return fail(err);
            tui::render_fault_timeline(std::cout, faults, style);
        }

        if (!results_path.empty()) {
            std::filesystem::path p(results_path);
            if (std::filesystem::is_directory(p)) p /= "raw.tsv";
            std::vector<tui::BenchRow> rows;
            std::string err;
            if (!read_results(p.string(), rows, err)) return fail(err);
            tui::render_policy_comparison(std::cout, rows, style);
        }
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    std::cout << '\n';
    return 0;
}

}  // namespace adaptive_restore::cli
