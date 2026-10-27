#pragma once
#include <cstdint>

#include "adaptive_restore/restore/target.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::restore {

struct PopulateResult {
    std::uint64_t pages_written = 0;      // Raw pages copied in
    std::uint64_t pages_zeroed = 0;       // Zero pages explicitly cleared
    std::uint64_t pages_already_zero = 0; // Zero pages in fresh anonymous memory
    std::uint64_t pages_from_file = 0;    // supplied by the backing file, not written
    std::uint64_t bytes_written = 0;
    std::uint64_t writev_calls = 0;       // syscalls actually issued
    std::uint64_t mappings_protected = 0;
    std::uint64_t verify_mismatches = 0;  // always 0 unless verify
};

// Fill the reconstructed address space, then apply the recorded protections.
//
// Runs of pages that are contiguous in both target address and image store are
// coalesced into a single process_vm_writev, so a typical image costs tens of
// syscalls rather than one per 4 KiB page.
//
// Protections are applied only after every page is in: mapping a region
// read-only before writing it would make the write fail.
//
// With `verify`, every page is read back from the target and re-checksummed
// against the image. That is the eager correctness reference.
PopulateResult populate_pages(RestoreTarget& target, const image::ImageReader& image, bool verify);

}  // namespace adaptive_restore::restore
