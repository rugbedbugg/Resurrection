#include "adaptive_restore/policy/calibration.hpp"

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::policy {

namespace {

constexpr double kMiB = 1024.0 * 1024.0;

double now_s() {
    ::timespec t{};
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) / 1e9;
}

// A probe large enough that throughput dominates syscall overhead and timer
// resolution. 2 MB is far too small: see the calibration report.
constexpr std::size_t kProbeBytes = 64u << 20;

}  // namespace

// Measured on this machine; see docs/phase4-calibration-report.md.
//   process_vm_writev  1505 MiB/s      UFFDIO_COPY  549 MiB/s
//   mean fault         26.2 us         UFFDIO_REGISTER  1.72 us/region
// The two transfer paths differ by 2.74x, which is why pricing prefetch with
// read bandwidth made the controller choose it and lose.
Calibration Calibration::defaults_warm() {
    return {1505.0 * kMiB, 549.0 * kMiB, 26.2e-6, 1.72e-6, 7060.0 * kMiB};
}

Calibration Calibration::defaults_cold() {
    return {1505.0 * kMiB, 549.0 * kMiB, 26.2e-6, 1.72e-6, 1141.0 * kMiB};
}

Calibration Calibration::measure(const char* image_dir) {
    Calibration out = defaults_cold();

    std::string path = std::string(image_dir ? image_dir : ".") + "/arcalibXXXXXX";
    std::vector<char> tmpl(path.begin(), path.end());
    tmpl.push_back('\0');
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) return out;  // fall back to the measured defaults

    std::vector<char> buf(1u << 20, 0);
    for (std::size_t w = 0; w < kProbeBytes; w += buf.size())
        if (::write(fd, buf.data(), buf.size()) != static_cast<ssize_t>(buf.size())) break;
    ::fsync(fd);

    // Cold: drop it from the page cache first. On tmpfs this is a no-op and the
    // figure will be memory bandwidth, which is why the caller must pass a
    // directory on real storage.
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::lseek(fd, 0, SEEK_SET);
    const double t0 = now_s();
    ssize_t total = 0, n;
    while ((n = ::read(fd, buf.data(), buf.size())) > 0) total += n;
    const double cold_dt = now_s() - t0;

    // Warm: it is in cache now, so read it again.
    ::lseek(fd, 0, SEEK_SET);
    const double t1 = now_s();
    ssize_t warm_total = 0;
    while ((n = ::read(fd, buf.data(), buf.size())) > 0) warm_total += n;
    const double warm_dt = now_s() - t1;

    ::close(fd);
    ::unlink(tmpl.data());

    if (cold_dt > 0 && total > 0)
        out.sequential_bandwidth_bps = static_cast<double>(total) / cold_dt;
    (void)warm_dt;
    (void)warm_total;
    // The transfer-path figures are not measurable without a live target, so
    // the calibrated defaults stand until a restore measures them in situ.
    return out;
}

double Calibration::cost_eager(std::uint64_t bytes) const {
    if (writev_bandwidth_bps <= 0) return 0;
    return static_cast<double>(bytes) / writev_bandwidth_bps;
}

double Calibration::cost_lazy(std::uint64_t expected_faults) const {
    return register_cost_s + static_cast<double>(expected_faults) * mean_fault_cost_s;
}

double Calibration::cost_prefetch(std::uint64_t prefetch_bytes, std::uint64_t miss_faults,
                                  std::uint64_t wasted_bytes, double lambda_waste) const {
    if (uffd_copy_bandwidth_bps <= 0) return 0;
    return register_cost_s + static_cast<double>(prefetch_bytes) / uffd_copy_bandwidth_bps +
           static_cast<double>(miss_faults) * mean_fault_cost_s +
           lambda_waste * static_cast<double>(wasted_bytes);
}

}  // namespace adaptive_restore::policy
