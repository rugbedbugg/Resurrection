#include <cassert>
#include <cstdio>
#include "adaptive_restore/checkpoint/maps.hpp"
#include "adaptive_restore/common/error.hpp"
using namespace adaptive_restore::checkpoint;

static const char* kMaps =
"00400000-00452000 r-xp 00000000 08:02 173521      /usr/bin/cat\n"
"0071b000-0071c000 rw-p 0001b000 08:02 173521      /usr/bin/cat\n"
"01e30000-01e51000 rw-p 00000000 00:00 0           [heap]\n"
"7ffff7a0d000-7ffff7bcd000 r-xp 00000000 08:02 1     /usr/lib/libc.so.6\n"
"7ffffffde000-7ffffffff000 rw-p 00000000 00:00 0     [stack]\n"
"7ffff7ffc000-7ffff7ffd000 r--p 00000000 00:00 0     [vvar]\n"
"7ffff7ffd000-7ffff7fff000 r-xp 00000000 00:00 0     [vdso]\n"
"ffffffffff600000-ffffffffff601000 --xp 00000000 00:00 0  [vsyscall]\n";

int main() {
    auto v = parse_maps_text(kMaps);
    assert(v.size() == 8);
    assert(v[0].kind == MapKind::FileBacked && v[0].x && !v[0].w);
    assert(v[2].kind == MapKind::AnonPrivate && v[2].tag == "[heap]");
    assert(v[3].kind == MapKind::FileBacked && v[3].path == "/usr/lib/libc.so.6");
    assert(v[4].tag == "[stack]" && v[4].kind == MapKind::AnonPrivate);
    assert(v[5].kind == MapKind::KernelSpecial);   // [vvar]
    assert(v[6].kind == MapKind::KernelSpecial);   // [vdso]
    assert(v[7].kind == MapKind::KernelSpecial);   // [vsyscall]

    // Field-level checks on the first record.
    assert(v[0].start == 0x00400000 && v[0].end == 0x00452000);
    assert(v[0].r && !v[0].shared && v[0].file_offset == 0);
    assert(v[0].ino == 173521 && v[0].dev != 0);
    assert(v[0].path == "/usr/bin/cat" && v[0].tag.empty());
    assert(v[1].file_offset == 0x1b000 && v[1].w && !v[1].x);

    // [vvar_vclock] appears alongside [vvar] on kernels since 6.13 and is just
    // as kernel-managed, so the whole vvar family classifies as KernelSpecial.
    auto vclock = parse_maps_text(
        "7f52c0980000-7f52c0982000 r--p 00000000 00:00 0    [vvar_vclock]\n");
    assert(vclock.size() == 1);
    assert(vclock[0].kind == MapKind::KernelSpecial && vclock[0].tag == "[vvar_vclock]");

    // Anonymous private memory: no path, no dev/ino, no tag.
    auto anon = parse_maps_text("7f0000000000-7f0000001000 rw-p 00000000 00:00 0\n");
    assert(anon.size() == 1);
    assert(anon[0].kind == MapKind::AnonPrivate);
    assert(anon[0].path.empty() && anon[0].tag.empty() && anon[0].dev == 0 && anon[0].ino == 0);

    // A no-access guard page classifies as GuardNone.
    auto guard = parse_maps_text("7f0000001000-7f0000002000 ---p 00000000 00:00 0\n");
    assert(guard.size() == 1 && guard[0].kind == MapKind::GuardNone);

    // A shared file mapping keeps its shared bit.
    auto shm = parse_maps_text("7f0000002000-7f0000003000 rw-s 00000000 00:05 42   /dev/zero\n");
    assert(shm.size() == 1 && shm[0].shared && shm[0].kind == MapKind::FileBacked);

    // Blank lines are skipped; a malformed line throws.
    assert(parse_maps_text("\n\n").empty());
    bool threw = false;
    try {
        parse_maps_text("not-a-mapping-line\n");
    } catch (const adaptive_restore::CheckpointError&) {
        threw = true;
    }
    assert(threw && "malformed maps line must throw CheckpointError");

    std::puts("test_maps_classify: ok");
    return 0;
}
