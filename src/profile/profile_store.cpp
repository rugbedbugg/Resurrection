#include "adaptive_restore/profile/profile_store.hpp"

#include <sqlite3.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <utility>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/common/version.hpp"

namespace adaptive_restore::profile {

namespace {

std::string hex(const std::array<std::byte, 32>& d) {
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (std::byte b : d) {
        const auto v = std::to_integer<unsigned>(b);
        s += k[v >> 4];
        s += k[v & 15];
    }
    return s;
}

}  // namespace

std::string ProfileKey::to_string() const {
    return hex(image_digest) + "/r" + std::to_string(region_layout_version) + "/f" +
           std::to_string(feature_schema_version) + "/" + controller;
}

struct ProfileStore::Impl {
    ::sqlite3* db = nullptr;

    void exec(const char* sql) {
        char* err = nullptr;
        if (::sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string msg = err ? err : "unknown error";
            ::sqlite3_free(err);
            throw CheckpointError("profile: " + msg);
        }
    }
};

ProfileStore ProfileStore::open(const std::filesystem::path& path) {
    ProfileStore s;
    s.impl_ = new Impl();
    if (::sqlite3_open(path.c_str(), &s.impl_->db) != SQLITE_OK) {
        const std::string msg = ::sqlite3_errmsg(s.impl_->db);
        ::sqlite3_close(s.impl_->db);
        delete s.impl_;
        s.impl_ = nullptr;
        throw CheckpointError("profile: cannot open " + path.string() + ": " + msg);
    }

    // Durability over speed: a profile is written once per restore, and a
    // half-written one is worse than none.
    s.impl_->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
    s.impl_->exec(
        "CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT);"
        "CREATE TABLE IF NOT EXISTS region_history ("
        "  key TEXT NOT NULL,"
        "  region_id INTEGER NOT NULL,"
        "  restore_count INTEGER NOT NULL,"
        "  fraction_accessed_avg REAL NOT NULL,"
        "  zero_page_fraction REAL NOT NULL,"
        "  last_action INTEGER NOT NULL,"
        "  PRIMARY KEY (key, region_id));");

    // Schema versioning (§16.3): refuse a database this build cannot read
    // rather than misinterpreting its columns.
    ::sqlite3_stmt* st = nullptr;
    ::sqlite3_prepare_v2(s.impl_->db, "SELECT v FROM meta WHERE k='schema_version';", -1, &st,
                         nullptr);
    std::string found;
    if (st && ::sqlite3_step(st) == SQLITE_ROW)
        found = reinterpret_cast<const char*>(::sqlite3_column_text(st, 0));
    ::sqlite3_finalize(st);

    const std::string want = std::to_string(kProfileSchemaVersion);
    if (found.empty()) {
        s.impl_->exec(("INSERT OR REPLACE INTO meta VALUES('schema_version','" + want + "');")
                          .c_str());
    } else if (found != want) {
        const std::string msg = "profile: database schema v" + found + " but this build reads v" +
                                want + "; reset the profile or use a different file";
        ::sqlite3_close(s.impl_->db);
        delete s.impl_;
        s.impl_ = nullptr;
        throw CheckpointError(msg);
    }
    return s;
}

ProfileStore::~ProfileStore() {
    if (impl_) {
        if (impl_->db) ::sqlite3_close(impl_->db);
        delete impl_;
    }
}

ProfileStore::ProfileStore(ProfileStore&& o) noexcept : impl_(std::exchange(o.impl_, nullptr)) {}

ProfileStore& ProfileStore::operator=(ProfileStore&& o) noexcept {
    if (this != &o) {
        this->~ProfileStore();
        impl_ = std::exchange(o.impl_, nullptr);
    }
    return *this;
}

std::vector<RegionHistory> ProfileStore::load(const ProfileKey& key) const {
    std::vector<RegionHistory> out;
    ::sqlite3_stmt* st = nullptr;
    ::sqlite3_prepare_v2(impl_->db,
                         "SELECT region_id, restore_count, fraction_accessed_avg,"
                         " zero_page_fraction, last_action FROM region_history"
                         " WHERE key=? ORDER BY region_id;",
                         -1, &st, nullptr);
    const std::string k = key.to_string();
    ::sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
    while (::sqlite3_step(st) == SQLITE_ROW) {
        RegionHistory h;
        h.region_id = static_cast<std::uint32_t>(::sqlite3_column_int64(st, 0));
        h.restore_count = static_cast<std::uint64_t>(::sqlite3_column_int64(st, 1));
        h.fraction_accessed_avg = static_cast<float>(::sqlite3_column_double(st, 2));
        h.zero_page_fraction = static_cast<float>(::sqlite3_column_double(st, 3));
        h.last_action = static_cast<paging::RestoreAction>(::sqlite3_column_int(st, 4));
        out.push_back(h);
    }
    ::sqlite3_finalize(st);
    return out;
}

void ProfileStore::record(const ProfileKey& key, const std::vector<RegionHistory>& observed) {
    const std::string k = key.to_string();
    impl_->exec("BEGIN IMMEDIATE;");
    try {
        for (const auto& o : observed) {
            // Running mean over the last N restores of this identity (§10.1).
            // Stored as an incremental average so no history of samples is kept.
            ::sqlite3_stmt* st = nullptr;
            ::sqlite3_prepare_v2(impl_->db,
                                 "INSERT INTO region_history"
                                 " (key,region_id,restore_count,fraction_accessed_avg,"
                                 "  zero_page_fraction,last_action)"
                                 " VALUES(?,?,1,?,?,?)"
                                 " ON CONFLICT(key,region_id) DO UPDATE SET"
                                 "  fraction_accessed_avg ="
                                 "    (fraction_accessed_avg * restore_count + excluded."
                                 "     fraction_accessed_avg) / (restore_count + 1),"
                                 "  zero_page_fraction = excluded.zero_page_fraction,"
                                 "  last_action = excluded.last_action,"
                                 "  restore_count = restore_count + 1;",
                                 -1, &st, nullptr);
            ::sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
            ::sqlite3_bind_int64(st, 2, o.region_id);
            ::sqlite3_bind_double(st, 3, o.fraction_accessed_avg);
            ::sqlite3_bind_double(st, 4, o.zero_page_fraction);
            ::sqlite3_bind_int(st, 5, static_cast<int>(o.last_action));
            const int rc = ::sqlite3_step(st);
            ::sqlite3_finalize(st);
            if (rc != SQLITE_DONE)
                throw CheckpointError(std::string("profile: write failed: ") +
                                      ::sqlite3_errmsg(impl_->db));
        }
        impl_->exec("COMMIT;");
    } catch (...) {
        impl_->exec("ROLLBACK;");
        throw;
    }
}

void ProfileStore::reset(const ProfileKey& key) {
    ::sqlite3_stmt* st = nullptr;
    ::sqlite3_prepare_v2(impl_->db, "DELETE FROM region_history WHERE key=?;", -1, &st, nullptr);
    const std::string k = key.to_string();
    ::sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
    ::sqlite3_step(st);
    ::sqlite3_finalize(st);
}

void ProfileStore::reset_all() { impl_->exec("DELETE FROM region_history;"); }

std::uint64_t ProfileStore::restores_recorded(const ProfileKey& key) const {
    const auto rows = load(key);
    std::uint64_t most = 0;
    for (const auto& r : rows) most = std::max(most, r.restore_count);
    return most;
}

void ProfileStore::export_csv(const ProfileKey& key, const std::filesystem::path& out) const {
    std::ofstream f(out);
    if (!f) throw CheckpointError("profile: cannot write " + out.string());
    f << "region_id,restore_count,fraction_accessed_avg,zero_page_fraction,last_action\n";
    for (const auto& r : load(key))
        f << r.region_id << ',' << r.restore_count << ',' << r.fraction_accessed_avg << ','
          << r.zero_page_fraction << ',' << static_cast<int>(r.last_action) << '\n';
}

}  // namespace adaptive_restore::profile
