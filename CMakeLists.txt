cmake_minimum_required(VERSION 3.20)

project(adaptive_restore
    VERSION 0.3.3
    DESCRIPTION "Adaptive region-aware process restoration for Linux"
    LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# version.hpp carries the version for C++ consumers; keep it honest.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/include/adaptive_restore/common/version.hpp _ver_hpp)
string(REGEX MATCH "kProjectVersion = \"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _m "${_ver_hpp}")
if(NOT CMAKE_MATCH_1 STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "version mismatch: CMakeLists.txt says ${PROJECT_VERSION}, "
        "version.hpp says ${CMAKE_MATCH_1}")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "adaptive-restore targets Linux only (needs ptrace, userfaultfd, process_vm_readv)")
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(AR_BUILD_TESTS "Build the test suite" ON)
option(AR_BUILD_WORKLOADS "Build the synthetic benchmark workloads" ON)
option(AR_FORCE_NO_COMPEL "Pretend compel is absent (exercises the no-restore build)" OFF)
option(AR_FORCE_NO_SQLITE "Pretend SQLite is absent (exercises the no-profile build)" OFF)

# --- optional: compel, for the Phase 2 restorer -----------------------------
# Capture (checkpoint/inspect) must never depend on this. A build without compel
# is a supported configuration, not a degraded one: it simply has no `restore`.
if(NOT AR_FORCE_NO_COMPEL)
    find_package(Compel)
endif()
if(NOT AR_FORCE_NO_SQLITE)
    find_package(SQLite3Opt)
endif()
if(SQLite3Opt_FOUND)
    message(STATUS "sqlite3 ${SQLITE3_VERSION} found: profile store enabled")
    # SQLite-gated: only these actually touch the database.
    list(APPEND AR_PROFILE_SOURCES src/profile/profile_store.cpp
                                   src/profile/region_profiler.cpp
                                   src/policy/metrics.cpp)
else()
    message(STATUS "sqlite3 not found: building without the profile store "
                   "(prefetch and cost policies unavailable)")
endif()

if(Compel_FOUND)
    message(STATUS "compel ${COMPEL_VERSION} found: restore support enabled")
    set(AR_RESTORE_SOURCES src/restore/target.cpp src/restore/address_space.cpp src/restore/page_writer.cpp src/restore/resume.cpp src/restore/restore.cpp src/paging/eager_provider.cpp src/paging/page_locator.cpp src/paging/lazy_provider.cpp src/paging/prefetch_provider.cpp src/restore/uffd.cpp)
else()
    message(STATUS "compel not found: building without restore support "
                   "(checkpoint and inspect are unaffected)")
endif()

# --- core library ------------------------------------------------------------
# Sources are added phase by phase (see docs/design-draft-v2.md §20).
add_library(adaptive_restore STATIC
    src/common/crc32c.cpp
    src/common/sha256.cpp
    src/image/image_writer.cpp
    src/image/image_reader.cpp
    src/checkpoint/maps.cpp
    src/checkpoint/proc_mem.cpp
    src/checkpoint/fds.cpp
    src/checkpoint/spawn.cpp
    src/checkpoint/registers.cpp
    src/checkpoint/regions.cpp
    src/checkpoint/capture.cpp
    src/policy/calibration.cpp
    src/policy/policy_engine.cpp
    ${AR_PROFILE_SOURCES}
    ${AR_RESTORE_SOURCES}
)
target_include_directories(adaptive_restore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(Compel_FOUND)
    target_link_libraries(adaptive_restore PUBLIC Compel::Compel)
    target_compile_definitions(adaptive_restore PUBLIC AR_HAVE_COMPEL)
endif()
if(SQLite3Opt_FOUND)
    target_link_libraries(adaptive_restore PUBLIC SQLite::SQLite3)
    target_compile_definitions(adaptive_restore PUBLIC AR_HAVE_SQLITE)
endif()
target_compile_features(adaptive_restore PUBLIC cxx_std_20)

# --- arctl CLI --------------------------------------------------------------
add_executable(arctl
    src/cli/main.cpp
    src/cli/checkpoint_cmd.cpp
    src/cli/inspect_cmd.cpp
)
target_link_libraries(arctl PRIVATE adaptive_restore)
if(SQLite3Opt_FOUND)
    target_sources(arctl PRIVATE src/cli/profile_cmd.cpp)
endif()
if(Compel_FOUND)
    target_sources(arctl PRIVATE src/cli/restore_cmd.cpp)
    target_link_libraries(arctl PRIVATE Compel::Compel)
    target_compile_definitions(arctl PRIVATE AR_HAVE_COMPEL)
endif()

if(AR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(AR_BUILD_WORKLOADS)
    add_subdirectory(workloads)
endif()
