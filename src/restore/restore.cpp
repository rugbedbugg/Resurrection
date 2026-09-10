#include "adaptive_restore/restore/restore.hpp"

#include <time.h>

#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::restore {

namespace {

std::uint64_t monotonic_ns() {
    ::timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace

RestoreSummary restore_eager(const image::ImageReader& image, const RestoreOptions& opts) {
    const std::uint64_t started = monotonic_ns();

    RestoreSummary out;
    auto target = make_target_for(image.mappings(), opts.reserve_addr, opts.reserve_bytes);
    out.pid = target->pid();

    out.layout = rebuild_address_space(*target, image);
    out.pages = populate_pages(*target, image, opts.verify);

    // §9.2: everything the target may touch is populated before control moves.
    // Anything above here that throws destroys the stub rather than resuming it.
    out.resume = resume(*target, image);
    out.resume_latency_ns = monotonic_ns() - started;
    return out;
}

}  // namespace adaptive_restore::restore
