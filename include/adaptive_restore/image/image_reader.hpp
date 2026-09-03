#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>
#include "adaptive_restore/image/format.hpp"

namespace adaptive_restore::image {

// Parses and validates an image format v1 buffer. Construction runs verify(),
// so a successfully constructed ImageReader is a structurally sound image;
// every accessor below then re-parses its section on demand.
class ImageReader {
public:
    // Read the whole file into memory, then from_bytes(). Throws ImageError on
    // an I/O problem or any validation failure.
    static ImageReader open(const std::filesystem::path& path);

    // Take ownership of an in-memory image and validate it. Throws ImageError
    // naming the first problem found.
    static ImageReader from_bytes(std::vector<std::byte> bytes);

    // Re-run every structural + integrity check. Throws ImageError on the first
    // failure. Called by open()/from_bytes(); public so a caller can re-assert.
    void verify() const;

    const ImageHeaderInfo& header() const { return header_; }
    RegisterRecord registers() const;
    std::vector<MappingRecord> mappings() const;
    std::vector<RegionRecord> regions() const;
    std::vector<PageIndexRecord> page_index() const;
    std::vector<FdRecord> fds() const;

    // Decoded contents of one page. Raw: the stored payload, its crc32c
    // re-checked (throws on mismatch). Zero: a page_size span of zero bytes.
    // Omitted: throws ImageError.
    std::span<const std::byte> page_bytes(const PageIndexRecord& rec) const;

private:
    ImageReader() = default;

    void parse_header();  // fills header_ + dir_; assumes buf_.size() >= kHeaderSize

    std::span<const std::byte> span(std::uint64_t offset, std::uint64_t length) const;
    std::uint32_t read_u32(std::uint64_t offset) const;
    std::uint64_t read_u64(std::uint64_t offset) const;

    std::vector<std::byte> buf_;
    ImageHeaderInfo header_{};
    SectionRef dir_[static_cast<std::size_t>(Section::Count)]{};
};

}  // namespace adaptive_restore::image
