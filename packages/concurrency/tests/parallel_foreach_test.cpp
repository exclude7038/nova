#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <vector>
#include <coroutine>

import cpu_scheduler;
import coro_algorithms;

namespace {
    [[nodiscard]] nova::scheduler_config par_cfg(const std::uint32_t workers) {
        return {
            .worker_count = workers,
            .local_queue_capacity = 64,
            .global_queue_capacity = 128,
            .task_slots_per_worker = 128,
            .global_poll_interval = 7,
            .steal_batch_size = 8
        };
    }

    struct callback_error final : std::runtime_error {
        callback_error() : std::runtime_error("parallel callback failed") {}
    };

    TEST_CASE("Parallel foreach") {
        SECTION("empty and singleton") {
            nova::cpu_scheduler<> scheduler {par_cfg(4)};
            std::atomic_int calls{0};

            auto operation = nova::coro::invoke([&]() -> nova::coro::task<void> {
                co_await nova::coro::parallel_foreach(std::views::iota(0, 0), [&](int) {
                    calls.fetch_add(1, std::memory_order_relaxed);
                });

                co_await nova::coro::parallel_foreach(std::views::iota(0, 1), [&](int) {
                    calls.fetch_add(1, std::memory_order_relaxed);
                });
            });

            nova::coro::sync_wait(scheduler, std::move(operation));
            CHECK(calls.load(std::memory_order::relaxed) == 1);
            scheduler.shutdown();
            CHECK(scheduler.pending() == 0);
        }
        SECTION("exactly once") {
            for(const auto workers : {1u, 2u, 4u, 12u}) {
                nova::cpu_scheduler<> scheduler{par_cfg(workers)};
                constexpr std::size_t count = 20003;
                std::vector<std::atomic_uint32_t> visits(count);
                std::atomic_uint64_t sum{0};

                auto operation = nova::coro::invoke([&]() -> nova::coro::task<void> {
                    co_await nova::coro::parallel_foreach(
                        scheduler,
                        std::views::iota(std::size_t{0}, count),
                        [&](const std::size_t index) {
                            visits[index].fetch_add(1, std::memory_order_relaxed);
                            sum.fetch_add(index, std::memory_order_relaxed);
                        },
                        {.grain_size = 7}
                    );
                });

                nova::coro::sync_wait(scheduler, std::move(operation));
                CHECK(sum.load(std::memory_order_relaxed) == (count * (count - 1)) / 2);
                for(const auto& visit : visits) {
                    CHECK(visit.load(std::memory_order_relaxed) == 1);
                }
                scheduler.shutdown();
                CHECK(scheduler.pending() == 0);
            }
        }
        SECTION("exception handling") {
            nova::cpu_scheduler<> scheduler{par_cfg(8)};
            std::atomic_uint32_t active{0};

            auto operation = nova::coro::invoke([&]() -> nova::coro::task<void> {
                co_await nova::coro::parallel_foreach(
                    scheduler,
                    std::views::iota(0, 1000),
                    [&](const int index) {
                        active.fetch_add(1, std::memory_order_relaxed);
                        struct scope_exit {
                            std::atomic_uint32_t& counter;
                            ~scope_exit() { counter.fetch_sub(1, std::memory_order_relaxed); }
                        } guard{active};

                        if(index == 100) {
                            throw callback_error{};
                        }
                    },
                    {.grain_size = 1}
                );
            });

            CHECK_THROWS_AS(nova::coro::sync_wait(scheduler, std::move(operation)), callback_error);
            CHECK(active.load(std::memory_order_relaxed) == 0);
            scheduler.shutdown();
            CHECK(scheduler.pending() == 0);
        }
        SECTION("move-only callback") {
            nova::cpu_scheduler<> scheduler{par_cfg(4)};
            std::atomic_int result{0};

            auto operation = nova::coro::parallel_foreach(
                std::vector<int>{1, 2, 3, 4, 5},
                [value = std::make_unique<int>(10), &result](const int item) {
                    result.fetch_add(item * *value, std::memory_order_relaxed);
                }
            );

            nova::coro::sync_wait(scheduler, std::move(operation));
            CHECK(result.load(std::memory_order_relaxed) == 150);
        }
        SECTION("cross scheduler") {
            nova::cpu_scheduler<> first{par_cfg(2)};
            nova::cpu_scheduler<> second{par_cfg(4)};
            std::atomic_uint32_t incorrect{0};

            auto operation = nova::coro::invoke([&]() -> nova::coro::task<void> {
                co_await nova::coro::parallel_foreach(second, std::views::iota(0, 200), [&](int) {
                    if(!second.is_worker_thread()) {
                        incorrect.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            });

            nova::coro::sync_wait(first, std::move(operation));
            CHECK(incorrect.load(std::memory_order_relaxed) == 0);
            first.shutdown();
            second.shutdown();
            CHECK(first.pending() == 0);
            CHECK(second.pending() == 0);
        }
        SECTION("parallel execution") {
            nova::cpu_scheduler<> scheduler{par_cfg(4)};

            constexpr std::size_t count = 4096;

            std::vector<std::uint32_t> input(count);
            std::vector<std::uint64_t> output(count, 0);

            for(std::size_t i = 0; i < count; ++i) {
                input[i] = static_cast<std::uint32_t>(i + 1);
            }

            std::atomic_uint32_t active{0};
            std::atomic_bool concurrent{false};
            std::atomic_bool first_callback{false};

            auto operation = nova::coro::parallel_foreach(
                scheduler,
                std::views::iota(std::size_t{0}, count),
                [&](const std::size_t index) {
                    const auto running = active.fetch_add(1, std::memory_order_acq_rel) + 1;

                    if(running > 1) {
                        concurrent.store(true, std::memory_order_release);
                    }
                    if(!first_callback.exchange(true, std::memory_order_relaxed)) {
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};

                        while(!concurrent.load(std::memory_order_acquire) &&
                            std::chrono::steady_clock::now() < deadline) {
                            std::this_thread::yield();
                        }
                    }

                    const auto value = static_cast<std::uint64_t>(input[index]);
                    output[index] = value * value + 3 * value;

                    active.fetch_sub(1, std::memory_order_acq_rel);
                },
                {.grain_size = 1, .min_parallel_size = 2}
            );

            nova::coro::sync_wait(scheduler, std::move(operation));

            CHECK(concurrent.load(std::memory_order_acquire));
            CHECK(active.load(std::memory_order_relaxed) == 0);

            for(std::size_t i = 0; i < count; ++i) {
                const auto value = static_cast<std::uint64_t>(input[i]);
                CHECK(output[i] == value * value + 3 * value);
            }

            scheduler.shutdown();
            CHECK(scheduler.pending() == 0);
        }
    }
}
