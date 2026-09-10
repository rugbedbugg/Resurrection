#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "adaptive_restore/paging/page_provider.hpp"

namespace adaptive_restore::profile {

// What makes two restores "the same checkpoint" for learning purposes (§16.1).
//
// The point of every field is that a mismatch must mean "no history", never
// "wrong history": applying a profile learned from a different image, a
// different region layout, or a different controller would be worse than
// starting cold, because the decisions would look confident and be wrong.
struct ProfileKey {
    std::array<std::byte, 32> image_digest{};  // ImageReader::identity_digest()
    int region_layout_version = 0;
    int feature_schema_version = 0;
    std::string controller = "cost-model/1";

    std::string to_string() const;
    bool operator==(const ProfileKey&) const = default;
};

// Tier 1 per-region record (§16.2), plus the recorded working set.
//
// §16.2's field list carries only `fraction_accessed_avg`, but a fraction
// cannot tell a prefetcher WHICH pages to load. §16 and §20 both state the
// store exists to accumulate working sets across restores, so `touched_bitmap`
// fills in what the trimmed record omits: one bit per page of the region, set
// if that page was demanded on the most recent observed restore.
//
// Most-recent rather than a union over history, matching REAP (§4): a union
// over-prefetches once behaviour drifts, and drift handling is Tier 2.
struct RegionHistory {
    std::uint32_t region_id = 0;
    std::uint64_t restore_count = 0;
    float fraction_accessed_avg = 0.0f;
    float zero_page_fraction = 0.0f;
    paging::RestoreAction last_action = paging::RestoreAction::Lazy;
    std::vector<std::uint8_t> touched_bitmap;  // bit i = page (page_index_lo + i)

    bool page_was_touched(std::uint32_t i) const {
        const std::size_t byte = i / 8;
        return byte < touched_bitmap.size() && (touched_bitmap[byte] & (1u << (i % 8)));
    }
};

// SQLite-backed, per §16.3: atomic updates, schema versioning, resettable,
// exportable. Page data never lives here; this is metadata only.
class ProfileStore {
public:
    static ProfileStore open(const std::filesystem::path& db);
    ~ProfileStore();
    ProfileStore(ProfileStore&&) noexcept;
    ProfileStore& operator=(ProfileStore&&) noexcept;
    ProfileStore(const ProfileStore&) = delete;

    // Empty when this key has no history. Never approximate a near-miss.
    std::vector<RegionHistory> load(const ProfileKey& key) const;

    // Fold one restore's observations into the running averages, in a single
    // transaction so a crash mid-update cannot leave a half-learned profile.
    void record(const ProfileKey& key, const std::vector<RegionHistory>& observed);

    void reset(const ProfileKey& key);
    void reset_all();
    void export_csv(const ProfileKey& key, const std::filesystem::path& out) const;

    std::uint64_t restores_recorded(const ProfileKey& key) const;

private:
    ProfileStore() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// Schema version of the profile database itself. A database written by a
// different schema is refused rather than misread.
inline constexpr int kProfileSchemaVersion = 2;  // v2 added touched_bitmap

}  // namespace adaptive_restore::profile
