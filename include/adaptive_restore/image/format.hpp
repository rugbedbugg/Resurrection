#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
// re-exported: consumers of this header reach kImageMagic / kImageFormatVersion
// through it (clangd may flag this include as unused — it is not; the image
// roundtrip test relies on the transitive reach).
#include "adaptive_restore/common/version.hpp"

// Image format v1 record structs + wire constants.
// See docs/specs/phase1-checkpoint-capture-design.md §"Image format v1".
//
// NOTE (deviation from the task brief): the brief specifies kHeaderSize == 256,
// but the header's own field list (136-byte fixed prefix + SectionRef[7] at
// 24 bytes each + a u32 metadata_crc32c) is 308 bytes and cannot fit in 256.
// SectionRef is the authoritative 3x u64 struct, so the header grows instead:
// kHeaderSize == 320 (308 rounded up to a 64-byte boundary). The on-wire
// header_size field is written so any reader learns the true size rather than
// assuming it.

namespace adaptive_restore::image {

// One entry in the header's section directory: byte offset, byte length, and
// record count of a section, all absolute / little-endian on the wire.
//
// `count` semantics per section:
//   Register  = 1 (always one record)
//   Mapping   = number of MappingRecord
//   Region    = number of RegionRecord
//   PageIndex = number of PageIndexRecord
//   PageData  = number of Raw page payloads (NOT a byte or record count)
//   Fd        = number of FdRecord
//   Integrity = 1
struct SectionRef {
    std::uint64_t offset = 0, length = 0, count = 0;
};

// Section directory order (index into the header's SectionRef[7]).
enum class Section : std::uint32_t {
    Register = 0,
    Mapping = 1,
    Region = 2,
    PageIndex = 3,
    PageData = 4,
    Fd = 5,
    Integrity = 6,
    Count = 7,
};

// Captured CPU state. GPR order: rax,rbx,rcx,rdx,rsi,rdi,rbp,r8,r9,r10,r11,r12,
// r13,r14,r15 (index 0..15). xstate is the opaque NT_X86_XSTATE blob.
struct RegisterRecord {
    std::uint32_t record_version = 1;
    std::uint64_t rip = 0, rsp = 0, rflags = 0;
    std::uint64_t gpr[16] = {};
    std::uint64_t fs_base = 0, gs_base = 0;
    std::uint16_t cs = 0, ss = 0, ds = 0, es = 0, fs = 0, gs = 0;
    std::uint64_t orig_rax = 0, xcr0 = 0;
    std::vector<std::byte> xstate;
};

// One VMA from /proc/<pid>/maps.
struct MappingRecord {
    std::uint32_t mapping_id = 0;
    std::uint32_t flags = 0;
    std::uint64_t start_vaddr = 0, end_vaddr = 0, file_offset = 0, dev = 0, ino = 0;
    std::string path;
};

// Bits for MappingRecord::flags.
enum MappingFlag : std::uint32_t {
    Read = 1u << 0,
    Write = 1u << 1,
    Exec = 1u << 2,
    Shared = 1u << 3,
    FileBacked = 1u << 4,
    KernelSpecial = 1u << 5,
    Unreadable = 1u << 6,
    ContentsSaved = 1u << 7,
};

// A mechanical subdivision of an eligible mapping (kDefaultRegionBytes each).
struct RegionRecord {
    std::uint32_t region_id = 0, mapping_id = 0;
    std::uint64_t start_vaddr = 0, end_vaddr = 0;
    std::uint32_t page_index_lo = 0, page_index_hi = 0;
};

enum class PageEncoding : std::uint8_t {
    Raw = 0,
    Zero = 1,
    Omitted = 2,
    // Byte-identical to the backing file at
    // mapping.file_offset + (vaddr - mapping.start_vaddr), so no bytes are
    // stored: creating the mapping from that file already supplies the page
    // through the page cache. crc32c still covers the real content, so a
    // restore can verify it. Only valid inside a FileBacked mapping.
    FromFile = 3,
};

// One page of the address space. For Raw pages store_offset/store_len/crc32c are
// filled by the writer; for Zero/Omitted/FromFile store_offset and store_len are
// 0 (FromFile still carries a meaningful crc32c).
struct PageIndexRecord {
    std::uint64_t vaddr = 0, store_offset = 0;
    std::uint32_t store_len = 0;
    PageEncoding encoding = PageEncoding::Raw;
    std::uint32_t crc32c = 0;
};

// A supported regular-file descriptor.
struct FdRecord {
    std::uint32_t fd = 0, flags = 0;
    std::uint64_t pos = 0, dev = 0, ino = 0;
    std::string path;
};

// Everything the writer needs for the fixed header prefix.
struct ImageHeaderInfo {
    std::uint32_t page_size = 0;
    std::string kernel_release;
    std::uint32_t original_pid = 0;
    std::uint64_t checkpoint_time_ns = 0;
    std::array<std::byte, 32> launch_env_digest = {};
};

// Fixed image header size, in bytes. See the deviation note above.
inline constexpr std::size_t kHeaderSize = 320;

// Bytes of the header before the section directory: magic(8) + format_version(4)
// + header_size(4) + arch(2) + endianness(1) + _pad0(1) + page_size(4)
// + kernel_release(65) + _pad1(3) + original_pid(4) + checkpoint_time_ns(8)
// + launch_env_digest(32) = 136.
inline constexpr std::size_t kFixedPrefixSize = 136;

// char[65] slot for uname -r in the header (64 chars + NUL).
inline constexpr std::size_t kKernelReleaseSize = 65;

static_assert(static_cast<int>(Section::Count) == 7);
// SectionRef is 3 plain u64 with no padding; its wire size equals sizeof().
static_assert(sizeof(SectionRef) == 24);
// The header must actually hold its fields: fixed prefix + SectionRef[Count] +
// the u32 metadata_crc32c. This fails loudly if a later task grows the header
// past kHeaderSize (which would also underflow `w.pad(kHeaderSize - w.size())`
// in the writer). This is the guard that a bare `kHeaderSize == 320` was not.
static_assert(kFixedPrefixSize
                  + sizeof(SectionRef) * static_cast<std::size_t>(Section::Count)
                  + sizeof(std::uint32_t)
              <= kHeaderSize,
              "image header fields no longer fit kHeaderSize");

}  // namespace adaptive_restore::image
