#include "adaptive_restore/checkpoint/capture.hpp"

#include <sys/utsname.h>
#include <time.h>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "adaptive_restore/checkpoint/fds.hpp"
#include "adaptive_restore/checkpoint/maps.hpp"
#include "adaptive_restore/checkpoint/proc_mem.hpp"
#include "adaptive_restore/checkpoint/regions.hpp"
#include "adaptive_restore/checkpoint/registers.hpp"
#include "adaptive_restore/common/crc32c.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/format.hpp"

namespace adaptive_restore::checkpoint {

namespace {

// What the first pass recorded for one page, replayed against the target in the
// verify pass.
struct PageWitness {
    std::uint64_t vaddr;
    std::uint32_t crc;
    image::PageEncoding encoding;
};

std::uint32_t mapping_flags(const Mapping& m) {
    std::uint32_t f = 0;
    if (m.r) f |= image::MappingFlag::Read;
    if (m.w) f |= image::MappingFlag::Write;
    if (m.x) f |= image::MappingFlag::Exec;
    if (m.shared) f |= image::MappingFlag::Shared;
    if (m.kind == MapKind::FileBacked) f |= image::MappingFlag::FileBacked;
    if (m.kind == MapKind::KernelSpecial) f |= image::MappingFlag::KernelSpecial;
    if (m.kind == MapKind::Unreadable) f |= image::MappingFlag::Unreadable;
    return f;
}

// Kernel-provided pages are re-created by the kernel on restore, and a mapping
// with neither read nor exec access has nothing to read.
bool contents_eligible(MapKind kind) {
    return kind == MapKind::AnonPrivate || kind == MapKind::FileBacked;
}

std::string kernel_release() {
    ::utsname u{};
    if (::uname(&u) != 0) return "";
    return std::string(u.release);
}

std::uint64_t realtime_ns() {
    ::timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

}  // namespace

CaptureResult capture(pid_t pid, const std::array<std::byte, 32>& launch_digest,
                      const CaptureOptions& opts, image::ImageWriter& out) {
    if (opts.page_size == 0 || (opts.page_size & (opts.page_size - 1)) != 0)
        throw CheckpointError("capture: page_size must be a power of two");
    const std::uint64_t page_size = opts.page_size;

    CaptureResult res;
    out.set_registers(capture_registers(pid));

    ProcMem mem(pid);
    const std::vector<Mapping> maps = read_maps(pid);
    res.mappings_total = maps.size();

    std::vector<PageWitness> witnesses;
    std::vector<std::byte> region_buf;
    std::uint64_t page_counter = 0;
    std::uint32_t next_region_id = 0;

    for (const Mapping& m : maps) {
        if (m.kind == MapKind::Unsupported)
            throw CheckpointError("capture: unsupported mapping at " + hex(m.start) + " (" +
                                  m.path + ")");

        image::MappingRecord rec;
        rec.flags = mapping_flags(m);
        rec.start_vaddr = m.start;
        rec.end_vaddr = m.end;
        rec.file_offset = m.file_offset;
        rec.dev = m.dev;
        rec.ino = m.ino;
        rec.path = m.path;

        const bool save = contents_eligible(m.kind);
        if (save) rec.flags |= image::MappingFlag::ContentsSaved;

        const std::uint32_t mapping_id = out.add_mapping(rec);
        if (!save) continue;  // recorded for layout, but carries no pages
        ++res.mappings_saved;

        if ((m.end - m.start) % page_size != 0)
            throw CheckpointError("capture: mapping at " + hex(m.start) +
                                  " is not a whole number of pages");

        image::MappingRecord for_subdivide = rec;
        for_subdivide.mapping_id = mapping_id;
        auto regions = subdivide(for_subdivide, next_region_id);

        for (image::RegionRecord& r : regions) {
            r.page_index_lo = static_cast<std::uint32_t>(page_counter);

            // One syscall per region rather than per page, then sliced up.
            const std::uint64_t span = r.end_vaddr - r.start_vaddr;
            mem.read_range(r.start_vaddr, static_cast<std::size_t>(span), region_buf);

            for (std::uint64_t off = 0; off < span; off += page_size) {
                const std::span<const std::byte> page(region_buf.data() + off,
                                                      static_cast<std::size_t>(page_size));
                image::PageIndexRecord p;
                p.vaddr = r.start_vaddr + off;

                if (page_is_zero(page)) {
                    p.encoding = image::PageEncoding::Zero;
                    out.add_page(p, {});
                    ++res.zero_pages;
                } else {
                    p.encoding = image::PageEncoding::Raw;
                    out.add_page(p, page);
                    res.bytes_saved += page_size;
                }

                if (opts.verify)
                    witnesses.push_back({p.vaddr, crc32c(page), p.encoding});

                ++page_counter;
                ++res.pages_saved;
            }

            r.page_index_hi = static_cast<std::uint32_t>(page_counter);
            out.add_region(r);
            ++res.regions_created;
            ++next_region_id;
        }
    }

    for (const image::FdRecord& f : collect_fds(pid)) {
        out.add_fd(f);
        ++res.fds_saved;
    }

    image::ImageHeaderInfo header;
    header.page_size = static_cast<std::uint32_t>(page_size);
    header.kernel_release = kernel_release();
    header.original_pid = static_cast<std::uint32_t>(pid);
    header.checkpoint_time_ns = realtime_ns();
    header.launch_env_digest = launch_digest;
    out.set_header(header);

    if (opts.verify) {
        std::vector<std::byte> again;
        for (const PageWitness& w : witnesses) {
            mem.read_range(w.vaddr, static_cast<std::size_t>(page_size), again);
            const bool zero_now = page_is_zero(again);
            const bool zero_then = w.encoding == image::PageEncoding::Zero;
            if (zero_now != zero_then || crc32c(again) != w.crc) ++res.verify_mismatches;
        }
    }

    return res;
}

}  // namespace adaptive_restore::checkpoint
