#pragma once
#include <sys/uio.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::paging::detail {

inline std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

// process_vm_writev/readv can come up short; loop until the range is done.
inline void write_range(pid_t pid, std::uint64_t vaddr, const std::byte* src, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        ::iovec local{const_cast<std::byte*>(src) + done, len - done};
        ::iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(vaddr + done)),
                       len - done};
        const ssize_t n = ::process_vm_writev(pid, &local, 1, &remote, 1, 0);
        if (n < 0)
            throw CheckpointError("restore: process_vm_writev at " + hex(vaddr + done) + ": " +
                                  std::strerror(errno));
        if (n == 0) throw CheckpointError("restore: short write at " + hex(vaddr + done));
        done += static_cast<std::size_t>(n);
    }
}

inline void read_range(pid_t pid, std::uint64_t vaddr, std::byte* dst, std::size_t len) {
    std::size_t done = 0;
    while (done < len) {
        ::iovec local{dst + done, len - done};
        ::iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(vaddr + done)),
                       len - done};
        const ssize_t n = ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (n <= 0)
            throw CheckpointError("restore: read-back at " + hex(vaddr + done) + ": " +
                                  std::strerror(errno));
        done += static_cast<std::size_t>(n);
    }
}

}  // namespace adaptive_restore::paging::detail
