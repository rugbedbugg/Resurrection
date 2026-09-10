#pragma once
#include <cstdint>

#include "adaptive_restore/restore/target.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::restore {

struct ResumeResult {
    std::uint64_t fds_restored = 0;
    std::uint64_t fds_closed = 0;      // stub descriptors the image does not have
    std::uint64_t xstate_bytes = 0;
    std::uint64_t residual_mappings = 0;  // see "Residual mappings" below
};

// Rebuild the descriptor table inside the target: reopen each recorded path,
// seek to its recorded offset, and move it to its recorded fd number. Stub
// descriptors absent from the image are closed. Validates dev/ino; a backing
// file that moved is a hard failure.
// `keep_fd` is a descriptor in the target that must survive the sweep even
// though the image does not list it. The target's own userfaultfd reference is
// deliberately held open (see uffd.hpp) so that losing the page server stalls
// faults rather than silently resolving them to zero pages; closing it here
// would quietly remove that guarantee.
std::uint64_t restore_fds(RestoreTarget& target, const image::ImageReader& image,
                          int keep_fd = -1);

// Load the saved register file and hand the process control. After this returns
// the target is running and is no longer managed by `target`.
//
// Residual mappings: the reserve and the stub's TLS block survive into the
// restored process. Neither can be removed, because compel executes from the
// reserve and restores fs_base around every injected syscall, so the last
// syscall cannot be the one that unmaps its own footing. Both are placed clear
// of the image, so the restored program cannot observe them except by reading
// /proc/self/maps. Documented as a Tier 1 limitation.
void transfer_control(RestoreTarget& target, const image::ImageReader& image);

// restore_fds() then transfer_control().
ResumeResult resume(RestoreTarget& target, const image::ImageReader& image,
                    int keep_fd = -1);

}  // namespace adaptive_restore::restore
