// Cold-cache measurement is only worth anything if the cache is actually cold.
// posix_fadvise(DONTNEED) is advisory and silently does nothing in several
// ordinary situations, so the harness verifies rather than trusts it (§26).

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "adaptive_restore/bench/cache.hpp"
#include "adaptive_restore/bench/suite.hpp"

using namespace adaptive_restore;

int main() {
    // Real storage, not /tmp: on tmpfs there is nothing to evict, which is the
    // very case the last block below checks for.
    const auto dir = std::filesystem::path(std::getenv("HOME")) / ".cache" / "ar-test-cache";
    std::filesystem::create_directories(dir);
    const auto file = dir / "blob.bin";

    {
        std::ofstream f(file, std::ios::binary);
        const std::vector<char> chunk(1 << 20, 'x');
        for (int i = 0; i < 8; ++i) f.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    }

    // Read it back so it is certainly cached, then evict it.
    {
        std::ifstream f(file, std::ios::binary);
        std::vector<char> buf(1 << 20);
        while (f.read(buf.data(), static_cast<std::streamsize>(buf.size()))) {}
    }

    std::string why;
    const bool ok = bench::evict_from_page_cache(file, why);
    if (!std::filesystem::exists("/proc/self/maps") || bench::is_tmpfs(dir)) {
        // Not the environment this can be checked in; do not assert a lie.
        std::puts("test_bench_cache: skipped (image dir is tmpfs)");
        std::filesystem::remove_all(dir);
        return 0;
    }
    assert(ok && "a freshly written, unmapped file on real storage must be evictable");
    assert(why.empty());

    // Still-mapped pages cannot be dropped, and saying otherwise would hand the
    // benchmark a warm run labelled cold.
    {
        const int fd = ::open(file.c_str(), O_RDONLY);
        assert(fd >= 0);
        void* p = ::mmap(nullptr, 1 << 20, PROT_READ, MAP_SHARED, fd, 0);
        assert(p != MAP_FAILED);
        volatile char sink = 0;
        for (std::size_t i = 0; i < (1u << 20); i += 4096) sink = static_cast<const char*>(p)[i];
        (void)sink;

        std::string why2;
        assert(!bench::evict_from_page_cache(file, why2) &&
               "pages held resident by a mapping must not be reported as evicted");
        assert(why2.find("still has") != std::string::npos);

        ::munmap(p, 1 << 20);
        ::close(fd);
    }

    // A missing file is an error, never a silent success.
    {
        std::string why3;
        assert(!bench::evict_from_page_cache(dir / "does-not-exist", why3));
        assert(why3.find("cannot open") != std::string::npos);
    }

    std::filesystem::remove_all(dir);
    std::puts("test_bench_cache: ok");
    return 0;
}
