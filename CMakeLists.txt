cmake_minimum_required(VERSION 3.20)

project(adaptive_restore
    VERSION 0.0.1
    DESCRIPTION "Adaptive region-aware process restoration for Linux"
    LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "adaptive-restore targets Linux only (needs ptrace, userfaultfd, process_vm_readv)")
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion)

option(AR_BUILD_TESTS "Build the test suite" ON)
option(AR_BUILD_WORKLOADS "Build the synthetic benchmark workloads" ON)

# --- core library ------------------------------------------------------------
# Sources are added phase by phase (see docs/design-draft-v2.md §20).
add_library(adaptive_restore STATIC
    src/common/crc32c.cpp
    src/common/sha256.cpp
    src/image/image_writer.cpp
    src/image/image_reader.cpp
    src/checkpoint/maps.cpp
    src/checkpoint/proc_mem.cpp
)
target_include_directories(adaptive_restore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(adaptive_restore PUBLIC cxx_std_20)

# --- arctl CLI --------------------------------------------------------------
add_executable(arctl src/cli/main.cpp)
target_link_libraries(arctl PRIVATE adaptive_restore)

if(AR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(AR_BUILD_WORKLOADS)
    add_subdirectory(workloads)
endif()
