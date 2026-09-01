# Test suite. Unit tests are plain executables registered with CTest -- no
# framework dependency yet; add one (Catch2/doctest) when Phase 1 lands real
# encode/decode logic to test.

add_executable(test_smoke unit/test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE adaptive_restore)
add_test(NAME smoke COMMAND test_smoke)

# arctl should print usage and exit non-zero with no args.
add_test(NAME arctl_usage COMMAND arctl)
set_tests_properties(arctl_usage PROPERTIES WILL_FAIL TRUE)

# arctl --version prints the version string.
add_test(NAME arctl_version COMMAND arctl --version)
set_tests_properties(arctl_version PROPERTIES PASS_REGULAR_EXPRESSION "^0\\.0\\.1")
