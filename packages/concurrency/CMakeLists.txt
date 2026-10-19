add_library(nova_concurrency STATIC)

enable_language(ASM)

set_source_files_properties(
    sync/seqlock.asm
    PROPERTIES
      LANGUAGE ASM
      COMPILE_OPTIONS "-Wno-unused-command-line-argument"
)

target_sources(nova_concurrency
    PRIVATE
      sync/seqlock.asm
    PUBLIC
    FILE_SET cxx_modules TYPE CXX_MODULES FILES
    backoff/backoff.cppm
    parker/thread_parker.cppm
    queue/mpmc_queue.cppm
    utils/hints.cppm
    sync/spinlock.cppm
    sync/seqlock.cppm
    sync/guard.cppm
    sync/mutex.cppm
    deque/concurrentdeque.cppm
    runtime/cpu_scheduler.cppm
    coros/scheduler_work.cppm
    coros/task.cppm
    coros/algorithms.cppm
    coros/parallel_foreach.cppm
)

target_compile_features(nova_concurrency PUBLIC cxx_std_23)
nova_enable_unity_build(nova_concurrency)

find_package(libassert REQUIRED)

target_link_libraries(nova_concurrency
    PUBLIC
    libassert::assert
    PRIVATE
    nova_project_options
)

if(NOVA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
