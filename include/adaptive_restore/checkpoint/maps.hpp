#pragma once
#include <sys/types.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace adaptive_restore::checkpoint {

// How a VMA is treated by the capture pass.
//   AnonPrivate   private anonymous memory (includes [heap]/[stack]) -- saved
//   FileBacked    backed by a file -- saved, identity recorded via dev+ino
//   KernelSpecial [vdso]/[vvar]/[vsyscall] -- never saved, re-created by the kernel
//   GuardNone     no read and no exec access -- nothing to read, recorded only
//   Unreadable    readable per perms but the read failed (set by the capture pass)
//   Unsupported   recognised but out of Tier 1 scope (set by the capture pass)
enum class MapKind { AnonPrivate, FileBacked, KernelSpecial, GuardNone, Unreadable, Unsupported };

// One line of /proc/<pid>/maps, parsed and classified.
struct Mapping {
    std::uint64_t start = 0, end = 0;
    bool r = false, w = false, x = false, shared = false;
    std::uint64_t file_offset = 0;
    std::uint64_t dev = 0;  // makedev(major, minor); 0 when anonymous
    std::uint64_t ino = 0;
    std::string path;       // pathname column verbatim, "" when absent
    MapKind kind = MapKind::AnonPrivate;
    std::string tag;        // "[heap]", "[stack]", "[vdso]", ... or ""
};

// Parse the text of a maps file. Blank lines are skipped; a malformed line
// throws CheckpointError. Pure and testable -- no /proc access.
std::vector<Mapping> parse_maps_text(std::string_view maps);

// Read and parse /proc/<pid>/maps. Throws CheckpointError if it cannot be read.
std::vector<Mapping> read_maps(pid_t pid);

}  // namespace adaptive_restore::checkpoint
