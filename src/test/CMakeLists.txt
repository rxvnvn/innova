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

# ---------------------------------------------------------------------------
# Standalone test modules with their own main() and their own global fixture.
# They must not be linked into test_innova (duplicate mains and globals).
# ---------------------------------------------------------------------------
add_executable(proto_allowlist_tests
    "${CMAKE_SOURCE_DIR}/src/test/allowlist_guard_fixture.cpp"
    "${CMAKE_SOURCE_DIR}/src/test/p2p_sync_tests.cpp")
target_link_libraries(proto_allowlist_tests PRIVATE innova_core Boost::unit_test_framework)
innova_apply_common_options(proto_allowlist_tests)
innova_apply_platform_definitions(proto_allowlist_tests)

add_executable(nullblock_guard_tests
    "${CMAKE_SOURCE_DIR}/src/test/nullblock_guard_fixture.cpp"
    "${CMAKE_SOURCE_DIR}/src/test/p2p_sync_tests.cpp"
    "${CMAKE_SOURCE_DIR}/src/test/pow_chainstale_tests.cpp"
    "${CMAKE_SOURCE_DIR}/src/test/pow_cn_deadlock_tests.cpp")
target_link_libraries(nullblock_guard_tests PRIVATE innova_core Boost::unit_test_framework)
innova_apply_common_options(nullblock_guard_tests)
innova_apply_platform_definitions(nullblock_guard_tests)

# ---------------------------------------------------------------------------
# Standalone regression runners (same invocations as the makefile check-*
# targets). The full suites of these binaries contain the known pre-existing
# p2p_sync inflight_limit_preserves_askfor_order UB hang, so each regression
# case is registered individually, exactly like `make check-proto-allowlist`.
# ---------------------------------------------------------------------------
set(_allowlist_cases
    p2p_sync_tests/peer_protocol_allowlist_table
    p2p_sync_tests/inbound_version_blocked_before_verack_and_requests
    p2p_sync_tests/inbound_version_allowlist_accepts_43950_and_50505
    p2p_sync_tests/outbound_verack_gate_blocks_peer_50000
    p2p_sync_tests/outbound_verack_gate_allows_50505
    p2p_sync_tests/two_own_nodes_handshake_50505_both_ways)
foreach(_c IN LISTS _allowlist_cases)
    string(REPLACE "/" "_" _n "${_c}")
    add_test(NAME "p2p_allowlist_${_n}" COMMAND proto_allowlist_tests --run_test=${_c} --log_level=error)
endforeach()

set(_nullblock_cases
    p2p_sync_tests/nullblock_answer_does_not_penalize_peer
    p2p_sync_tests/nonnull_malformed_block_still_penalized
    p2p_sync_tests/serve_never_pushes_null_block_shape
    pow_cn_deadlock)
foreach(_c IN LISTS _nullblock_cases)
    string(REPLACE "/" "_" _n "${_c}")
    add_test(NAME "p2p_nullblock_${_n}" COMMAND nullblock_guard_tests --run_test=${_c} --log_level=error)
endforeach()
