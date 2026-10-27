#include "adaptive_restore/restore/address_space.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "adaptive_restore/checkpoint/maps.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/restore/compel_compat.hpp"

namespace adaptive_restore::restore {

namespace {

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

bool overlaps(std::uint64_t a0, std::uint64_t a1, std::uint64_t b0, std::uint64_t b1) {
    return a0 < b1 && b0 < a1;
}

// The target needs the path in its own address space to call openat, so it is
// staged into the reserve. Paths are bounded by the reserve's spare room.
std::uint64_t stage_path(RestoreTarget& t, const std::string& path, std::uint64_t scratch_at) {
    if (path.size() + 1 > 4096)
        throw CheckpointError("restore: path too long to stage: " + path);
    ::iovec local{const_cast<char*>(path.c_str()), path.size() + 1};
    ::iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(scratch_at)),
                   path.size() + 1};
    if (::process_vm_writev(t.pid(), &local, 1, &remote, 1, 0) !=
        static_cast<ssize_t>(path.size() + 1))
        throw CheckpointError("restore: could not stage path into the target: " + path);
    return scratch_at;
}

}  // namespace

int prot_of(std::uint32_t flags) {
    int p = 0;
    if (flags & image::MappingFlag::Read) p |= PROT_READ;
    if (flags & image::MappingFlag::Write) p |= PROT_WRITE;
    if (flags & image::MappingFlag::Exec) p |= PROT_EXEC;
    return p;
}

bool collides_with_tls(const RestoreTarget& target,
                       const std::vector<image::MappingRecord>& mappings) {
    for (const auto& m : mappings) {
        if (m.flags & image::MappingFlag::KernelSpecial) continue;
        if (overlaps(m.start_vaddr, m.end_vaddr, target.tls_start(), target.tls_end())) return true;
    }
    return false;
}

std::unique_ptr<RestoreTarget> make_target_for(const std::vector<image::MappingRecord>& mappings,
                                               std::uint64_t reserve_addr,
                                               std::size_t reserve_bytes, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        auto t = std::make_unique<RestoreTarget>(reserve_addr, reserve_bytes);
        if (!collides_with_tls(*t, mappings)) return t;
        // ASLR puts the next stub's TLS somewhere else; discard this one.
    }
    throw CheckpointError(
        "restore: could not place a stub whose TLS clears the image after " +
        std::to_string(max_attempts) + " attempts");
}

void disable_huge_pages(RestoreTarget& target, std::uint64_t start, std::uint64_t end) {
    // Transparent huge pages are fatal to a page-granular restore. With THP set
    // to [always], writing anywhere in a large anonymous mapping lets the kernel
    // back a 2 MiB span with one huge page, making up to 512 neighbouring 4 KiB
    // pages resident at once. Pages the restore believes are missing then
    // already exist: UFFDIO_COPY returns EEXIST, and a target that carries on
    // runs partly on stale memory. Observed as heap corruption
    // (`free(): invalid size`) before the cause was found.
    //
    // Applied ONLY to mappings that will actually contain a deferred region.
    // Blanket-disabling it cost eager restore more than twice its runtime, for
    // a hazard eager cannot encounter: it defers nothing, so no page it writes
    // is ever expected to be missing later.
    const long rc = target.syscall(__NR_madvise, start, end - start,
                                   static_cast<std::uint64_t>(MADV_NOHUGEPAGE));
    if (rc != 0 && rc != -EINVAL)  // EINVAL: kernel built without THP
        throw CheckpointError("restore: madvise(MADV_NOHUGEPAGE) on " + hex(start) + ": " +
                              std::strerror(static_cast<int>(-rc)));
}

void enable_huge_pages(RestoreTarget& target, std::uint64_t start, std::uint64_t end) {
    // Eager writes into fresh target memory, so it pays for every page the
    // kernel allocates. With THP set to [madvise] those are 4 KiB pages unless
    // the range opts in, which halves eager's speed (64 MiB in ~49 ms against
    // ~22 ms with huge pages). Opting in makes eager independent
    // of the system-wide mode. It is a hint: a kernel that cannot supply a
    // huge page falls back to 4 KiB, so failure costs speed, never correctness.
    const long rc = target.syscall(__NR_madvise, start, end - start,
                                   static_cast<std::uint64_t>(MADV_HUGEPAGE));
    if (rc != 0 && rc != -EINVAL)  // EINVAL: kernel built without THP
        throw CheckpointError("restore: madvise(MADV_HUGEPAGE) on " + hex(start) + ": " +
                              std::strerror(static_cast<int>(-rc)));
}

LayoutResult rebuild_address_space(RestoreTarget& target, const image::ImageReader& image) {
    const auto mappings = image.mappings();
    if (collides_with_tls(target, mappings))
        throw CheckpointError("restore: image overlaps the stub's TLS block at " +
                              hex(target.tls_start()) + "; use make_target_for()");

    LayoutResult res;

    // --- evict the stub, sparing only what compel needs to keep working ------
    const std::uint64_t reserve_end = target.reserve_addr() + target.reserve_bytes();
    for (const auto& m : checkpoint::read_maps(target.pid())) {
        if (overlaps(m.start, m.end, target.reserve_addr(), reserve_end)) continue;
        if (overlaps(m.start, m.end, target.tls_start(), target.tls_end())) continue;
        if (m.tag == "[vsyscall]") continue;  // cannot be unmapped
        target.syscall(__NR_munmap, m.start, m.end - m.start);
        ++res.evicted;
    }

    // --- rebuild the image's layout -----------------------------------------
    // A spare page inside the reserve carries file paths over to the target.
    const std::uint64_t path_scratch = target.reserve_addr() + target.reserve_bytes() - 4096;

    for (const auto& m : mappings) {
        if (m.flags & image::MappingFlag::KernelSpecial) {
            ++res.skipped_kernel;  // the kernel re-creates these itself
            continue;
        }

        const std::uint64_t len = m.end_vaddr - m.start_vaddr;
        const bool file_backed = (m.flags & image::MappingFlag::FileBacked) && !m.path.empty();

        // Everything is mapped writable so pages can be populated; Task 5 lowers
        // it to prot_of(m.flags) once the contents are in.
        const int prot = PROT_READ | PROT_WRITE;
        int flags = MAP_FIXED_NOREPLACE;
        flags |= (m.flags & image::MappingFlag::Shared) ? MAP_SHARED : MAP_PRIVATE;

        long fd = -1;
        if (file_backed) {
            // Identity check first: a changed binary must fail loudly, not
            // silently restore a process onto different code (§28).
            struct stat st {};
            if (::stat(m.path.c_str(), &st) != 0)
                throw CheckpointError("restore: backing file is gone: " + m.path);
            if (static_cast<std::uint64_t>(st.st_dev) != m.dev ||
                static_cast<std::uint64_t>(st.st_ino) != m.ino)
                throw CheckpointError("restore: backing file changed since checkpoint: " + m.path);

            stage_path(target, m.path, path_scratch);
            fd = target.syscall(__NR_openat, static_cast<std::uint64_t>(AT_FDCWD), path_scratch,
                                O_RDONLY, 0);
            if (fd < 0)
                throw CheckpointError("restore: target could not open " + m.path + ": " +
                                      std::strerror(static_cast<int>(-fd)));
        } else {
            flags |= MAP_ANONYMOUS;
        }

        const long got = target.syscall(__NR_mmap, m.start_vaddr, len,
                                        static_cast<std::uint64_t>(prot),
                                        static_cast<std::uint64_t>(flags),
                                        static_cast<std::uint64_t>(fd), m.file_offset);
        if (fd >= 0) target.syscall(__NR_close, static_cast<std::uint64_t>(fd));

        if (static_cast<std::uint64_t>(got) != m.start_vaddr)
            throw CheckpointError("restore: could not map " + hex(m.start_vaddr) + "-" +
                                  hex(m.end_vaddr) + (m.path.empty() ? "" : " (" + m.path + ")") +
                                  ": mmap returned " + hex(static_cast<std::uint64_t>(got)));

        ++res.created;
        res.bytes_mapped += len;
    }

    return res;
}

}  // namespace adaptive_restore::restore
