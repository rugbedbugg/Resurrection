#include "adaptive_restore/checkpoint/maps.hpp"

#include <sys/sysmacros.h>

#include <fstream>
#include <sstream>
#include <string>

#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::checkpoint {

namespace {

[[noreturn]] void bad_line(std::string_view line, const char* what) {
    throw CheckpointError(std::string("maps: ") + what + " in line: \"" + std::string(line) + '"');
}

void skip_spaces(std::string_view& c) {
    std::size_t i = 0;
    while (i < c.size() && (c[i] == ' ' || c[i] == '\t')) ++i;
    c.remove_prefix(i);
}

// Consume one run of digits in `base` (16 or 10). Throws if there is none.
std::uint64_t take_number(std::string_view& c, std::string_view line, unsigned base) {
    std::uint64_t v = 0;
    std::size_t n = 0;
    while (n < c.size()) {
        const char ch = c[n];
        unsigned d;
        if (ch >= '0' && ch <= '9') d = static_cast<unsigned>(ch - '0');
        else if (base == 16 && ch >= 'a' && ch <= 'f') d = static_cast<unsigned>(ch - 'a') + 10;
        else if (base == 16 && ch >= 'A' && ch <= 'F') d = static_cast<unsigned>(ch - 'A') + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        ++n;
    }
    if (n == 0) bad_line(line, base == 16 ? "expected a hex number" : "expected a decimal number");
    c.remove_prefix(n);
    return v;
}

void expect(std::string_view& c, char want, std::string_view line, const char* what) {
    if (c.empty() || c.front() != want) bad_line(line, what);
    c.remove_prefix(1);
}

Mapping parse_line(std::string_view line) {
    std::string_view c = line;
    Mapping m;

    m.start = take_number(c, line, 16);
    expect(c, '-', line, "expected '-' between the address bounds");
    m.end = take_number(c, line, 16);
    if (m.end < m.start) bad_line(line, "end address precedes start address");

    if (c.empty() || (c.front() != ' ' && c.front() != '\t'))
        bad_line(line, "expected whitespace before the permission field");
    skip_spaces(c);

    if (c.size() < 4) bad_line(line, "truncated permission field");
    const std::string_view perms = c.substr(0, 4);
    if ((perms[0] != 'r' && perms[0] != '-') || (perms[1] != 'w' && perms[1] != '-') ||
        (perms[2] != 'x' && perms[2] != '-') || (perms[3] != 's' && perms[3] != 'p'))
        bad_line(line, "malformed permission field");
    m.r = perms[0] == 'r';
    m.w = perms[1] == 'w';
    m.x = perms[2] == 'x';
    m.shared = perms[3] == 's';
    c.remove_prefix(4);

    skip_spaces(c);
    m.file_offset = take_number(c, line, 16);

    skip_spaces(c);
    const std::uint64_t major = take_number(c, line, 16);
    expect(c, ':', line, "expected ':' in the device field");
    const std::uint64_t minor = take_number(c, line, 16);
    m.dev = (major == 0 && minor == 0)
                ? 0
                : ::makedev(static_cast<unsigned>(major), static_cast<unsigned>(minor));

    skip_spaces(c);
    m.ino = take_number(c, line, 10);

    // Everything after the inode is the pathname column: optional, may itself
    // contain spaces, so only the surrounding whitespace is trimmed.
    skip_spaces(c);
    while (!c.empty() && (c.back() == ' ' || c.back() == '\t')) c.remove_suffix(1);
    m.path.assign(c);

    // Classification, in the order documented in maps.hpp.
    const bool bracketed = m.path.size() >= 2 && m.path.front() == '[' && m.path.back() == ']';
    if (bracketed) m.tag = m.path;

    // The vvar family is matched by prefix, not by exact name: kernels since
    // 6.13 split the vvar page and expose [vvar_vclock] alongside [vvar], and
    // both are kernel-mapped pages that must never be saved or re-mapped.
    const bool vvar_family = m.path.starts_with("[vvar");
    if (vvar_family || m.path == "[vdso]" || m.path == "[vsyscall]") {
        m.kind = MapKind::KernelSpecial;
    } else if (!m.r && !m.x) {
        m.kind = MapKind::GuardNone;
    } else if (bracketed) {
        // [heap], [stack], [stack:tid] and friends are kernel-named but are
        // ordinary private anonymous memory, so they are captured as such.
        m.kind = MapKind::AnonPrivate;
    } else if (!m.path.empty() || m.dev != 0 || m.ino != 0) {
        m.kind = MapKind::FileBacked;
    } else {
        m.kind = MapKind::AnonPrivate;
    }
    return m;
}

}  // namespace

std::vector<Mapping> parse_maps_text(std::string_view maps) {
    std::vector<Mapping> out;
    std::size_t pos = 0;
    while (pos < maps.size()) {
        const std::size_t nl = maps.find('\n', pos);
        std::string_view line =
            maps.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? maps.size() : nl + 1;

        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;

        out.push_back(parse_line(line));
    }
    return out;
}

std::vector<Mapping> read_maps(pid_t pid) {
    const std::string path = "/proc/" + std::to_string(pid) + "/maps";
    std::ifstream f(path, std::ios::binary);
    if (!f) throw CheckpointError("maps: cannot open " + path);

    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) throw CheckpointError("maps: read error on " + path);

    return parse_maps_text(ss.str());
}

}  // namespace adaptive_restore::checkpoint
