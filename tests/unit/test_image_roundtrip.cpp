#include <cassert>
#include <cstdio>
#include <cstring>
#include "adaptive_restore/image/format.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/image/image_reader.hpp"
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

    // Reader half: parse it back and check every section survived the round trip.
    auto ir = ImageReader::from_bytes(bytes);
    ir.verify();  // must not throw
    assert(ir.header().page_size == 4096);
    assert(ir.header().kernel_release == "test-kernel");
    assert(ir.header().original_pid == 1234);
    assert(ir.header().checkpoint_time_ns == 42);

    auto got_regs = ir.registers();
    assert(got_regs.rip == 0x400123 && got_regs.fs_base == 0xdeadbeef);
    assert(got_regs.rsp == 0x7fffffffe000);
    assert(got_regs.xstate.size() == 64 && got_regs.xstate[0] == std::byte{0xAB});

    auto ms = ir.mappings();
    assert(ms.size() == 1 && ms[0].start_vaddr == 0x400000 && ms[0].end_vaddr == 0x401000);
    assert(ms[0].flags == (MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved));

    auto rs = ir.regions();
    assert(rs.size() == 1 && rs[0].mapping_id == mid && rs[0].page_index_hi == 1);

    auto ridx = ir.page_index();
    assert(ridx.size() == 1 && ridx[0].encoding == PageEncoding::Raw);
    auto pb = ir.page_bytes(ridx[0]);
    assert(pb.size() == 4096 && pb[0] == std::byte{0x5A} && pb[4095] == std::byte{0x5A});

    auto fs = ir.fds();
    assert(fs.size() == 1 && fs[0].fd == 1 && fs[0].path == "/tmp/out");

    std::puts("test_image_roundtrip: ok");
    return 0;
}
