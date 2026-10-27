#include "adaptive_restore/bench/suite.hpp"

#include <sys/statfs.h>

#include <fstream>
#include <sstream>

#include "adaptive_restore/common/error.hpp"

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

namespace adaptive_restore::bench {

namespace {

std::string trim(std::string v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    return v;
}

std::vector<std::string> split_commas(const std::string& v) {
    std::vector<std::string> out;
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

}  // namespace

bool is_tmpfs(const std::filesystem::path& dir) {
    struct statfs sf {};
    if (::statfs(dir.c_str(), &sf) != 0) return false;
    return static_cast<unsigned long>(sf.f_type) == TMPFS_MAGIC;
}

Suite Suite::parse(const std::filesystem::path& file) {
    std::ifstream f(file);
    if (!f) throw CheckpointError("benchmark: cannot read suite " + file.string());

    Suite s;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        if (line.rfind("run ", 0) == 0) {
            s.workloads.push_back(trim(line.substr(4)));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            throw CheckpointError("benchmark: " + file.string() + ":" + std::to_string(lineno) +
                                  ": expected 'key = value' or 'run <path>'");
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));

        if (key == "policies") s.policies = split_commas(value);
        else if (key == "caches") s.caches = split_commas(value);
        else if (key == "reps") s.reps = std::stoi(value);
        else if (key == "teach_reps") s.teach_reps = std::stoi(value);
        else if (key == "image_dir") s.image_dir = value;
        else
            throw CheckpointError("benchmark: " + file.string() + ":" + std::to_string(lineno) +
                                  ": unknown key '" + key + "'");
    }

    if (s.workloads.empty())
        throw CheckpointError("benchmark: " + file.string() + " lists no workloads");
    if (s.image_dir.empty())
        throw CheckpointError("benchmark: " + file.string() +
                              " must set image_dir, and it must be real storage");
    // Created here, before the tmpfs check: statfs on a path that does not
    // exist fails, and a failed check reads as "not tmpfs".
    std::error_code ec;
    std::filesystem::create_directories(s.image_dir, ec);
    if (ec || !std::filesystem::is_directory(s.image_dir))
        throw CheckpointError("benchmark: cannot create image_dir " + s.image_dir.string() +
                              ": " + (ec ? ec.message() : "not a directory"));
    if (is_tmpfs(s.image_dir))
        throw CheckpointError(
            "benchmark: image_dir " + s.image_dir.string() +
            " is on tmpfs. Images there are read from RAM and posix_fadvise cannot "
            "evict them, so every cold-cache figure would be fiction (see §26 and "
            "docs/limitations.md). Point it at real storage.");
    return s;
}

}  // namespace adaptive_restore::bench
