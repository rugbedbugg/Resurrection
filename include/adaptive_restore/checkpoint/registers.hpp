#pragma once
#include <sys/types.h>

#include "adaptive_restore/image/format.hpp"

namespace adaptive_restore::checkpoint {

// Capture the CPU state of a ptrace-stopped target.
//
// GPRs come from NT_PRSTATUS; the extended state (x87/SSE/AVX/AVX-512/AMX) is
// pulled as one opaque NT_X86_XSTATE blob, falling back to the 512-byte legacy
// FXSAVE area if the kernel has no xstate regset. Throws CheckpointError if the
// target cannot be read.
image::RegisterRecord capture_registers(pid_t pid);

}  // namespace adaptive_restore::checkpoint
