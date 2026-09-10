#include <cinttypes>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>

#include "adaptive_restore/cli/commands.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::cli {

namespace {

std::string perms(std::uint32_t flags) {
    std::string s = "----";
    if (flags & image::MappingFlag::Read) s[0] = 'r';
    if (flags & image::MappingFlag::Write) s[1] = 'w';
    if (flags & image::MappingFlag::Exec) s[2] = 'x';
    s[3] = (flags & image::MappingFlag::Shared) ? 's' : 'p';
    return s;
}

const char* kind(std::uint32_t flags) {
    if (flags & image::MappingFlag::KernelSpecial) return "kernel";
    if (flags & image::MappingFlag::Unreadable) return "unread";
    if (flags & image::MappingFlag::FileBacked) return "file";
    return "anon";
}

std::string hex(std::uint64_t v, int width = 12) {
    char b[32];
    std::snprintf(b, sizeof b, "%0*" PRIx64, width, v);
    return std::string(b);
}

std::string digest_hex(const std::array<std::byte, 32>& d) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (std::byte b : d) {
        const auto v = std::to_integer<unsigned>(b);
        s += k[v >> 4];
        s += k[v & 15];
    }
    return s;
}

}  // namespace

int cmd_inspect(std::span<const std::string_view> args) {
    std::string path;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--image") {
            if (i + 1 >= args.size()) {
                std::cerr << "arctl inspect: --image needs a value\n";
                return 1;
            }
            path = args[++i];
        } else {
            std::cerr << "arctl inspect: unknown option '" << args[i] << "'\n";
            return 1;
        }
    }
    if (path.empty()) {
        std::cerr << "arctl inspect: usage: arctl inspect --image <image>\n";
        return 1;
    }

    try {
        const auto ir = image::ImageReader::open(path);

        const auto& h = ir.header();
        std::cout << "image " << path << "\n"
                  << "  page size     " << h.page_size << '\n'
                  << "  kernel        " << h.kernel_release << '\n'
                  << "  original pid  " << h.original_pid << '\n'
                  << "  captured at   " << h.checkpoint_time_ns << " ns\n"
                  << "  launch digest " << digest_hex(h.launch_env_digest) << "\n\n";

        const auto r = ir.registers();
        std::cout << "registers\n"
                  << "  rip     0x" << hex(r.rip) << "    rsp     0x" << hex(r.rsp) << '\n'
                  << "  rflags  0x" << hex(r.rflags, 8) << "        fs_base 0x" << hex(r.fs_base)
                  << '\n'
                  << "  xcr0    0x" << hex(r.xcr0, 8) << "        xstate  " << r.xstate.size()
                  << " bytes\n\n";

        const auto maps = ir.mappings();
        std::cout << "mappings (" << maps.size() << ")\n";
        for (const auto& m : maps) {
            const bool saved = (m.flags & image::MappingFlag::ContentsSaved) != 0;
            std::cout << "  " << std::setw(3) << m.mapping_id << "  " << hex(m.start_vaddr) << '-'
                      << hex(m.end_vaddr) << "  " << perms(m.flags) << "  " << std::setw(6)
                      << kind(m.flags) << "  " << (saved ? "saved" : "     ") << "  " << m.path
                      << '\n';
        }

        const auto regions = ir.regions();
        std::cout << "\nregions (" << regions.size() << ")\n";
        if (!regions.empty()) {
            const auto& f = regions.front();
            const auto& l = regions.back();
            std::cout << "  first  id " << f.region_id << "  " << hex(f.start_vaddr) << '-'
                      << hex(f.end_vaddr) << "  pages [" << f.page_index_lo << ','
                      << f.page_index_hi << ")\n"
                      << "  last   id " << l.region_id << "  " << hex(l.start_vaddr) << '-'
                      << hex(l.end_vaddr) << "  pages [" << l.page_index_lo << ','
                      << l.page_index_hi << ")\n";
        }

        std::uint64_t raw = 0, zero = 0, omitted = 0, stored = 0;
        for (const auto& p : ir.page_index()) {
            switch (p.encoding) {
            case image::PageEncoding::Raw:
                ++raw;
                stored += p.store_len;
                break;
            case image::PageEncoding::Zero: ++zero; break;
            case image::PageEncoding::Omitted: ++omitted; break;
            }
        }
        std::cout << "\npages (" << (raw + zero + omitted) << ")\n"
                  << "  raw      " << raw << "  (" << stored << " bytes stored)\n"
                  << "  zero     " << zero << '\n'
                  << "  omitted  " << omitted << '\n';

        const auto fds = ir.fds();
        std::cout << "\nfds (" << fds.size() << ")\n";
        for (const auto& f : fds)
            std::cout << "  " << std::setw(3) << f.fd << "  pos " << std::setw(10) << f.pos
                      << "  flags 0" << std::oct << f.flags << std::dec << "  " << f.path << '\n';
    } catch (const std::exception& e) {
        std::cerr << "arctl inspect: " << e.what() << '\n';
        return 1;
    }
    return 0;
}

}  // namespace adaptive_restore::cli
