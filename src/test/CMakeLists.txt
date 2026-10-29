include("${CMAKE_CURRENT_SOURCE_DIR}/test-sources.cmake")

# test_innova: one Boost.Test module with its own main(). innova_core does not
# contain a main(), so there is exactly one entry point here.
add_executable(test_innova ${INNOVA_TEST_SOURCES})
target_link_libraries(test_innova PRIVATE innova_core Boost::unit_test_framework)
innova_apply_common_options(test_innova)
innova_apply_platform_definitions(test_innova)

# CTest registration. Each suite runs in its own process, exactly as the
# Makefile-based workflow does; the exit code is reported as-is (no
# PASS_REGULAR_EXPRESSION masking).
set(INNOVA_CTEST_SUITES
    blockindex_authoritative_regtest_bootstrap_tests
    blockindex_derived_state_tests
    blockindex_startup_seam_tests
    candidate_frontier_tests
    cpu_mining_controller_tests
    qt_tx_semantics_tests
    blockindex_hashindex_tests
    cpu_mining_nbits_tests
    invalidate_reconsider_tests)

foreach(_suite IN LISTS INNOVA_CTEST_SUITES)
  add_test(NAME ${_suite}
           COMMAND test_innova --run_test=${_suite} --log_level=error)
  set_tests_properties(${_suite} PROPERTIES TIMEOUT 900)
endforeach()
