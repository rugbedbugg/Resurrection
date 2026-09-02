#include <cassert>
#include <cstdio>
#include <cstring>
#include "adaptive_restore/image/format.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/common/byte_cursor.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;

int main() {
    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "test-kernel";
    h.original_pid = 1234;
    h.checkpoint_time_ns = 42;
    w.set_header(h);

    RegisterRecord rr{};
    rr.rip = 0x400123; rr.rsp = 0x7fffffffe000; rr.fs_base = 0xdeadbeef;
    rr.xstate = std::vector<std::byte>(64, std::byte{0xAB});
    w.set_registers(rr);

    auto mid = w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                              0x400000, 0x401000, 0, 0, 0, ""});
    assert(mid == 0);
    w.add_region({0, mid, 0x400000, 0x401000, 0, 1});

    std::vector<std::byte> page(4096, std::byte{0x5A});
    w.add_page({0x400000, 0, 0, PageEncoding::Raw, 0}, page);

    w.add_fd({1, 0x1, 0, 0, 0, "/tmp/out"});

    auto bytes = w.serialize_to_bytes();
    // Header: magic + version at the front.
    assert(bytes.size() > kHeaderSize);
    assert(std::memcmp(bytes.data(), kImageMagic, 8) == 0);
    ByteReader r({bytes.data() + 8, bytes.size() - 8});
    assert(r.u32() == kImageFormatVersion);

    std::puts("test_image_roundtrip (writer): ok");
    return 0;
}
