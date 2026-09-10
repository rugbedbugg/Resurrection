#pragma once
#include <sys/types.h>

#include <cstdint>

#include "adaptive_restore/restore/address_space.hpp"
#include "adaptive_restore/restore/page_writer.hpp"
#include "adaptive_restore/restore/resume.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::restore {

struct RestoreOptions {
    bool verify = false;
    // Scratch area for compel. Kept far from any realistic process layout so it
    // cannot collide with an image; see docs/phase2-spike-report.md.
    std::uint64_t reserve_addr = 0x700000000000ULL;
    std::size_t reserve_bytes = 4 * 4096;
};

struct RestoreSummary {
    pid_t pid = -1;
    LayoutResult layout;
    PopulateResult pages;
    ResumeResult resume;
    std::uint64_t resume_latency_ns = 0;  // invocation to first resumed instruction (§22)
};

// Rebuild the image into a fresh process and hand it control. Returns once the
// process is running; the caller owns waiting on it. Throws on any failure
// before control transfer, leaving no partially-restored process behind.
RestoreSummary restore_eager(const image::ImageReader& image, const RestoreOptions& opts);

}  // namespace adaptive_restore::restore
