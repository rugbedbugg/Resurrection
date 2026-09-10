# Test suite. Unit tests are plain executables registered with CTest -- no
# framework dependency yet; add one (Catch2/doctest) when Phase 1 lands real
# encode/decode logic to test.

# Every test: links the core lib, keeps assert() live under any build type,
# registers with CTest. Extra deps/defs go on the returned test_<name> target.
function(ar_add_test name)
    add_executable(test_${name} ${ARGN})
    target_link_libraries(test_${name} PRIVATE adaptive_restore)
    target_compile_options(test_${name} PRIVATE -UNDEBUG)
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

ar_add_test(smoke unit/test_smoke.cpp)
ar_add_test(byte_cursor unit/test_byte_cursor.cpp)
ar_add_test(crc32c unit/test_crc32c.cpp)
ar_add_test(sha256 unit/test_sha256.cpp)
ar_add_test(image_roundtrip unit/test_image_roundtrip.cpp)
ar_add_test(image_corruption unit/test_image_corruption.cpp)
ar_add_test(maps_classify unit/test_maps_classify.cpp)
ar_add_test(proc_mem unit/test_proc_mem.cpp)
ar_add_test(fds unit/test_fds.cpp)
ar_add_test(regions unit/test_regions.cpp)
ar_add_test(registers unit/test_registers.cpp)
ar_add_test(capture unit/test_capture.cpp)
ar_add_test(calibration unit/test_calibration.cpp)

if(SQLite3Opt_FOUND)
    ar_add_test(profile_store unit/test_profile_store.cpp)
    ar_add_test(region_profiler unit/test_region_profiler.cpp)
endif()

if(Compel_FOUND)
    ar_add_test(restore_target unit/test_restore_target.cpp)
    ar_add_test(address_space unit/test_address_space.cpp)
    ar_add_test(page_writer unit/test_page_writer.cpp)
    ar_add_test(resume unit/test_resume.cpp)
    ar_add_test(restore_failures unit/test_restore_failures.cpp)
    ar_add_test(uffd unit/test_uffd.cpp)
    ar_add_test(lazy_provider unit/test_lazy_provider.cpp)

    # Phase 3 parity: lazy must match eager, byte for byte, on the real workload.
    ar_add_test(restore_mini_lazy integration/test_restore_mini_lazy.cpp)
    target_compile_definitions(test_restore_mini_lazy PRIVATE
        ARCTL_PATH="$<TARGET_FILE:arctl>"
        MINI_PATH="$<TARGET_FILE:workload_mini>")
    add_dependencies(test_restore_mini_lazy arctl workload_mini)

    # Phase 3 exit gate: lazy must load only what a sparse workload accesses.
    ar_add_test(restore_sparse integration/test_restore_sparse.cpp)
    target_compile_definitions(test_restore_sparse PRIVATE
        ARCTL_PATH="$<TARGET_FILE:arctl>"
        SPARSE_PATH="$<TARGET_FILE:workload_sparse_random>")
    add_dependencies(test_restore_sparse arctl workload_sparse_random)

    # Phase 2 exit gate: real arctl, real workload, checkpoint then restore.
    ar_add_test(restore_mini integration/test_restore_mini.cpp)
    target_compile_definitions(test_restore_mini PRIVATE
        ARCTL_PATH="$<TARGET_FILE:arctl>"
        MINI_PATH="$<TARGET_FILE:workload_mini>")
    add_dependencies(test_restore_mini arctl workload_mini)
endif()

# Phase 1 exit gate: drives the real arctl binary against the real workload.
ar_add_test(capture_mini integration/test_capture_mini.cpp)
target_compile_definitions(test_capture_mini PRIVATE
    ARCTL_PATH="$<TARGET_FILE:arctl>"
    MINI_PATH="$<TARGET_FILE:workload_mini>")
add_dependencies(test_capture_mini arctl workload_mini)

# test_spawn launches this helper, so it needs the built path at compile time.
add_executable(helper_marker_target unit/helper_marker_target.c)
target_include_directories(helper_marker_target PRIVATE ${CMAKE_SOURCE_DIR}/include)

ar_add_test(spawn unit/test_spawn.cpp)
target_compile_definitions(test_spawn PRIVATE HELPER_PATH="$<TARGET_FILE:helper_marker_target>")
add_dependencies(test_spawn helper_marker_target)

# arctl should print usage and exit non-zero with no args.
add_test(NAME arctl_usage COMMAND arctl)
set_tests_properties(arctl_usage PROPERTIES WILL_FAIL TRUE)

# arctl --version prints the version string.
add_test(NAME arctl_version COMMAND arctl --version)
string(REPLACE "." "\\." _ver_re "${PROJECT_VERSION}")
set_tests_properties(arctl_version PROPERTIES PASS_REGULAR_EXPRESSION "^${_ver_re}")
