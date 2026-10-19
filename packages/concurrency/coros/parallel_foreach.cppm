
module;

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

export module coro_parallel_foreach;

export import coro_task;
import cpu_scheduler;

namespace nova::coro {
    constexpr auto cache_line_size = 64;

    export struct parallel_foreach_options {
        std::size_t grain_size = 0;
        std::size_t min_parallel_size = 2;
    };

    template<class Range, class Fnc>
    concept parallel_foreach_invocable =
        std::ranges::viewable_range<Range> &&
        std::ranges::random_access_range<std::views::all_t<Range>> &&
        std::ranges::sized_range<std::views::all_t<Range>> &&
        requires(Fnc& fnc, std::ranges::range_reference_t<std::views::all_t<Range>> value) {
            { std::invoke(fnc, value) } -> std::same_as<void>;
        };

    template<class View, class Fnc>
    struct parallel_foreach_state {
        parallel_foreach_state(View input, Fnc function, const std::size_t chunk_size)
            :
            range(std::move(input)),
            fn(std::move(function)),
            count(static_cast<std::size_t>(std::ranges::size(range))),
            grain_size(chunk_size)
        {}

        View range;
        Fnc fn;

        const std::size_t count;
        const std::size_t grain_size;

        alignas(cache_line_size) std::atomic_size_t next{0};
        alignas(cache_line_size) std::atomic_bool cancelled{false};
    };

    template<class State>
    void parallel_foreach_worker(State& state) {
        using diff_type = std::ranges::range_difference_t<decltype(state.range)>;
        const auto first = std::ranges::begin(state.range);

        try {
            for(;;) {
                if(state.cancelled.load(std::memory_order_relaxed)) {
                    return;
                }

                const auto begin = state.next.fetch_add(state.grain_size, std::memory_order_relaxed);
                if(begin >= state.count) {
                    return;
                }

                const auto end = begin + std::min(state.grain_size, state.count - begin);

                for(auto i = begin; i != end; ++i) {
                    std::invoke(state.fn, first[static_cast<diff_type>(i)]);
                }
            }
        }
        catch(...) {
            state.cancelled.store(true, std::memory_order_relaxed);
            throw;
        }
    }

    template<class Scheduler, class View, class Fnc>
    [[nodiscard]] task<void> parallel_foreach_impl(
        Scheduler* const requested,
        View range,
        Fnc fn,
        const parallel_foreach_options options
    ) {
        auto* const scheduler = requested != nullptr
            ? requested
            : Scheduler::current();

        if(scheduler == nullptr) [[unlikely]] {
            throw std::logic_error("parallel_foreach requires an active CPU scheduler worker");
        }

        co_await scheduler->schedule();

        const auto size = static_cast<std::size_t>(std::ranges::size(range));

        if(size == 0) {
            co_return;
        }

        const auto concurrency = static_cast<std::size_t>(scheduler->worker_count());
        const auto desired_chunks = std::max<std::size_t>(1, concurrency * 32);

        const auto grain = options.grain_size != 0
            ? options.grain_size
            : std::max<std::size_t>(1, size / desired_chunks);

        const auto chunks = size / grain + static_cast<std::size_t>(size % grain != 0);
        const auto workers = std::min(concurrency, chunks);

        if(size < options.min_parallel_size || workers <= 1) {
            for(auto&& value : range) {
                std::invoke(fn, value);
            }
            co_return;
        }

        parallel_foreach_state state{ std::move(range), std::move(fn), grain };

        using future_type = typename Scheduler::template future<void>;

        std::vector<future_type> futures;
        futures.reserve(workers - 1);

        std::exception_ptr error;

        auto* const shared = std::addressof(state);

        try {
            for(std::size_t i = 1; i < workers; ++i) {
                futures.emplace_back(
                    scheduler->submit([shared] {
                        parallel_foreach_worker(*shared);
                    })
                );
            }
        }
        catch(...) {
            state.cancelled.store(true, std::memory_order_relaxed);
            error = std::current_exception();
        }

        if(error == nullptr) {
            try {
                parallel_foreach_worker(state);
            }
            catch(...) {
                error = std::current_exception();
            }
        }

        for(auto& future : futures) {
            try {
                co_await std::move(future);
            }
            catch(...) {
                if(error == nullptr) {
                    error = std::current_exception();
                }
            }
        }

        if(error != nullptr) {
            std::rethrow_exception(error);
        }
    }

    export template<class Scheduler, class Range, class Fnc>
    requires parallel_foreach_invocable<Range, std::decay_t<Fnc>>
    [[nodiscard]] task<void> parallel_foreach(
        Scheduler& scheduler,
        Range&& range,
        Fnc&& fn,
        const parallel_foreach_options options = {}
    ) {
        return parallel_foreach_impl(
            std::addressof(scheduler),
            std::views::all(std::forward<Range>(range)),
            std::forward<Fnc>(fn),
            options
        );
    }

    export template<class Range, class Fnc>
    requires parallel_foreach_invocable<Range, std::decay_t<Fnc>>
    [[nodiscard]] task<void> parallel_foreach(
        Range&& range,
        Fnc&& fn,
        const parallel_foreach_options options = {}
    ) {
        return parallel_foreach_impl(
            static_cast<cpu_scheduler<>*>(nullptr),
            std::views::all(std::forward<Range>(range)),
            std::forward<Fnc>(fn),
            options
        );
    }
}

