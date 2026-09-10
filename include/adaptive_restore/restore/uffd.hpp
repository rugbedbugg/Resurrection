#pragma once
#include <cstdint>
#include <vector>

#include "adaptive_restore/image/format.hpp"
#include "adaptive_restore/restore/target.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::restore {

struct LazyRange {
    std::uint64_t start = 0, end = 0;
    std::uint32_t mapping_id = 0;
};

// Which of an image's mappings can be served by userfaultfd MISSING mode.
//
// Only private anonymous mappings whose contents were saved. File-backed
// mappings are excluded: MISSING mode does not apply to them, and they are
// already demand-paged by the kernel from their own file. Kernel-special and
// no-access mappings carry nothing to serve.
std::vector<LazyRange> lazy_ranges(const image::ImageReader& image);

// The target's userfaultfd, created inside it and driven from here.
//
// The fd is created by the target (a uffd is bound to the mm it was made in)
// and then stolen with pidfd_getfd, so every ioctl runs from arctl and no code
// needs to execute inside the target to service faults.
class Userfault {
public:
    // Creates the uffd, steals it, runs UFFDIO_API, and arms the target's
    // death signal. Throws CheckpointError on any failure.
    explicit Userfault(RestoreTarget& target);
    ~Userfault();

    Userfault(const Userfault&) = delete;
    Userfault& operator=(const Userfault&) = delete;

    int fd() const { return fd_; }

    // The descriptor number the uffd occupies INSIDE the target. That reference
    // is kept deliberately (see the constructor), so anything sweeping the
    // target's fd table must spare it.
    int target_fd() const { return target_fd_; }
    std::uint64_t features() const { return features_; }

    // Register a range in MISSING mode. The range must be page-aligned and must
    // not cover the compel reserve or the stub's TLS.
    void register_missing(std::uint64_t start, std::uint64_t end);

    std::uint64_t registered_bytes() const { return registered_bytes_; }

private:
    int fd_ = -1;
    int target_fd_ = -1;
    std::uint64_t features_ = 0;
    std::uint64_t registered_bytes_ = 0;
    std::uint64_t reserve_start_ = 0, reserve_end_ = 0;
    std::uint64_t tls_start_ = 0, tls_end_ = 0;
};

}  // namespace adaptive_restore::restore
