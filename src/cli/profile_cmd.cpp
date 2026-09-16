#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>

#include "adaptive_restore/cli/commands.hpp"
#include "adaptive_restore/common/version.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/profile/profile_store.hpp"

namespace adaptive_restore::cli {

namespace {

int fail(const std::string& msg) {
    std::cerr << "arctl profile: " << msg << '\n';
    return 1;
}

const char* action_name(paging::RestoreAction a) {
    switch (a) {
    case paging::RestoreAction::Eager: return "eager";
    case paging::RestoreAction::StaticPrefetch: return "prefetch";
    case paging::RestoreAction::Lazy: return "lazy";
    }
    return "?";
}

}  // namespace

int cmd_profile(std::span<const std::string_view> args) {
    if (args.empty())
        return fail("usage: arctl profile show|reset|export --image <img> --profile <db>"
                    " [--output <csv>]");

    const std::string verb(args[0]);
    std::string image_path, profile_path, out_path;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--image" || a == "--profile" || a == "--output") {
            if (i + 1 >= args.size()) return fail(std::string(a) + " needs a value");
            const std::string value(args[++i]);
            if (a == "--image") image_path = value;
            else if (a == "--profile") profile_path = value;
            else out_path = value;
        } else {
            return fail("unknown option '" + std::string(a) + "'");
        }
    }
    if (verb != "show" && verb != "reset" && verb != "export")
        return fail("unknown subcommand '" + verb + "'; expected show, reset or export");
    if (image_path.empty() || profile_path.empty())
        return fail("both --image and --profile are required: a profile is only meaningful "
                    "against the checkpoint identity it was learned from");

    try {
        const auto image = image::ImageReader::open(image_path);
        profile::ProfileKey key;
        key.image_digest = image.identity_digest();
        key.region_layout_version = kRegionLayoutVersion;
        key.feature_schema_version = kFeatureSchemaVersion;

        auto store = profile::ProfileStore::open(profile_path);

        if (verb == "reset") {
            store.reset(key);
            std::cout << "profile reset for this checkpoint identity\n";
            return 0;
        }
        if (verb == "export") {
            if (out_path.empty()) return fail("export needs --output <csv>");
            store.export_csv(key, out_path);
            std::cout << "exported to " << out_path << '\n';
            return 0;
        }

        const auto rows = store.load(key);
        std::cout << "profile " << profile_path << '\n'
                  << "  identity   " << key.to_string() << '\n'
                  << "  restores   " << store.restores_recorded(key) << '\n'
                  << "  regions    " << rows.size() << "\n\n";
        if (rows.empty()) {
            std::cout << "  no history for this identity yet; run a restore with\n"
                         "  --policy lazy and --profile to record one\n";
            return 0;
        }

        std::cout << "  region  restores  accessed  zero  workingset  last\n";
        for (const auto& r : rows) {
            std::uint32_t ws = 0;
            for (std::size_t b = 0; b < r.touched_bitmap.size(); ++b)
                for (int bit = 0; bit < 8; ++bit)
                    if (r.touched_bitmap[b] & (1u << bit)) ++ws;
            char line[160];
            std::snprintf(line, sizeof line, "  %6u  %8llu  %7.1f%%  %3.0f%%  %10u  %s\n",
                          r.region_id, static_cast<unsigned long long>(r.restore_count),
                          static_cast<double>(r.fraction_accessed_avg) * 100.0,
                          static_cast<double>(r.zero_page_fraction) * 100.0, ws,
                          action_name(r.last_action));
            std::cout << line;
        }
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    return 0;
}

}  // namespace adaptive_restore::cli
