#pragma once

// Central version + on-disk format identifiers.
//
// kProjectVersion must match project(VERSION ...) in CMakeLists.txt; configure
// hard-errors if they drift apart.
// The image format version is independent of the project version: bump it only
// when the checkpoint layout (docs/design-draft-v2.md §7) changes incompatibly.

namespace adaptive_restore {

inline constexpr int kProjectVersionMajor = 0;
inline constexpr int kProjectVersionMinor = 4;
inline constexpr int kProjectVersionPatch = 2;
inline constexpr const char* kProjectVersion = "0.4.5";

// Checkpoint image: 8-byte magic + format version (§7.2).
inline constexpr unsigned char kImageMagic[8] = {'A', 'R', 'I', 'M', 'G', '\0', '\0', '\1'};
// v2 added PageEncoding::FromFile: pages identical to their backing file are
// referenced rather than stored. v1 images are not readable by this build.
inline constexpr int kImageFormatVersion = 2;

// Region-layout + feature-schema versions travel in the profile key (§16.1).
inline constexpr int kRegionLayoutVersion = 1;
inline constexpr int kFeatureSchemaVersion = 1;

// Fixed Tier 1 region size (§5.3, §27 "region granularity" mitigation).
inline constexpr unsigned kDefaultRegionBytes = 256u * 1024u;

}  // namespace adaptive_restore
