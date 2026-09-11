#include "adaptive_restore/bench/cache.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace adaptive_restore::bench {

namespace {

struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

// How many of the file's pages the kernel still has cached.
long resident_pages(int fd, std::size_t bytes, std::string& why) {
    if (bytes == 0) return 0;
    void* p = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        why = std::string("mmap for the residency check failed: ") + std::strerror(errno);
        return -1;
    }
    const std::size_t pages = (bytes + 4095) / 4096;
    std::vector<unsigned char> vec(pages, 0);
    long resident = -1;
    if (::mincore(p, bytes, vec.data()) == 0) {
        resident = 0;
        for (const auto v : vec) resident += (v & 1);
    } else {
        why = std::string("mincore failed: ") + std::strerror(errno);
    }
    ::munmap(p, bytes);
    return resident;
}

}  // namespace

bool evict_from_page_cache(const std::filesystem::path& path, std::string& why) {
    Fd f(::open(path.c_str(), O_RDONLY));
    if (f.fd < 0) {
        why = "cannot open " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    struct stat st {};
    if (::fstat(f.fd, &st) != 0) {
        why = "cannot stat " + path.string() + ": " + std::strerror(errno);
        return false;
    }
    const auto bytes = static_cast<std::size_t>(st.st_size);

    // Dirty pages are not droppable, so flush first or the eviction below is a
    // no-op on exactly the file that was just written.
    ::fsync(f.fd);
    if (::posix_fadvise(f.fd, 0, 0, POSIX_FADV_DONTNEED) != 0) {
        why = "posix_fadvise(DONTNEED) on " + path.string() + " failed";
        return false;
    }

    const long resident = resident_pages(f.fd, bytes, why);
    if (resident < 0) return false;
    if (resident > 0) {
        why = path.string() + " still has " + std::to_string(resident) + " of " +
              std::to_string((bytes + 4095) / 4096) +
              " pages cached after eviction (mapped elsewhere, or no backing store)";
        return false;
    }
    return true;
}

}  // namespace adaptive_restore::bench
