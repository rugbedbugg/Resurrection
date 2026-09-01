#include <cassert>
#include <cstdio>
#include <span>
#include <vector>
#include "adaptive_restore/common/byte_cursor.hpp"
#include "adaptive_restore/common/error.hpp"

using adaptive_restore::ByteReader;
using adaptive_restore::ByteWriter;

int main() {
    std::vector<std::byte> buf;
    {
        ByteWriter w(buf);
        w.u8(0x12);
        w.u16(0x3456);
        w.u32(0x789ABCDEu);
        w.u64(0x0102030405060708ull);
        w.pad(3);
    }
    // 1 + 2 + 4 + 8 + 3 = 18 bytes, little-endian
    assert(buf.size() == 18);
    assert(buf[0] == std::byte{0x12});
    assert(buf[1] == std::byte{0x56} && buf[2] == std::byte{0x34});
    assert(buf[3] == std::byte{0xDE} && buf[6] == std::byte{0x78});
    assert(buf[7] == std::byte{0x08} && buf[14] == std::byte{0x01});
    assert(buf[15] == std::byte{0} && buf[17] == std::byte{0});

    ByteReader r(buf);
    assert(r.u8() == 0x12);
    assert(r.u16() == 0x3456);
    assert(r.u32() == 0x789ABCDEu);
    assert(r.u64() == 0x0102030405060708ull);
    r.skip(3);
    assert(r.remaining() == 0);

    bool threw = false;
    try { r.u8(); } catch (const adaptive_restore::ImageError&) { threw = true; }
    assert(threw && "read past end must throw ImageError");

    std::puts("test_byte_cursor: ok");
    return 0;
}
