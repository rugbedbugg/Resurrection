#pragma once
#include <sys/types.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "adaptive_restore/image/image_writer.hpp"

namespace adaptive_restore::checkpoint {

struct CaptureOptions {
    bool verify = false;
    std::uint64_t page_size = 4096;

    // Capture a target holding state the image cannot represent (signal masks
    // and handlers, extra threads, seccomp, a different cwd). Off by default:
    // such a checkpoint restores into a subtly different process, and the
    // divergence surfaces far from its cause. Set only when the caller
    // understands what will be lost.
    bool allow_unsupported_state = false;
};

struct CaptureResult {
    std::uint64_t mappings_total = 0;   // every VMA, saved or not
    std::uint64_t mappings_saved = 0;   // those whose contents were captured
    std::uint64_t regions_created = 0;
    std::uint64_t pages_saved = 0;      // page index records written
    std::uint64_t zero_pages = 0;       // subset stored as PageEncoding::Zero
    std::uint64_t file_pages = 0;       // subset referenced, not stored (FromFile)
    std::uint64_t bytes_saved = 0;      // raw page payload actually stored
    std::uint64_t fds_saved = 0;
    std::uint64_t verify_mismatches = 0;  // always 0 unless opts.verify
};

// Capture a ptrace-stopped target into `out`. The target must stay stopped for
// the whole call; nothing here stops or resumes it.
//
// With opts.verify, every captured page is read from the target a second time
// and re-checksummed against the first pass. That proves the page contents and
// their vaddr/encoding bookkeeping are stable and correctly recorded while the
// process is still live. Checking the serialized image against the process is
// the end-to-end gate and belongs to the caller.
CaptureResult capture(pid_t pid, const std::array<std::byte, 32>& launch_digest,
                      const CaptureOptions& opts, image::ImageWriter& out);

}  // namespace adaptive_restore::checkpoint
