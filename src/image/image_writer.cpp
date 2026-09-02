#include "adaptive_restore/image/image_writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "adaptive_restore/common/byte_cursor.hpp"
#include "adaptive_restore/common/crc32c.hpp"
#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::image {

namespace {

constexpr std::size_t kSectionCount = static_cast<std::size_t>(Section::Count);

std::span<const std::byte> as_bytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

// Overwrite little-endian integers already present in a buffer.
void patch_u32(std::vector<std::byte>& b, std::size_t off, std::uint32_t v) {
    for (std::size_t i = 0; i < 4; ++i)
        b[off + i] = std::byte(static_cast<std::uint8_t>(v >> (8 * i)));
}

void patch_u64(std::vector<std::byte>& b, std::size_t off, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i)
        b[off + i] = std::byte(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::span<const std::byte> span_of(const std::vector<std::byte>& b, std::size_t off, std::size_t len) {
    return std::span<const std::byte>(b.data() + off, len);
}

}  // namespace

void ImageWriter::set_header(const ImageHeaderInfo& info) { header_ = info; }

void ImageWriter::set_registers(RegisterRecord regs) { regs_ = std::move(regs); }

std::uint32_t ImageWriter::add_mapping(MappingRecord m) {
    const auto id = static_cast<std::uint32_t>(maps_.size());
    m.mapping_id = id;
    maps_.push_back(std::move(m));
    return id;
}

std::uint32_t ImageWriter::add_region(RegionRecord r) {
    const auto id = static_cast<std::uint32_t>(regions_.size());
    r.region_id = id;
    regions_.push_back(r);
    return id;
}

void ImageWriter::add_page(PageIndexRecord rec, std::span<const std::byte> raw_or_empty) {
    if (rec.encoding == PageEncoding::Raw) {
        if (raw_or_empty.empty())
            throw CheckpointError("ImageWriter::add_page: Raw page with empty payload");
        rec.store_offset = pagestore_.size();
        rec.store_len = static_cast<std::uint32_t>(raw_or_empty.size());
        rec.crc32c = adaptive_restore::crc32c(raw_or_empty);
        pagestore_.insert(pagestore_.end(), raw_or_empty.begin(), raw_or_empty.end());
    } else {
        if (!raw_or_empty.empty())
            throw CheckpointError("ImageWriter::add_page: non-Raw page given a payload");
        rec.store_offset = 0;
        rec.store_len = 0;
        rec.crc32c = 0;
    }
    pidx_.push_back(rec);
}

void ImageWriter::add_fd(FdRecord f) { fds_.push_back(std::move(f)); }

std::vector<std::byte> ImageWriter::serialize_to_bytes() {
    // --- 1. Build each section body ----------------------------------------
    std::vector<std::byte> reg_body;
    {
        ByteWriter w(reg_body);
        w.u32(regs_.record_version);
        w.u32(static_cast<std::uint32_t>(regs_.xstate.size()));
        w.u64(regs_.rip);
        w.u64(regs_.rsp);
        w.u64(regs_.rflags);
        for (std::uint64_t g : regs_.gpr) w.u64(g);
        w.u64(regs_.fs_base);
        w.u64(regs_.gs_base);
        w.u16(regs_.cs);
        w.u16(regs_.ss);
        w.u16(regs_.ds);
        w.u16(regs_.es);
        w.u16(regs_.fs);
        w.u16(regs_.gs);
        w.pad(4);
        w.u64(regs_.orig_rax);
        w.u64(regs_.xcr0);
        w.bytes(regs_.xstate);
    }

    std::vector<std::byte> map_body;
    {
        ByteWriter w(map_body);
        for (const auto& m : maps_) {
            w.u32(m.mapping_id);
            w.u32(m.flags);
            w.u64(m.start_vaddr);
            w.u64(m.end_vaddr);
            w.u64(m.file_offset);
            w.u64(m.dev);
            w.u64(m.ino);
            w.u32(static_cast<std::uint32_t>(m.path.size()));
            w.bytes(as_bytes(m.path));
        }
    }

    std::vector<std::byte> region_body;
    {
        ByteWriter w(region_body);
        for (const auto& r : regions_) {
            w.u32(r.region_id);
            w.u32(r.mapping_id);
            w.u64(r.start_vaddr);
            w.u64(r.end_vaddr);
            w.u32(r.page_index_lo);
            w.u32(r.page_index_hi);
        }
    }

    std::vector<std::byte> pidx_body;
    std::uint64_t raw_page_count = 0;
    {
        ByteWriter w(pidx_body);
        for (const auto& p : pidx_) {
            if (p.encoding == PageEncoding::Raw) ++raw_page_count;
            w.u64(p.vaddr);
            w.u64(p.store_offset);
            w.u32(p.store_len);
            w.u8(static_cast<std::uint8_t>(p.encoding));
            w.pad(3);
            w.u32(p.crc32c);
        }
    }

    std::vector<std::byte> fd_body;
    {
        ByteWriter w(fd_body);
        for (const auto& f : fds_) {
            w.u32(f.fd);
            w.u32(f.flags);
            w.u64(f.pos);
            w.u64(f.dev);
            w.u64(f.ino);
            w.u32(static_cast<std::uint32_t>(f.path.size()));
            w.bytes(as_bytes(f.path));
        }
    }

    // Integrity body: u32 full_file_crc32c (patched later) + u64 page_data_crc32c.
    constexpr std::size_t kIntegrityBodySize = 12;

    // --- 2. Section directory --------------------------------------------
    SectionRef dir[kSectionCount];
    std::uint64_t off = kHeaderSize;

    dir[static_cast<std::size_t>(Section::Register)] = {off, reg_body.size(), 1};
    off += reg_body.size();
    dir[static_cast<std::size_t>(Section::Mapping)] = {off, map_body.size(), maps_.size()};
    off += map_body.size();
    dir[static_cast<std::size_t>(Section::Region)] = {off, region_body.size(), regions_.size()};
    off += region_body.size();
    dir[static_cast<std::size_t>(Section::PageIndex)] = {off, pidx_body.size(), pidx_.size()};
    off += pidx_body.size();
    dir[static_cast<std::size_t>(Section::PageData)] = {off, pagestore_.size(), raw_page_count};
    off += pagestore_.size();
    dir[static_cast<std::size_t>(Section::Fd)] = {off, fd_body.size(), fds_.size()};
    off += fd_body.size();
    dir[static_cast<std::size_t>(Section::Integrity)] = {off, kIntegrityBodySize, 1};

    // --- 3. Header (kHeaderSize bytes) -----------------------------------
    std::vector<std::byte> buf;
    std::size_t metadata_crc32c_offset = 0;
    {
        ByteWriter w(buf);
        w.bytes({reinterpret_cast<const std::byte*>(kImageMagic), 8});
        w.u32(static_cast<std::uint32_t>(kImageFormatVersion));
        w.u32(static_cast<std::uint32_t>(kHeaderSize));
        w.u16(1);  // arch = x86-64
        w.u8(1);   // endianness = little
        w.pad(1);  // _pad0
        w.u32(header_.page_size);

        if (header_.kernel_release.size() >= kKernelReleaseSize)
            throw CheckpointError("ImageWriter: kernel_release exceeds " +
                                  std::to_string(kKernelReleaseSize - 1) + " chars: \"" +
                                  header_.kernel_release + "\"");
        const std::size_t kr_n = header_.kernel_release.size();
        w.bytes({reinterpret_cast<const std::byte*>(header_.kernel_release.data()), kr_n});
        w.pad(kKernelReleaseSize - kr_n);
        w.pad(3);  // _pad1

        w.u32(header_.original_pid);
        w.u64(header_.checkpoint_time_ns);
        w.bytes(header_.launch_env_digest);

        for (const auto& s : dir) {
            w.u64(s.offset);
            w.u64(s.length);
            w.u64(s.count);
        }

        metadata_crc32c_offset = w.size();
        w.u32(0);  // metadata_crc32c, back-patched below

        w.pad(kHeaderSize - w.size());  // _reserved
    }

    // --- 4. Concatenate bodies in directory order ----------------------
    buf.insert(buf.end(), reg_body.begin(), reg_body.end());
    buf.insert(buf.end(), map_body.begin(), map_body.end());
    buf.insert(buf.end(), region_body.begin(), region_body.end());
    buf.insert(buf.end(), pidx_body.begin(), pidx_body.end());

    const std::size_t pagedata_offset = buf.size();
    buf.insert(buf.end(), pagestore_.begin(), pagestore_.end());
    const std::size_t pagedata_len = buf.size() - pagedata_offset;

    const std::size_t fd_offset = buf.size();
    buf.insert(buf.end(), fd_body.begin(), fd_body.end());
    const std::size_t fd_len = buf.size() - fd_offset;

    const std::size_t integrity_offset = buf.size();
    const std::size_t full_file_crc32c_offset = integrity_offset;         // u32, first field
    const std::size_t page_data_crc32c_offset = integrity_offset + 4;     // u64, second field
    {
        ByteWriter w(buf);
        w.u32(0);  // full_file_crc32c, back-patched below
        w.u64(0);  // page_data_crc32c, back-patched below
    }

    // --- 5. Back-patch checksums, in order ----------------------------
    // (a) page_data_crc32c over the page data store span.
    const std::uint32_t page_data_crc = adaptive_restore::crc32c(
        span_of(buf, pagedata_offset, pagedata_len));
    patch_u64(buf, page_data_crc32c_offset, page_data_crc);

    // (b) metadata_crc32c over header (its own field zeroed) + register +
    //     mapping + region + pageindex + fd sections. Page data lies between
    //     the page index and the fd section, so the covered range is
    //     discontiguous: [0, pagedata_offset) then [fd_offset, fd_offset+fd_len).
    std::uint32_t meta_crc = adaptive_restore::crc32c(span_of(buf, 0, pagedata_offset));
    meta_crc = adaptive_restore::crc32c(span_of(buf, fd_offset, fd_len), meta_crc);
    patch_u32(buf, metadata_crc32c_offset, meta_crc);

    // (c) full_file_crc32c over the entire buffer with its own field still zero.
    const std::uint32_t full_crc = adaptive_restore::crc32c(span_of(buf, 0, buf.size()));
    patch_u32(buf, full_file_crc32c_offset, full_crc);

    return buf;
}

void ImageWriter::finish(const std::filesystem::path& out) {
    const std::vector<std::byte> bytes = serialize_to_bytes();

    std::filesystem::path tmp = out;
    tmp += ".tmp";

    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw CheckpointError("ImageWriter::finish: open(" + tmp.string() +
                              "): " + std::strerror(errno));

    // Any failure past this point must not leave the temp file behind. `out`
    // itself is still never a partial image (it only appears via rename).
    struct TmpGuard {
        const std::filesystem::path& path;
        bool armed = true;
        ~TmpGuard() {
            if (armed) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
    } tmp_guard{tmp};

    const std::byte* p = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        const ssize_t n = ::write(fd, p, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            const int e = errno;
            ::close(fd);
            throw CheckpointError("ImageWriter::finish: write(" + tmp.string() +
                                  "): " + std::strerror(e));
        }
        p += n;
        remaining -= static_cast<std::size_t>(n);
    }

    if (::fsync(fd) != 0) {
        const int e = errno;
        ::close(fd);
        throw CheckpointError("ImageWriter::finish: fsync(" + tmp.string() +
                              "): " + std::strerror(e));
    }
    if (::close(fd) != 0)
        throw CheckpointError("ImageWriter::finish: close(" + tmp.string() +
                              "): " + std::strerror(errno));

    std::error_code ec;
    std::filesystem::rename(tmp, out, ec);
    if (ec)
        throw CheckpointError("ImageWriter::finish: rename(" + tmp.string() + " -> " +
                              out.string() + "): " + ec.message());

    tmp_guard.armed = false;  // tmp no longer exists — rename consumed it
}

}  // namespace adaptive_restore::image
