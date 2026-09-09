#include "adaptive_restore/checkpoint/fds.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>

#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::checkpoint {

namespace {

// Link targets the kernel synthesises for objects with no path. Matching the
// prefix catches epoll/eventfd/timerfd/inotify/signalfd under anon_inode:.
constexpr const char* kUnsupportedPrefixes[] = {"anon_inode:", "socket:", "pipe:"};

[[noreturn]] void reject(int fd, const std::string& target, const char* why) {
    throw CheckpointError("fd " + std::to_string(fd) + ": unsupported " + why + " (" + target +
                          ")");
}

std::string read_link(const std::string& path, int fd) {
    std::string buf(256, '\0');
    for (;;) {
        const ssize_t n = ::readlink(path.c_str(), buf.data(), buf.size());
        if (n < 0)
            throw CheckpointError("fd " + std::to_string(fd) + ": readlink(" + path +
                                  "): " + std::strerror(errno));
        if (static_cast<std::size_t>(n) < buf.size()) {
            buf.resize(static_cast<std::size_t>(n));
            return buf;
        }
        buf.resize(buf.size() * 2);  // truncated, retry with room
    }
}

// /proc/<pid>/fdinfo/<n> is "key:\tvalue" lines. `pos` is decimal; `flags` is
// octal with a leading 0 (the kernel prints it as "0%o"), so parsing it as
// decimal would corrupt every flag word.
void read_fdinfo(pid_t pid, int fd, std::uint64_t& pos, std::uint32_t& flags) {
    const std::string path =
        "/proc/" + std::to_string(pid) + "/fdinfo/" + std::to_string(fd);
    std::ifstream f(path);
    if (!f)
        throw CheckpointError("fd " + std::to_string(fd) + ": cannot open " + path);

    bool have_pos = false, have_flags = false;
    std::string line;
    while (std::getline(f, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, colon);
        const char* value = line.c_str() + colon + 1;
        while (*value == ' ' || *value == '\t') ++value;

        if (key == "pos") {
            pos = std::strtoull(value, nullptr, 10);
            have_pos = true;
        } else if (key == "flags") {
            flags = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 8));
            have_flags = true;
        }
    }
    if (!have_pos || !have_flags)
        throw CheckpointError("fd " + std::to_string(fd) + ": " + path +
                              " is missing a pos or flags line");
}

}  // namespace

std::vector<image::FdRecord> collect_fds(pid_t pid) {
    const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        throw CheckpointError("fds: cannot open " + dir + ": " + std::strerror(errno));

    // Our own opendir handle shows up in the listing when we walk ourselves.
    // Only then is the fd number ours to skip; against another process the same
    // number belongs to the target and must be collected.
    const int own_dirfd = ::dirfd(d);
    const bool walking_self = (pid == ::getpid());

    std::vector<image::FdRecord> out;
    try {
        while (const dirent* e = (errno = 0, ::readdir(d))) {
            if (e->d_name[0] == '.') continue;

            char* end = nullptr;
            const long parsed = std::strtol(e->d_name, &end, 10);
            if (end == e->d_name || *end != '\0' || parsed < 0)
                continue;  // not a descriptor entry
            const int fd = static_cast<int>(parsed);
            if (walking_self && fd == own_dirfd) continue;

            const std::string link = dir + "/" + e->d_name;
            const std::string target = read_link(link, fd);

            for (const char* prefix : kUnsupportedPrefixes)
                if (target.starts_with(prefix)) reject(fd, target, "descriptor kind");

            struct stat st {};
            if (::stat(link.c_str(), &st) != 0)
                throw CheckpointError("fd " + std::to_string(fd) + ": stat(" + target +
                                      "): " + std::strerror(errno));

            if (S_ISSOCK(st.st_mode)) reject(fd, target, "socket");
            if (S_ISFIFO(st.st_mode)) reject(fd, target, "fifo");
            if (S_ISCHR(st.st_mode) && target != "/dev/null")
                reject(fd, target, "character device");
            if (!S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode))
                reject(fd, target, "file type");

            image::FdRecord rec;
            rec.fd = static_cast<std::uint32_t>(fd);
            rec.dev = static_cast<std::uint64_t>(st.st_dev);
            rec.ino = static_cast<std::uint64_t>(st.st_ino);
            rec.path = target;
            read_fdinfo(pid, fd, rec.pos, rec.flags);
            out.push_back(std::move(rec));
        }
        if (errno != 0)
            throw CheckpointError("fds: readdir(" + dir + "): " + std::strerror(errno));
    } catch (...) {
        ::closedir(d);
        throw;
    }
    ::closedir(d);

    std::sort(out.begin(), out.end(),
              [](const image::FdRecord& a, const image::FdRecord& b) { return a.fd < b.fd; });
    return out;
}

}  // namespace adaptive_restore::checkpoint
