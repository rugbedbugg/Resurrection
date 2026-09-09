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

# arctl should print usage and exit non-zero with no args.
add_test(NAME arctl_usage COMMAND arctl)
set_tests_properties(arctl_usage PROPERTIES WILL_FAIL TRUE)

# arctl --version prints the version string.
add_test(NAME arctl_version COMMAND arctl --version)
set_tests_properties(arctl_version PROPERTIES PASS_REGULAR_EXPRESSION "^0\\.0\\.1")
