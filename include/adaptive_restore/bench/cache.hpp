#pragma once
#include <filesystem>
#include <string>

namespace adaptive_restore::bench {

// Drop one file from the page cache and confirm it actually left.
//
// posix_fadvise(DONTNEED) is advisory in the strict sense: it silently does
// nothing for a file that is still dirty, still mapped by some other process,
// or on a filesystem with no backing store. A "cold" measurement taken from a
// warm cache is worse than no measurement, so this verifies the result with
// mincore() instead of trusting the call, and reports what it found.
//
// Returns true only if no page of the file is resident afterwards. On false,
// `why` explains what is still cached. Needs no privileges: this evicts one
// file, it does not touch /proc/sys/vm/drop_caches.
bool evict_from_page_cache(const std::filesystem::path& path, std::string& why);

}  // namespace adaptive_restore::bench
