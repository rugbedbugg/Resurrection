#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "adaptive_restore/image/format.hpp"
#include "adaptive_restore/restore/target.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::restore {

// Recorded mapping flags -> PROT_* bits.
int prot_of(std::uint32_t mapping_flags);

struct LayoutResult {
    std::uint64_t evicted = 0;          // stub mappings torn down
    std::uint64_t created = 0;          // image mappings recreated
    std::uint64_t skipped_kernel = 0;   // [vdso]/[vvar]/[vsyscall], kernel-provided
    std::uint64_t bytes_mapped = 0;
};

// True when any image mapping overlaps the stub's TLS block. That block is
// load-bearing for compel and cannot be relocated, so such a stub cannot host
// this image and must be discarded. (docs/phase2-spike-report.md, trap 3.)
bool collides_with_tls(const RestoreTarget& target,
                       const std::vector<image::MappingRecord>& mappings);

// Fork stubs until one lands with its TLS clear of the image, or give up.
// ASLR gives a fresh TLS address on every attempt.
std::unique_ptr<RestoreTarget> make_target_for(const std::vector<image::MappingRecord>& mappings,
                                               std::uint64_t reserve_addr,
                                               std::size_t reserve_bytes, int max_attempts = 8);

// Tear down the stub's address space and rebuild the image's in its place.
//
// Every non-kernel mapping is recreated, including no-access guard regions, so
// the restored layout matches the original rather than merely covering the
// pages that carry data. Mappings are created writable regardless of their
// recorded protection; Task 5 applies the real protection after populating them.
LayoutResult rebuild_address_space(RestoreTarget& target, const image::ImageReader& image);

}  // namespace adaptive_restore::restore
