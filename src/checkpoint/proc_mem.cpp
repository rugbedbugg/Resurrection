#include "adaptive_restore/checkpoint/proc_mem.hpp"

#include <sys/uio.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::checkpoint {

namespace {

// One syscall per megabyte: large enough that the per-call cost disappears
// against the copy, small enough to keep the transient buffer bounded.
constexpr std::size_t kChunkBytes = 1024 * 1024;

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

}  // namespace

bool page_is_zero(std::span<const std::byte> page) {
    for (const std::byte b : page)
        if (b != std::byte{0}) return false;
    return true;
}

void ProcMem::read_range(std::uint64_t start_vaddr, std::size_t len, std::vector<std::byte>& out) {
    out.assign(len, std::byte{0});

    std::size_t done = 0;
    while (done < len) {
        const std::size_t chunk = std::min(kChunkBytes, len - done);
        const std::uint64_t vaddr = start_vaddr + done;

        ::iovec local{out.data() + done, chunk};
        ::iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(vaddr)), chunk};

        const ssize_t n = ::process_vm_readv(pid_, &local, 1, &remote, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw CheckpointError("proc_mem: process_vm_readv(pid " + std::to_string(pid_) +
                                  ", vaddr " + hex(vaddr) + ", " + std::to_string(chunk) +
                                  " bytes): " + std::strerror(errno));
        }
        if (static_cast<std::size_t>(n) != chunk)
            throw CheckpointError("proc_mem: short read at vaddr " + hex(vaddr) + ": got " +
                                  std::to_string(n) + " of " + std::to_string(chunk) + " bytes");
        done += chunk;
    }
}

}  // namespace adaptive_restore::checkpoint
