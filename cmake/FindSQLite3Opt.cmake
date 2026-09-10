# Optional SQLite3 for the Phase 4 profile store.
#
# Like compel, this is restore-side only: checkpoint and inspect must keep
# building without it. Absence disables the profile-backed policies
# (prefetch, cost), not the build.

find_path(SQLITE3_INCLUDE_DIR NAMES sqlite3.h)
find_library(SQLITE3_LIBRARY NAMES sqlite3)

if(SQLITE3_INCLUDE_DIR AND EXISTS "${SQLITE3_INCLUDE_DIR}/sqlite3.h")
    file(STRINGS "${SQLITE3_INCLUDE_DIR}/sqlite3.h" _ver
         REGEX "^#define +SQLITE_VERSION +\"[^\"]+\"")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" SQLITE3_VERSION "${_ver}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SQLite3Opt
    REQUIRED_VARS SQLITE3_LIBRARY SQLITE3_INCLUDE_DIR
    VERSION_VAR SQLITE3_VERSION)

if(SQLite3Opt_FOUND AND NOT TARGET SQLite::SQLite3)
    add_library(SQLite::SQLite3 UNKNOWN IMPORTED)
    set_target_properties(SQLite::SQLite3 PROPERTIES
        IMPORTED_LOCATION "${SQLITE3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SQLITE3_INCLUDE_DIR}")
endif()

mark_as_advanced(SQLITE3_INCLUDE_DIR SQLITE3_LIBRARY)
