#pragma once
#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "adaptive_restore/image/format.hpp"
#include "adaptive_restore/paging/page_locator.hpp"
#include "adaptive_restore/paging/page_provider.hpp"
#include "adaptive_restore/restore/uffd.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::paging {

// One demand fault, as it happened. Under pure lazy every first touch faults,
// so a full trace IS the workload's touched set -- the ground truth the profile
// and the benchmark measure waste and coverage against.
struct FaultRecord {
    std::uint64_t vaddr = 0;
    std::uint32_t region_id = 0;
    std::uint64_t offset_ns = 0;      // since serving began
    std::uint64_t service_ns = 0;     // time spent resolving this fault
    std::uint32_t bytes = 0;          // supplied from the image (0 for a zero page)
    std::uint8_t encoding = 0;        // image::PageEncoding
};

struct LazyStats {
    std::uint64_t regions_registered = 0;
    std::uint64_t faults_served = 0;
    std::uint64_t copy_faults = 0;   // resolved with UFFDIO_COPY
    std::uint64_t zero_faults = 0;   // resolved with UFFDIO_ZEROPAGE
    std::uint64_t bytes_supplied = 0;
    std::uint64_t pages_registered = 0;
    std::uint64_t service_ns_total = 0;
};

// Writes nothing before the target resumes; every page arrives as a demand
// fault and is resolved from the image.
class LazyProvider : public PageProvider {
public:
    LazyProvider(const image::ImageReader& image, restore::Userfault& uffd);

    void prepare_region(std::uint32_t region_id, RestoreAction action) override;
    void service_fault(const PageRequest& request) override;

    const PageLocator& locator() const { return locator_; }
    const LazyStats& stats() const { return stats_; }
    const std::vector<FaultRecord>& trace() const { return trace_; }

    // Keep a per-fault trace. Off by default: a long run can fault a lot, and
    // the recording itself must not distort what it measures.
    void record_trace(bool on) { recording_ = on; }

    // Write the trace as TSV. Debuggability wins over compression here: this
    // file is read by humans and by the analysis scripts, not by the restorer.
    void write_trace(const std::string& path) const;

private:
    const image::ImageReader& image_;
    restore::Userfault& uffd_;
    PageLocator locator_;
    std::vector<image::RegionRecord> regions_;  // cached; see prepare_region
    LazyStats stats_;
    std::vector<FaultRecord> trace_;
    bool recording_ = false;
    std::uint64_t serving_started_ns_ = 0;
};

struct ServeResult {
    std::uint64_t faults_served = 0;
    bool target_exited = false;
    int exit_status = 0;  // wait status, valid when target_exited
};

// Drive the uffd until the target exits. Returns when the target is gone or the
// uffd hangs up. Any unrecoverable error kills the target rather than returning
// and leaving it stalled with nobody serving.
ServeResult serve_faults(restore::Userfault& uffd, PageProvider& provider,
                         const PageLocator& locator, pid_t target_pid);

}  // namespace adaptive_restore::paging
