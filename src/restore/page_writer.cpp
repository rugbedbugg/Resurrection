#include "adaptive_restore/restore/page_writer.hpp"

#include <string>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/paging/eager_provider.hpp"

namespace adaptive_restore::restore {

// Kept as the eager driver so existing callers are unaffected; the work now
// lives behind the PageProvider interface (design-draft-v2 §6.1) so Phase 3's
// lazy provider and Phase 4's controllers slot in without touching this.
PopulateResult populate_pages(RestoreTarget& target, const image::ImageReader& image, bool verify) {
    paging::EagerProvider provider(target, image);

    const auto regions = image.regions();
    for (std::uint32_t id = 0; id < regions.size(); ++id)
        provider.prepare_region(id, paging::RestoreAction::Eager);

    // Regions are how work is dispatched, so a page outside every region would
    // be silently skipped and the target would resume with a hole in it. The
    // §9.2 invariant demands better than that.
    std::uint64_t covered = 0;
    for (const auto& r : regions) covered += r.page_index_hi - r.page_index_lo;
    if (covered != image.page_index().size())
        throw CheckpointError("restore: the region table covers " + std::to_string(covered) +
                              " of " + std::to_string(image.page_index().size()) +
                              " pages; the image is inconsistent");

    provider.apply_protections();
    if (verify) provider.verify();
    return provider.stats();
}

}  // namespace adaptive_restore::restore
