#include "adaptive_restore/tui/render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <ostream>

#include "adaptive_restore/image/format.hpp"
#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::tui {

namespace {

// Action -> (cell glyph, colour). Glyphs carry the meaning on their own so the
// views stay readable piped into a file, where the colour is gone.
struct Look {
    char glyph;
    const char* color;
    const char* name;
};

constexpr const char* kReset = "\033[0m";
constexpr const char* kDim = "\033[2m";

Look look_of(paging::RestoreAction a) {
    switch (a) {
    case paging::RestoreAction::Eager:          return {'E', "\033[31m", "eager"};
    case paging::RestoreAction::StaticPrefetch: return {'P', "\033[33m", "prefetch"};
    case paging::RestoreAction::Lazy:           return {'L', "\033[32m", "lazy"};
    }
    return {'?', "", "?"};
}

std::string paint(const Style& s, const char* color, const std::string& text) {
    if (!s.color || !*color) return text;
    return std::string(color) + text + kReset;
}

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

std::string human_bytes(std::uint64_t b) {
    char out[32];
    if (b >= 1024ull * 1024 * 1024) std::snprintf(out, sizeof out, "%.1f GiB", static_cast<double>(b) / (1024.0 * 1024 * 1024));
    else if (b >= 1024 * 1024)      std::snprintf(out, sizeof out, "%.1f MiB", static_cast<double>(b) / (1024.0 * 1024));
    else if (b >= 1024)             std::snprintf(out, sizeof out, "%.1f KiB", static_cast<double>(b) / 1024.0);
    else                            std::snprintf(out, sizeof out, "%llu B", static_cast<unsigned long long>(b));
    return out;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// A bar drawn in eighth-of-a-cell blocks, so a 3% difference is still visible
// at a width where whole-cell bars would round it away.
std::string bar(double fraction, std::size_t width) {
    static const char* kEighths[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    fraction = std::clamp(fraction, 0.0, 1.0);
    const double cells = fraction * static_cast<double>(width);
    const auto full = static_cast<std::size_t>(cells);
    const auto rem = static_cast<std::size_t>((cells - static_cast<double>(full)) * 8.0);
    std::string out;
    for (std::size_t i = 0; i < full; ++i) out += "█";
    if (full < width) out += kEighths[rem];
    return out;
}

}  // namespace

void render_region_map(std::ostream& os, const image::ImageReader& image,
                       const policy::RestorePlan& plan, const Style& style) {
    const auto regions = image.regions();
    const auto mappings = image.mappings();
    const std::uint32_t page_size = image.header().page_size;

    // The largest, not the first: regions are clipped to mapping bounds, so the
    // first one is usually a one-page sliver and quoting it as the cell size
    // would understate every cell in the map by two orders of magnitude.
    std::uint64_t widest = 0;
    for (const auto& r : regions)
        widest = std::max(widest, static_cast<std::uint64_t>(r.page_index_hi - r.page_index_lo) *
                                      page_size);
    os << "\nregion map -- " << regions.size() << " regions of up to " << human_bytes(widest)
       << ", one cell each, grouped by mapping\n\n";

    // Group regions under the mapping that contains them, so the map reads like
    // /proc/pid/maps rather than like a flat wall of letters.
    std::map<std::uint32_t, std::vector<std::size_t>> by_mapping;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        for (const auto& m : mappings)
            if (regions[i].start_vaddr >= m.start_vaddr && regions[i].end_vaddr <= m.end_vaddr) {
                by_mapping[m.mapping_id].push_back(i);
                break;
            }
    }

    std::uint64_t counts[3] = {0, 0, 0};
    for (const auto& [mapping_id, idxs] : by_mapping) {
        const image::MappingRecord* m = nullptr;
        for (const auto& c : mappings)
            if (c.mapping_id == mapping_id) { m = &c; break; }
        if (!m) continue;

        std::string label = m->path.empty() ? "(anon)" : m->path;
        if (label.size() > 28) label = "..." + label.substr(label.size() - 25);

        os << "  " << hex(m->start_vaddr) << "  " << (m->flags & image::MappingFlag::Read ? 'r' : '-')
           << (m->flags & image::MappingFlag::Write ? 'w' : '-')
           << (m->flags & image::MappingFlag::Exec ? 'x' : '-') << "  " << label << '\n' << "    ";

        std::size_t col = 0;
        for (const std::size_t i : idxs) {
            const auto lk = look_of(plan.actions[i]);
            ++counts[static_cast<int>(plan.actions[i])];
            os << paint(style, lk.color, std::string(1, lk.glyph));
            if (++col % (style.width > 8 ? style.width - 6 : 64) == 0) os << "\n    ";
        }
        os << '\n';
    }

    const std::uint64_t total = counts[0] + counts[1] + counts[2];
    os << "\n  legend  ";
    for (const auto a : {paging::RestoreAction::Eager, paging::RestoreAction::StaticPrefetch,
                         paging::RestoreAction::Lazy}) {
        const auto lk = look_of(a);
        char buf[64];
        std::snprintf(buf, sizeof buf, "%c %-9s %5llu (%4.1f%%)   ", lk.glyph, lk.name,
                      static_cast<unsigned long long>(counts[static_cast<int>(a)]),
                      total ? 100.0 * static_cast<double>(counts[static_cast<int>(a)]) /
                                  static_cast<double>(total)
                            : 0.0);
        os << paint(style, lk.color, buf);
    }
    os << '\n';

    if (plan.deferral_declined)
        os << paint(style, kDim,
                    "\n  the cost model chose to defer nothing: no plan it could build saved "
                    "more\n  than the fixed cost of setting deferral up, so every region is "
                    "eager.\n");
    else if (!plan.decisions.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "\n  predicted transfer: %.2f ms across %zu decided regions\n",
                      plan.predicted_transfer_s * 1000.0, plan.decisions.size());
        os << buf;
    }
}

void render_fault_timeline(std::ostream& os, const std::vector<FaultRow>& faults,
                           const Style& style) {
    os << "\ndemand-fault timeline\n\n";
    if (faults.empty()) {
        os << paint(style, kDim, "  no faults recorded: every page was resident before resume.\n");
        return;
    }

    std::uint64_t span = 0;
    for (const auto& f : faults) span = std::max(span, f.offset_ns);
    const std::size_t cols = style.width > 24 ? style.width - 16 : 48;
    // A zero span means every fault landed in the same nanosecond bucket, which
    // is a division hazard, not a drawing problem.
    const double per_col = span ? static_cast<double>(span) / static_cast<double>(cols) : 1.0;

    std::vector<std::uint64_t> hist(cols, 0);
    std::uint64_t served_ns = 0, bytes = 0;
    for (const auto& f : faults) {
        auto c = static_cast<std::size_t>(static_cast<double>(f.offset_ns) / per_col);
        hist[std::min(c, cols - 1)]++;
        served_ns += f.service_ns;
        bytes += f.bytes;
    }
    const std::uint64_t peak = *std::max_element(hist.begin(), hist.end());

    // Five rows of vertical ticks: enough shape to see burst vs. steady, cheap
    // enough that it costs nothing to always print.
    static const char* kTicks[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    for (int row = 4; row >= 0; --row) {
        os << "  ";
        for (const auto h : hist) {
            const double scaled = peak ? static_cast<double>(h) / static_cast<double>(peak) * 5.0 : 0.0;
            const double in_row = std::clamp(scaled - row, 0.0, 1.0);
            os << kTicks[static_cast<std::size_t>(in_row * 8.0)];
        }
        os << '\n';
    }
    char buf[192];
    std::snprintf(buf, sizeof buf, "  %-*s%.2f ms\n", static_cast<int>(cols), "0",
                  static_cast<double>(span) / 1e6);
    os << buf;
    std::snprintf(buf, sizeof buf,
                  "\n  %llu faults, peak %llu per %.3f ms bucket, %s supplied,\n"
                  "  %.2f ms in the handler (%.1f us mean)\n",
                  static_cast<unsigned long long>(faults.size()),
                  static_cast<unsigned long long>(peak), per_col / 1e6, human_bytes(bytes).c_str(),
                  static_cast<double>(served_ns) / 1e6,
                  static_cast<double>(served_ns) / static_cast<double>(faults.size()) / 1e3);
    os << buf;
}

void render_policy_comparison(std::ostream& os, const std::vector<BenchRow>& rows,
                              const Style& style) {
    os << "\nresume latency by policy -- median over repetitions\n";
    if (rows.empty()) {
        os << paint(style, kDim, "  no measurements.\n");
        return;
    }

    // (workload, cache) -> policy -> samples. Cache state moves the crossover by
    // 6x, so folding warm and cold together would average away the effect the
    // whole controller exists to exploit.
    std::map<std::pair<std::string, std::string>, std::map<std::string, std::vector<double>>> by_case;
    bool any_bad_output = false;
    for (const auto& r : rows) {
        by_case[{r.workload, r.cache}][r.policy].push_back(r.restore_ms);
        if (!r.output_ok) any_bad_output = true;
    }

    const std::size_t bar_width = style.width > 62 ? style.width - 56 : 18;
    for (const auto& [key, per_policy] : by_case) {
        std::map<std::string, double> med, lo, hi;
        double worst = 0.0, best = 1e300;
        std::string best_pol;
        for (const auto& [pol, samples] : per_policy) {
            auto sorted = samples;
            std::sort(sorted.begin(), sorted.end());
            med[pol] = median(samples);
            lo[pol] = sorted.front();
            hi[pol] = sorted.back();
            worst = std::max(worst, med[pol]);
            if (med[pol] < best) { best = med[pol]; best_pol = pol; }
        }
        os << "\n  " << key.first << "  (" << key.second << " cache, "
           << per_policy.begin()->second.size() << " reps)\n";
        for (const auto& [pol, ms] : med) {
            char head[80];
            std::snprintf(head, sizeof head, "    %-9s %6.1f ms [%5.1f..%5.1f] ", pol.c_str(), ms,
                          lo.at(pol), hi.at(pol));
            os << head;
            const std::string b = bar(worst > 0 ? ms / worst : 0.0, bar_width);
            const bool winner = pol == best_pol;
            os << (winner ? paint(style, "\033[32m", b) : b);

            // A median gap between two distributions that overlap is not a
            // result, and printing "+6%" next to it reads as one. Say so
            // instead: this view exists to make a comparison, which obliges it
            // not to manufacture one.
            if (winner) os << paint(style, "\033[32m", "  best");
            else if (lo.at(pol) <= hi.at(best_pol) && lo.at(best_pol) <= hi.at(pol))
                os << paint(style, kDim, "  = within spread");
            else {
                char tail[32];
                std::snprintf(tail, sizeof tail, "  +%.0f%%", best > 0 ? (ms / best - 1.0) * 100.0 : 0.0);
                os << tail;
            }
            os << '\n';
        }
    }
    if (any_bad_output)
        os << paint(style, "\033[31m",
                    "\n  WARNING: at least one run produced the wrong output. These latencies "
                    "are not comparable.\n");
}

}  // namespace adaptive_restore::tui
