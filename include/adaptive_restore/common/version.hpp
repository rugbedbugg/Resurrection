#pragma once

// Central version + on-disk format identifiers.
//
// kProjectVersion must match project(VERSION ...) in CMakeLists.txt; configure
// hard-errors if they drift apart.
// The image format version is independent of the project version: bump it only
// when the checkpoint layout (docs/design-draft-v2.md §7) changes incompatibly.

namespace adaptive_restore {

inline constexpr int kProjectVersionMajor = 0;
inline constexpr int kProjectVersionMinor = 1;
inline constexpr int kProjectVersionPatch = 0;
inline constexpr const char* kProjectVersion = "0.1.0";

// Checkpoint image: 8-byte magic + format version (§7.2).
inline constexpr unsigned char kImageMagic[8] = {'A', 'R', 'I', 'M', 'G', '\0', '\0', '\1'};
inline constexpr int kImageFormatVersion = 1;

// Region-layout + feature-schema versions travel in the profile key (§16.1).
inline constexpr int kRegionLayoutVersion = 1;
inline constexpr int kFeatureSchemaVersion = 1;

// Fixed Tier 1 region size (§5.3, §27 "region granularity" mitigation).
inline constexpr unsigned kDefaultRegionBytes = 256u * 1024u;

}  // namespace adaptive_restore
