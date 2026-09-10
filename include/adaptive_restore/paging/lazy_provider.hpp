#pragma once
#include <sys/types.h>

#include <cstdint>

#include "adaptive_restore/paging/page_locator.hpp"
#include "adaptive_restore/paging/page_provider.hpp"
#include "adaptive_restore/restore/uffd.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::paging {

struct LazyStats {
    std::uint64_t regions_registered = 0;
    std::uint64_t faults_served = 0;
    std::uint64_t copy_faults = 0;   // resolved with UFFDIO_COPY
    std::uint64_t zero_faults = 0;   // resolved with UFFDIO_ZEROPAGE
    std::uint64_t bytes_supplied = 0;
    std::uint64_t pages_registered = 0;
};

// Writes nothing before the target resumes; every page arrives as a demand
// fault and is resolved from the image (§9.4).
class LazyProvider : public PageProvider {
public:
    LazyProvider(const image::ImageReader& image, restore::Userfault& uffd);

    void prepare_region(std::uint32_t region_id, RestoreAction action) override;
    void service_fault(const PageRequest& request) override;

    const PageLocator& locator() const { return locator_; }
    const LazyStats& stats() const { return stats_; }

private:
    const image::ImageReader& image_;
    restore::Userfault& uffd_;
    PageLocator locator_;
    LazyStats stats_;
};

struct ServeResult {
    std::uint64_t faults_served = 0;
    bool target_exited = false;
};

// Drive the uffd until the target exits. Returns when the target is gone or the
// uffd hangs up. Any unrecoverable error kills the target rather than returning
// and leaving it stalled with nobody serving (docs/phase3-spike-report.md, Q3).
ServeResult serve_faults(restore::Userfault& uffd, PageProvider& provider,
                         const PageLocator& locator, pid_t target_pid);

}  // namespace adaptive_restore::paging
