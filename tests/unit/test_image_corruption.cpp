#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/common/error.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;

static std::vector<std::byte> good_image() {
    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "k";
    w.set_header(h);
    w.set_registers(RegisterRecord{});
    auto m = w.add_mapping({0, MappingFlag::Read | MappingFlag::ContentsSaved,
                            0x1000, 0x2000, 0, 0, 0, ""});
    w.add_region({0, m, 0x1000, 0x2000, 0, 1});
    std::vector<std::byte> pg(4096, std::byte{0x11});
    w.add_page({0x1000, 0, 0, PageEncoding::Raw, 0}, pg);
    return w.serialize_to_bytes();
}

static bool rejects(std::vector<std::byte> b) {
    try {
        ImageReader::from_bytes(std::move(b));
        return false;
    } catch (const ImageError&) {
        return true;
    }
}

int main() {
    assert(!rejects(good_image()));  // sanity: a clean image is accepted

    auto b1 = good_image();
    b1[0] = std::byte{0};  // break magic
    assert(rejects(std::move(b1)));

    auto b2 = good_image();
    b2.resize(b2.size() - 10);  // truncate into the integrity section
    assert(rejects(std::move(b2)));

    auto b3 = good_image();
    b3[5] ^= std::byte{0xFF};  // corrupt a byte of the magic/version prefix
    assert(rejects(std::move(b3)));

    auto b4 = good_image();
    b4.back() ^= std::byte{0xFF};  // flip the last byte -> whole-file CRC fails
    assert(rejects(std::move(b4)));

    auto b5 = good_image();
    b5[300] ^= std::byte{0xFF};  // flip a metadata byte -> metadata CRC fails
    assert(rejects(std::move(b5)));

    // A clean image parses; page_bytes() then guards each page with its own CRC.
    auto ir = ImageReader::from_bytes(good_image());
    auto pidx = ir.page_index();
    assert(pidx.size() == 1);
    assert(ir.page_bytes(pidx[0]).size() == 4096);  // clean page: no throw

    PageIndexRecord bogus = pidx[0];
    bogus.crc32c ^= 0xFFFFFFFFu;
    bool page_threw = false;
    try {
        ir.page_bytes(bogus);
    } catch (const ImageError&) {
        page_threw = true;
    }
    assert(page_threw && "page_bytes must reject a CRC mismatch");

    std::puts("test_image_corruption: ok");
    return 0;
}
