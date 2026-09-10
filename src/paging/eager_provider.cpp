#include "adaptive_restore/paging/eager_provider.hpp"

#include <sys/mman.h>
#include <sys/syscall.h>

#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "adaptive_restore/common/crc32c.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/paging/transfer.hpp"
#include "adaptive_restore/restore/address_space.hpp"

namespace adaptive_restore::paging {

using detail::hex;

EagerProvider::EagerProvider(restore::RestoreTarget& target, const image::ImageReader& image)
    : target_(target),
      image_(image),
      mappings_(image.mappings()),
      regions_(image.regions()),
      pages_(image.page_index()),
      zero_page_(static_cast<std::size_t>(image.header().page_size), std::byte{0}) {}

void EagerProvider::prepare_region(std::uint32_t region_id, RestoreAction action) {
    if (action != RestoreAction::Eager)
        throw CheckpointError("EagerProvider: asked to prepare region " +
                              std::to_string(region_id) + " with a non-eager action");

    if (region_id >= regions_.size())
        throw CheckpointError("EagerProvider: no such region " + std::to_string(region_id));
    const auto& r = regions_[region_id];

    const auto& pages = pages_;
    const auto& mappings = mappings_;
    const std::uint64_t page_size = image_.header().page_size;

    // A Zero page only comes back for free in fresh anonymous memory. Inside a
    // file-backed mapping the page holds file data, so it must be cleared.
    const auto is_file_backed = [&mappings](std::uint64_t vaddr) {
        for (const auto& m : mappings)
            if (vaddr >= m.start_vaddr && vaddr < m.end_vaddr)
                return (m.flags & image::MappingFlag::FileBacked) != 0 && !m.path.empty();
        return false;
    };

    std::size_t i = r.page_index_lo;
    const std::size_t end = r.page_index_hi;
    while (i < end) {
        const auto& p = pages[i];

        if (p.encoding == image::PageEncoding::Omitted)
            throw CheckpointError("restore: page at " + hex(p.vaddr) +
                                  " was omitted at capture; eager restore cannot supply it");

        if (p.encoding == image::PageEncoding::FromFile) {
            // The mapping was created from the same file; the page cache holds
            // this content already.
            ++stats_.pages_from_file;
            ++i;
            continue;
        }

        if (p.encoding == image::PageEncoding::Zero) {
            if (is_file_backed(p.vaddr)) {
                detail::write_range(target_.pid(), p.vaddr, zero_page_.data(), zero_page_.size());
                ++stats_.writev_calls;
                ++stats_.pages_zeroed;
            } else {
                ++stats_.pages_already_zero;
            }
            ++i;
            continue;
        }

        // Coalesce a run contiguous in both the target and the image store, so
        // it goes out as a single write.
        const auto first = image_.page_bytes(p);
        std::size_t j = i + 1;
        std::size_t run_bytes = first.size();
        while (j < end && pages[j].encoding == image::PageEncoding::Raw &&
               pages[j].vaddr == pages[j - 1].vaddr + page_size &&
               pages[j].store_offset == pages[j - 1].store_offset + pages[j - 1].store_len) {
            run_bytes += pages[j].store_len;
            ++j;
        }

        detail::write_range(target_.pid(), p.vaddr, first.data(), run_bytes);
        ++stats_.writev_calls;
        stats_.pages_written += (j - i);
        stats_.bytes_written += run_bytes;
        i = j;
    }
}

void EagerProvider::service_fault(const PageRequest& request) {
    // An eager region has every page written before the target resumes, so a
    // fault here means the promise was broken.
    throw CheckpointError("EagerProvider: unexpected fault at " + hex(request.virtual_address) +
                          " in region " + std::to_string(request.region_id) +
                          "; an eagerly prepared page was missing");
}

void EagerProvider::apply_protections() {
    for (const auto& m : mappings_) {
        if (m.flags & image::MappingFlag::KernelSpecial) continue;
        const int want = restore::prot_of(m.flags);
        if (want == (PROT_READ | PROT_WRITE)) continue;  // already correct
        const long rc = target_.syscall(__NR_mprotect, m.start_vaddr, m.end_vaddr - m.start_vaddr,
                                        static_cast<std::uint64_t>(want));
        if (rc != 0)
            throw CheckpointError("restore: mprotect " + hex(m.start_vaddr) + ": " +
                                  std::strerror(static_cast<int>(-rc)));
        ++stats_.mappings_protected;
    }
}

void EagerProvider::verify() {
    const auto& mappings = mappings_;
    std::vector<std::byte> back(zero_page_.size());
    for (const auto& p : pages_) {
        // A PROT_NONE region cannot be read back even by the tracer.
        bool readable = true;
        for (const auto& m : mappings)
            if (p.vaddr >= m.start_vaddr && p.vaddr < m.end_vaddr)
                readable = (restore::prot_of(m.flags) & PROT_READ) != 0;
        if (!readable) continue;

        detail::read_range(target_.pid(), p.vaddr, back.data(), back.size());
        if (p.encoding == image::PageEncoding::Zero) {
            if (crc32c(back) != crc32c(zero_page_)) ++stats_.verify_mismatches;
        } else if (crc32c(back) != p.crc32c) {
            // Covers FromFile too: proof that the page cache handed back
            // exactly what was captured.
            ++stats_.verify_mismatches;
        }
    }
}

}  // namespace adaptive_restore::paging
