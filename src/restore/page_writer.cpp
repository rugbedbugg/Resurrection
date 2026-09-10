#include "adaptive_restore/restore/page_writer.hpp"

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/common/crc32c.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/restore/address_space.hpp"

namespace adaptive_restore::restore {

namespace {

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

void write_range(pid_t pid, std::uint64_t vaddr, const std::byte* src, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        ::iovec local{const_cast<std::byte*>(src) + done, len - done};
        ::iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(vaddr + done)),
                       len - done};
        const ssize_t n = ::process_vm_writev(pid, &local, 1, &remote, 1, 0);
        if (n < 0)
            throw CheckpointError("restore: process_vm_writev at " + hex(vaddr + done) + ": " +
                                  std::strerror(errno));
        if (n == 0) throw CheckpointError("restore: short write at " + hex(vaddr + done));
        done += static_cast<std::size_t>(n);
    }
}

void read_range(pid_t pid, std::uint64_t vaddr, std::byte* dst, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        ::iovec local{dst + done, len - done};
        ::iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(vaddr + done)),
                       len - done};
        const ssize_t n = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (n <= 0)
            throw CheckpointError("restore: read-back at " + hex(vaddr + done) + ": " +
                                  std::strerror(errno));
        done += static_cast<std::size_t>(n);
    }
}

}  // namespace

PopulateResult populate_pages(RestoreTarget& target, const image::ImageReader& image, bool verify) {
    const auto mappings = image.mappings();
    const auto pages = image.page_index();
    const std::uint64_t page_size = image.header().page_size;

    // A Zero page only comes back for free in fresh anonymous memory. Inside a
    // file-backed mapping the page holds file data, which is very unlikely to be
    // zero, so those must be cleared explicitly. Getting this wrong would leave
    // stale file bytes where the process expects zeros.
    const auto is_file_backed = [&mappings](std::uint64_t vaddr) {
        for (const auto& m : mappings)
            if (vaddr >= m.start_vaddr && vaddr < m.end_vaddr)
                return (m.flags & image::MappingFlag::FileBacked) != 0 && !m.path.empty();
        return false;
    };

    PopulateResult res;
    const std::vector<std::byte> zero_page(static_cast<std::size_t>(page_size), std::byte{0});

    // --- copy contents in ---------------------------------------------------
    std::size_t i = 0;
    while (i < pages.size()) {
        const auto& p = pages[i];

        if (p.encoding == image::PageEncoding::Omitted)
            throw CheckpointError("restore: page at " + hex(p.vaddr) +
                                  " was omitted at capture; eager restore cannot supply it");

        if (p.encoding == image::PageEncoding::Zero) {
            if (is_file_backed(p.vaddr)) {
                write_range(target.pid(), p.vaddr, zero_page.data(), zero_page.size());
                ++res.writev_calls;
                ++res.pages_zeroed;
            } else {
                ++res.pages_already_zero;
            }
            ++i;
            continue;
        }

        // Coalesce a run that is contiguous in the target AND in the image's
        // page store, so it can go out as one write.
        const auto first = image.page_bytes(p);
        std::size_t j = i + 1;
        std::size_t run_bytes = first.size();
        while (j < pages.size() && pages[j].encoding == image::PageEncoding::Raw &&
               pages[j].vaddr == pages[j - 1].vaddr + page_size &&
               pages[j].store_offset == pages[j - 1].store_offset + pages[j - 1].store_len) {
            run_bytes += pages[j].store_len;
            ++j;
        }

        write_range(target.pid(), p.vaddr, first.data(), run_bytes);
        ++res.writev_calls;
        res.pages_written += (j - i);
        res.bytes_written += run_bytes;
        i = j;
    }

    // --- apply the real protections, now that everything is in --------------
    for (const auto& m : mappings) {
        if (m.flags & image::MappingFlag::KernelSpecial) continue;
        const int want = prot_of(m.flags);
        if (want == (PROT_READ | PROT_WRITE)) continue;  // already correct
        const long rc = target.syscall(__NR_mprotect, m.start_vaddr, m.end_vaddr - m.start_vaddr,
                                       static_cast<std::uint64_t>(want));
        if (rc != 0)
            throw CheckpointError("restore: mprotect " + hex(m.start_vaddr) + ": " +
                                  std::strerror(static_cast<int>(-rc)));
        ++res.mappings_protected;
    }

    if (!verify) return res;

    // --- correctness reference: read every page back and re-checksum --------
    std::vector<std::byte> back(static_cast<std::size_t>(page_size));
    for (const auto& p : pages) {
        // A PROT_NONE region cannot be read back even by the tracer.
        bool readable = true;
        for (const auto& m : mappings)
            if (p.vaddr >= m.start_vaddr && p.vaddr < m.end_vaddr)
                readable = (prot_of(m.flags) & PROT_READ) != 0;
        if (!readable) continue;

        read_range(target.pid(), p.vaddr, back.data(), back.size());
        if (p.encoding == image::PageEncoding::Zero) {
            if (crc32c(back) != crc32c(zero_page)) ++res.verify_mismatches;
        } else if (crc32c(back) != p.crc32c) {
            ++res.verify_mismatches;
        }
    }
    return res;
}

}  // namespace adaptive_restore::restore
