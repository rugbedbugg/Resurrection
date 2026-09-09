#pragma once
#include <sys/types.h>

#include <vector>

#include "adaptive_restore/image/format.hpp"

namespace adaptive_restore::checkpoint {

// Walk /proc/<pid>/fd and build an FdRecord per descriptor.
//
// Tier 1 supports regular files and /dev/null. Anything else -- sockets, pipes,
// anon_inode objects (epoll/eventfd/timerfd/inotify), ptys and other character
// devices, directories -- throws CheckpointError naming the fd and its link
// target, rather than being silently dropped: a checkpoint that quietly forgot
// a descriptor would restore into a process that cannot work.
//
// The target should be stopped so its fd table cannot change mid-walk.
std::vector<image::FdRecord> collect_fds(pid_t pid);

}  // namespace adaptive_restore::checkpoint
