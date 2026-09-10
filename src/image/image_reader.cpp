#include "adaptive_restore/image/image_reader.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>

#include "adaptive_restore/common/byte_cursor.hpp"
#include "adaptive_restore/common/crc32c.hpp"
#include "adaptive_restore/common/sha256.hpp"
#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::image {

namespace {

constexpr std::size_t kSectionCount = static_cast<std::size_t>(Section::Count);

// Byte offset of the header's metadata_crc32c field: it sits immediately after
// the fixed prefix and the SectionRef[7] directory. Mirrors the writer, which
// records this position as `w.size()` right before emitting the field.
constexpr std::size_t kMetadataCrc32cOffset =
    kFixedPrefixSize + sizeof(SectionRef) * kSectionCount;

std::size_t sidx(Section s) { return static_cast<std::size_t>(s); }

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

// CRC-32C over `data` with the 4 bytes at [hole, hole+4) treated as zero. Used
// to recompute the two checksums whose own storage lies inside their coverage.
std::uint32_t crc32c_with_hole(std::span<const std::byte> data, std::size_t hole) {
    static constexpr std::array<std::byte, 4> kZero4{};
    std::uint32_t c = crc32c(data.subspan(0, hole));
    c = crc32c(kZero4, c);
    c = crc32c(data.subspan(hole + 4), c);
    return c;
}

}  // namespace

std::span<const std::byte> ImageReader::span(std::uint64_t offset, std::uint64_t length) const {
    return std::span<const std::byte>(buf_.data() + offset, length);
}

std::uint32_t ImageReader::read_u32(std::uint64_t offset) const {
    ByteReader r(span(offset, 4));
    return r.u32();
}

std::uint64_t ImageReader::read_u64(std::uint64_t offset) const {
    ByteReader r(span(offset, 8));
    return r.u64();
}

void ImageReader::parse_header() {
    ByteReader r(span(0, kHeaderSize));
    r.skip(8);            // magic
    r.skip(4);            // format_version  (validated in verify())
    r.skip(4);            // header_size     (validated in verify())
    r.skip(2);            // arch
    r.skip(1);            // endianness
    r.skip(1);            // _pad0
    header_.page_size = r.u32();

    auto kr = r.bytes(kKernelReleaseSize);
    const auto* krp = reinterpret_cast<const char*>(kr.data());
    header_.kernel_release.assign(krp, ::strnlen(krp, kKernelReleaseSize));
    r.skip(3);            // _pad1

    header_.original_pid = r.u32();
    header_.checkpoint_time_ns = r.u64();
    auto digest = r.bytes(header_.launch_env_digest.size());
    std::copy(digest.begin(), digest.end(), header_.launch_env_digest.begin());

    for (auto& s : dir_) {
        s.offset = r.u64();
        s.length = r.u64();
        s.count = r.u64();
    }
}

void ImageReader::verify() const {
    // 1. Magic + minimum size.
    if (buf_.size() < kHeaderSize)
        throw ImageError("image: buffer smaller than the fixed header");
    if (std::memcmp(buf_.data(), kImageMagic, 8) != 0)
        throw ImageError("image: bad magic");

    // 2. Version + fixed fields.
    const std::uint32_t got_version = read_u32(8);
    if (got_version != static_cast<std::uint32_t>(kImageFormatVersion))
        throw ImageError("image: unsupported format_version " + std::to_string(got_version) +
                         " (this build reads v" + std::to_string(kImageFormatVersion) +
                         "); re-checkpoint the workload");
    if (read_u32(12) != kHeaderSize)
        throw ImageError("image: header_size does not match kHeaderSize");
    const std::uint32_t ps = header_.page_size;
    if (ps < 4096 || (ps & (ps - 1)) != 0)
        throw ImageError("image: page_size is not a power of two >= 4096");

    // 3. Section directory: contiguous, in bounds, covering the whole file.
    std::uint64_t cursor = kHeaderSize;
    for (std::size_t i = 0; i < kSectionCount; ++i) {
        const SectionRef& s = dir_[i];
        if (s.length > buf_.size() || s.offset > buf_.size() - s.length)
            throw ImageError("image: section " + std::to_string(i) + " out of bounds");
        if (s.offset != cursor)
            throw ImageError("image: section " + std::to_string(i) + " not contiguous");
        cursor = s.offset + s.length;
    }
    if (cursor != buf_.size())
        throw ImageError("image: trailing bytes after the last section");

    const SectionRef& page_data = dir_[sidx(Section::PageData)];
    const SectionRef& fd = dir_[sidx(Section::Fd)];
    const SectionRef& integrity = dir_[sidx(Section::Integrity)];
    if (integrity.length != 12)
        throw ImageError("image: integrity section has the wrong size");

    // 4. metadata_crc32c: header (its own crc field zeroed) + register + mapping
    //    + region + page-index + fd sections. Page data is excluded, so the
    //    covered bytes are [0, page_data.offset) then the fd section.
    std::uint32_t meta = crc32c_with_hole(span(0, page_data.offset), kMetadataCrc32cOffset);
    meta = crc32c(span(fd.offset, fd.length), meta);
    if (meta != read_u32(kMetadataCrc32cOffset))
        throw ImageError("image: metadata CRC mismatch");

    // 5. page_data_crc32c (stored as u64) over the page data store.
    const std::uint32_t page_data_crc = crc32c(span(page_data.offset, page_data.length));
    if (read_u64(integrity.offset + 4) != static_cast<std::uint64_t>(page_data_crc))
        throw ImageError("image: page data CRC mismatch");

    // 5b. Every Raw page must be exactly one page long. Consumers copy
    //     page_size bytes starting at page_bytes(), so a short record would
    //     read past the page store -- past the whole buffer for the last page --
    //     and hand that memory to the restored target.
    {
        ByteReader pr(span(dir_[sidx(Section::PageIndex)].offset,
                           dir_[sidx(Section::PageIndex)].length));
        const std::uint64_t n = dir_[sidx(Section::PageIndex)].count;
        for (std::uint64_t i = 0; i < n; ++i) {
            const std::uint64_t vaddr = pr.u64();
            pr.u64();  // store_offset
            const std::uint32_t store_len = pr.u32();
            const auto enc = static_cast<PageEncoding>(pr.u8());
            pr.skip(3);
            pr.u32();  // crc32c
            if (enc == PageEncoding::Raw && store_len != ps)
                throw ImageError("image: raw page @ vaddr " + hex(vaddr) + " has store_len " +
                                 std::to_string(store_len) + ", expected " + std::to_string(ps));
            if (enc != PageEncoding::Raw && store_len != 0)
                throw ImageError("image: non-raw page @ vaddr " + hex(vaddr) +
                                 " carries a payload length");
        }
    }

    // 6. full_file_crc32c over the whole buffer with its own field zeroed.
    const std::uint32_t full = crc32c_with_hole(span(0, buf_.size()), integrity.offset);
    if (full != read_u32(integrity.offset))
        throw ImageError("image: whole-file CRC mismatch");
}

ImageReader ImageReader::from_bytes(std::vector<std::byte> bytes) {
    ImageReader r;
    r.buf_ = std::move(bytes);
    if (r.buf_.size() >= kHeaderSize)
        r.parse_header();  // otherwise verify() reports the short buffer first
    r.verify();
    return r;
}

ImageReader ImageReader::open(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw ImageError("image: cannot open " + path.string());

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (size < 0)
        throw ImageError("image: cannot determine size of " + path.string());

    std::vector<std::byte> buf(static_cast<std::size_t>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(buf.data()), size))
        throw ImageError("image: short read on " + path.string());

    return from_bytes(std::move(buf));
}

RegisterRecord ImageReader::registers() const {
    const SectionRef& s = dir_[sidx(Section::Register)];
    ByteReader r(span(s.offset, s.length));

    RegisterRecord rr;
    rr.record_version = r.u32();
    const std::uint32_t xstate_len = r.u32();
    rr.rip = r.u64();
    rr.rsp = r.u64();
    rr.rflags = r.u64();
    for (auto& g : rr.gpr) g = r.u64();
    rr.fs_base = r.u64();
    rr.gs_base = r.u64();
    rr.cs = r.u16();
    rr.ss = r.u16();
    rr.ds = r.u16();
    rr.es = r.u16();
    rr.fs = r.u16();
    rr.gs = r.u16();
    r.skip(4);  // _pad
    rr.orig_rax = r.u64();
    rr.xcr0 = r.u64();
    auto xstate = r.bytes(xstate_len);
    rr.xstate.assign(xstate.begin(), xstate.end());
    return rr;
}

std::vector<MappingRecord> ImageReader::mappings() const {
    const SectionRef& s = dir_[sidx(Section::Mapping)];
    ByteReader r(span(s.offset, s.length));

    std::vector<MappingRecord> out;
    out.reserve(s.count);
    for (std::uint64_t i = 0; i < s.count; ++i) {
        MappingRecord m;
        m.mapping_id = r.u32();
        m.flags = r.u32();
        m.start_vaddr = r.u64();
        m.end_vaddr = r.u64();
        m.file_offset = r.u64();
        m.dev = r.u64();
        m.ino = r.u64();
        const std::uint32_t path_len = r.u32();
        auto p = r.bytes(path_len);
        m.path.assign(reinterpret_cast<const char*>(p.data()), path_len);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<RegionRecord> ImageReader::regions() const {
    const SectionRef& s = dir_[sidx(Section::Region)];
    ByteReader r(span(s.offset, s.length));

    std::vector<RegionRecord> out;
    out.reserve(s.count);
    for (std::uint64_t i = 0; i < s.count; ++i) {
        RegionRecord rec;
        rec.region_id = r.u32();
        rec.mapping_id = r.u32();
        rec.start_vaddr = r.u64();
        rec.end_vaddr = r.u64();
        rec.page_index_lo = r.u32();
        rec.page_index_hi = r.u32();
        out.push_back(rec);
    }
    return out;
}

std::vector<PageIndexRecord> ImageReader::page_index() const {
    const SectionRef& s = dir_[sidx(Section::PageIndex)];
    ByteReader r(span(s.offset, s.length));

    std::vector<PageIndexRecord> out;
    out.reserve(s.count);
    for (std::uint64_t i = 0; i < s.count; ++i) {
        PageIndexRecord rec;
        rec.vaddr = r.u64();
        rec.store_offset = r.u64();
        rec.store_len = r.u32();
        rec.encoding = static_cast<PageEncoding>(r.u8());
        r.skip(3);  // _pad
        rec.crc32c = r.u32();
        out.push_back(rec);
    }
    return out;
}

std::vector<FdRecord> ImageReader::fds() const {
    const SectionRef& s = dir_[sidx(Section::Fd)];
    ByteReader r(span(s.offset, s.length));

    std::vector<FdRecord> out;
    out.reserve(s.count);
    for (std::uint64_t i = 0; i < s.count; ++i) {
        FdRecord f;
        f.fd = r.u32();
        f.flags = r.u32();
        f.pos = r.u64();
        f.dev = r.u64();
        f.ino = r.u64();
        const std::uint32_t path_len = r.u32();
        auto p = r.bytes(path_len);
        f.path.assign(reinterpret_cast<const char*>(p.data()), path_len);
        out.push_back(std::move(f));
    }
    return out;
}

std::array<std::byte, 32> ImageReader::identity_digest() const {
    std::vector<std::byte> material;
    const auto push = [&material](const void* p, std::size_t n) {
        const auto* b = static_cast<const std::byte*>(p);
        material.insert(material.end(), b, b + n);
    };

    const std::uint32_t page_size = header_.page_size;
    push(&page_size, sizeof page_size);
    push(header_.launch_env_digest.data(), header_.launch_env_digest.size());

    // Layout plus backing-file identity: what the image *is*, not when it was
    // taken. Page contents are excluded; a region's data changes run to run
    // while its identity does not.
    for (const auto& m : mappings()) {
        push(&m.flags, sizeof m.flags);
        push(&m.start_vaddr, sizeof m.start_vaddr);
        push(&m.end_vaddr, sizeof m.end_vaddr);
        push(&m.file_offset, sizeof m.file_offset);
        push(&m.dev, sizeof m.dev);
        push(&m.ino, sizeof m.ino);
        push(m.path.data(), m.path.size());
    }
    for (const auto& r : regions()) {
        push(&r.region_id, sizeof r.region_id);
        push(&r.start_vaddr, sizeof r.start_vaddr);
        push(&r.end_vaddr, sizeof r.end_vaddr);
    }
    return sha256(material);
}

std::span<const std::byte> ImageReader::page_bytes(const PageIndexRecord& rec) const {
    switch (rec.encoding) {
    case PageEncoding::Raw: {
        const SectionRef& pd = dir_[sidx(Section::PageData)];
        if (rec.store_len > pd.length || rec.store_offset > pd.length - rec.store_len)
            throw ImageError("image: page store range out of bounds @ vaddr " + hex(rec.vaddr));
        auto page = span(pd.offset + rec.store_offset, rec.store_len);
        if (crc32c(page) != rec.crc32c)
            throw ImageError("image: page CRC mismatch @ vaddr " + hex(rec.vaddr));
        return page;
    }
    case PageEncoding::Zero: {
        static constexpr std::array<std::byte, 65536> kZeros{};
        if (header_.page_size > kZeros.size())
            throw ImageError("image: page_size exceeds the zero-page buffer");
        return std::span<const std::byte>(kZeros.data(), header_.page_size);
    }
    case PageEncoding::Omitted:
        throw ImageError("image: page_bytes() on an omitted page @ vaddr " + hex(rec.vaddr));
    case PageEncoding::FromFile:
        throw ImageError("image: page @ vaddr " + hex(rec.vaddr) +
                         " is stored by reference; its content lives in the backing file");
    }
    throw ImageError("image: page index record has an unknown encoding");
}

}  // namespace adaptive_restore::image
