# Unit tests
add_executable(unit_tests
    unit/test_protocol.cpp
    unit/test_auth.cpp
    unit/test_invalidation.cpp
    unit/test_retry_failover.cpp
    unit/test_usage_collector.cpp
    unit/test_router.cpp
    unit/test_fault_injection.cpp
)
target_link_libraries(unit_tests PRIVATE gateway_core GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(unit_tests)

# Benchmarks
if(GATEWAY_BUILD_BENCHMARKS)
    add_executable(benchmarks
        benchmark/bench_protocol.cpp
        benchmark/bench_auth.cpp
        benchmark/bench_ring_buffer.cpp
    )
    target_link_libraries(benchmarks PRIVATE gateway_core benchmark::benchmark_main)
endif()
