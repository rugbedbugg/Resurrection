#include <sqlite3.h>
#include <unistd.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/profile/profile_store.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::profile;
using paging::RestoreAction;

static ProfileKey key_with(std::byte first, int layout = 1, int schema = 1,
                           std::string controller = "cost-model/1") {
    ProfileKey k;
    k.image_digest[0] = first;
    k.region_layout_version = layout;
    k.feature_schema_version = schema;
    k.controller = std::move(controller);
    return k;
}

int main() {
    const std::filesystem::path db =
        std::filesystem::temp_directory_path() / ("arprof_" + std::to_string(::getpid()) + ".sqlite");
    std::filesystem::remove(db);

    const auto k = key_with(std::byte{0xAA});

    {
        auto store = ProfileStore::open(db);
        assert(store.load(k).empty() && "a fresh profile has no history");

        // First restore: region 0 saw 50% of its pages, region 1 saw 10%.
        store.record(k, {{0, 0, 0.50f, 0.0f, RestoreAction::Lazy, {0b00000101}},
                         {1, 0, 0.10f, 0.25f, RestoreAction::Lazy, {0b00000010}}});
        auto rows = store.load(k);
        assert(rows.size() == 2);
        assert(rows[0].restore_count == 1 && std::fabs(rows[0].fraction_accessed_avg - 0.50f) < 1e-6);

        // Second restore: the average must move, not be replaced.
        store.record(k, {{0, 0, 0.30f, 0.0f, RestoreAction::Eager, {0b00001001}}});
        rows = store.load(k);
        assert(rows[0].restore_count == 2);
        assert(std::fabs(rows[0].fraction_accessed_avg - 0.40f) < 1e-5 &&
               "fraction_accessed must be a running mean over restores");
        assert(rows[0].last_action == RestoreAction::Eager);
        // The working set is replaced, not averaged: it names pages, and the
        // most recent restore is the one worth prefetching (REAP).
        assert(rows[0].page_was_touched(0) && rows[0].page_was_touched(3));
        assert(!rows[0].page_was_touched(2) && "the earlier set must not linger");
        // An untouched region keeps its own history.
        assert(rows[1].restore_count == 1);
        assert(store.restores_recorded(k) == 2);
    }

    // --- a different identity must mean NO history, never wrong history ----
    {
        auto store = ProfileStore::open(db);
        assert(store.load(key_with(std::byte{0xBB})).empty() && "a different image");
        assert(store.load(key_with(std::byte{0xAA}, 2)).empty() && "a different region layout");
        assert(store.load(key_with(std::byte{0xAA}, 1, 2)).empty() && "a different feature schema");
        assert(store.load(key_with(std::byte{0xAA}, 1, 1, "bandit/1")).empty() &&
               "a different controller");
        assert(store.load(key_with(std::byte{0xAA})).size() == 2 && "the original key still resolves");
    }

    // --- export and reset ---------------------------------------------------
    {
        auto store = ProfileStore::open(db);
        const auto csv = db.string() + ".csv";
        store.export_csv(k, csv);
        std::ifstream f(csv);
        std::string line;
        assert(std::getline(f, line) && line.rfind("region_id,", 0) == 0);
        int rows = 0;
        while (std::getline(f, line))
            if (!line.empty()) ++rows;
        assert(rows == 2);
        std::filesystem::remove(csv);

        store.reset(k);
        assert(store.load(k).empty() && "reset must clear this identity's history");
    }

    // --- a database from a different schema is refused, not misread --------
    {
        auto store = ProfileStore::open(db);
        store.record(k, {{0, 0, 0.9f, 0.0f, RestoreAction::Eager, {}}});
    }
    const std::filesystem::path bad = db.string() + ".bad";
    std::filesystem::copy_file(db, bad, std::filesystem::copy_options::overwrite_existing);
    {
        // Forge a future schema version directly, rather than shelling out to
        // the sqlite3 CLI: the test already links the library, and depending on
        // a command-line tool being installed makes the test fragile.
        ::sqlite3* raw = nullptr;
        assert(::sqlite3_open(bad.c_str(), &raw) == SQLITE_OK);
        assert(::sqlite3_exec(raw, "UPDATE meta SET v='99' WHERE k='schema_version';", nullptr,
                              nullptr, nullptr) == SQLITE_OK);
        ::sqlite3_close(raw);
    }
    {
        bool threw = false;
        try {
            auto s = ProfileStore::open(bad);
            (void)s;
        } catch (const CheckpointError& e) {
            threw = std::string(e.what()).find("schema") != std::string::npos;
        }
        assert(threw && "a profile written by a different schema must be refused");
    }

    std::filesystem::remove(bad);
    std::filesystem::remove(db);
    std::filesystem::remove(db.string() + "-wal");
    std::filesystem::remove(db.string() + "-shm");
    std::puts("test_profile_store: ok");
    return 0;
}
