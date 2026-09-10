#pragma once
#include <vector>

#include "adaptive_restore/image/format.hpp"

#include "adaptive_restore/paging/page_provider.hpp"
#include "adaptive_restore/restore/page_writer.hpp"
#include "adaptive_restore/restore/target.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::paging {

// Writes a region's pages in full before the target resumes. The correctness
// reference every other action is measured against (§9.3).
class EagerProvider : public PageProvider {
public:
    EagerProvider(restore::RestoreTarget& target, const image::ImageReader& image);

    void prepare_region(std::uint32_t region_id, RestoreAction action) override;
    void service_fault(const PageRequest& request) override;

    // Lower each mapping to its recorded protection. Must run after every
    // region is prepared: a region mapped read-only could not be written.
    void apply_protections();

    // Read every page back from the target and re-checksum it (§9.3).
    void verify();

    const restore::PopulateResult& stats() const { return stats_; }

private:
    restore::RestoreTarget& target_;
    const image::ImageReader& image_;

    // ImageReader parses each section on demand, so these are cached once.
    // Re-reading them per region turns a linear restore into a quadratic one.
    std::vector<image::MappingRecord> mappings_;
    std::vector<image::RegionRecord> regions_;
    std::vector<image::PageIndexRecord> pages_;

    restore::PopulateResult stats_;
    std::vector<std::byte> zero_page_;
};

}  // namespace adaptive_restore::paging
