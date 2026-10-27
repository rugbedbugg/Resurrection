#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace adaptive_restore::bench {

// One measured run.
struct Row {
    std::string workload, policy, cache;
    int rep = 0;
    double restore_ms = 0;       // resume latency plus fault service
    double total_ms = 0;         // invocation to target exit
    std::uint64_t pages_loaded = 0;
    std::uint64_t faults = 0;
    bool output_matched = false;
};

// A benchmark suite.
//
// The original CLI design said `--suite suite.yaml`, but a YAML parser is a
// third-party dependency and this project's constraint is zero of those. A
// matrix this small does not need YAML: the format is `key = value` lines plus
// `run <path>` entries, which parses in thirty lines and reads without a
// manual.
struct Suite {
    std::vector<std::string> workloads;
    std::vector<std::string> policies{"eager", "lazy", "prefetch", "cost"};
    std::vector<std::string> caches{"cold", "warm"};
    int reps = 11;
    int teach_reps = 2;                   // lazy runs to seed the profile
    std::filesystem::path image_dir;      // MUST be real storage, not tmpfs

    static Suite parse(const std::filesystem::path& file);
};

// Where the image lives decides whether cold-cache numbers mean anything: on
// tmpfs there is no backing store, posix_fadvise cannot evict, and every "cold"
// figure is fiction.
bool is_tmpfs(const std::filesystem::path& dir);

}  // namespace adaptive_restore::bench
