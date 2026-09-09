#pragma once
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace adaptive_restore::checkpoint {

// True when every byte is zero. Empty spans are vacuously zero. Used by the
// capture pass to elide all-zero pages (PageEncoding::Zero) from the store.
bool page_is_zero(std::span<const std::byte> page);

// Reads another process's memory with process_vm_readv(2). The target must be
// stopped for the snapshot to be coherent; ProcMem does not stop it.
class ProcMem {
public:
    explicit ProcMem(pid_t pid) : pid_(pid) {}

    // Resize `out` to `len` and fill it with [start_vaddr, start_vaddr + len)
    // from the target, issuing one syscall per chunk. Phase 1 is strict: a
    // failed or short read throws CheckpointError naming the vaddr, since every
    // eligible page must be fully readable.
    void read_range(std::uint64_t start_vaddr, std::size_t len, std::vector<std::byte>& out);

    pid_t pid() const { return pid_; }

private:
    pid_t pid_;
};

}  // namespace adaptive_restore::checkpoint
