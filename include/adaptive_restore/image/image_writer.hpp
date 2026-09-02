#pragma once
#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>
#include "adaptive_restore/image/format.hpp"

namespace adaptive_restore::image {

// Builds an image format v1 buffer from captured process state and writes it
// transactionally (tmp file + fsync + rename).
class ImageWriter {
public:
    void set_header(const ImageHeaderInfo& info);
    void set_registers(RegisterRecord regs);

    // Assigns and returns the mapping_id / region_id (assignment order).
    std::uint32_t add_mapping(MappingRecord m);
    std::uint32_t add_region(RegionRecord r);

    // For PageEncoding::Raw, raw_or_empty is the page payload and its
    // crc32c/store_offset/store_len are filled in here. For Zero/Omitted pass
    // an empty span.
    void add_page(PageIndexRecord rec, std::span<const std::byte> raw_or_empty);

    void add_fd(FdRecord f);

    // Serialize to <out>.tmp, fsync, then rename to out. Throws CheckpointError
    // on any I/O failure.
    void finish(const std::filesystem::path& out);

    // The exact bytes finish() would write.
    std::vector<std::byte> serialize_to_bytes();

private:
    ImageHeaderInfo header_{};
    RegisterRecord regs_{};
    std::vector<MappingRecord> maps_;
    std::vector<RegionRecord> regions_;
    std::vector<PageIndexRecord> pidx_;
    std::vector<std::byte> pagestore_;
    std::vector<FdRecord> fds_;
};

}  // namespace adaptive_restore::image
