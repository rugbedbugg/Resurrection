#pragma once
#include <stdexcept>

namespace adaptive_restore {

// Thrown when capturing a process fails in a way the caller cannot recover from
// (unsupported mapping/fd, ptrace failure, target vanished).
struct CheckpointError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Thrown when an image is malformed: bad magic, truncated section, failed CRC,
// or a cursor read past the end of a buffer.
struct ImageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace adaptive_restore
