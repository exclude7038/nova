
find_package(Catch2 3 REQUIRED)

add_executable(nova_concurrency_tests
    backoff_test.cpp
    thread_parker_test.cpp
    seqlock_test.cpp
    mutex_test.cpp
    cpu_scheduler_test.cpp
    coro_test.cpp
    parallel_foreach_test.cpp
)

target_compile_features(nova_concurrency_tests PRIVATE cxx_std_23)
nova_enable_unity_build(nova_concurrency_tests)

target_link_libraries(nova_concurrency_tests
    PRIVATE
      nova_concurrency
      Catch2::Catch2WithMain
      nova_project_options
)

include(CTest)
include(Catch)
catch_discover_tests(nova_concurrency_tests)
