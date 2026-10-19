module;

#include "libassert/assert.hpp"
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module coro_algorithms;

export import coro_task;
export import coro_parallel_foreach;
import scheduler_work;

namespace nova::coro {
    template<class T>
    using joined_result = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

    class join_state {
    public:
        explicit join_state(const std::size_t count) noexcept
            :
            m_remaining(count)
        {}

        void continuation(const std::coroutine_handle<> next) noexcept {
            m_continuation = next;
        }

        static std::coroutine_handle<> complete(void* const context) noexcept {
            auto& state = *static_cast<join_state*>(context);
            const auto next = state.m_continuation;
            if(state.m_remaining.fetch_sub(1, std::memory_order::acq_rel) == 1) {
                return next;
            }
            return std::noop_coroutine();
        }
    private:
        std::atomic<std::size_t> m_remaining;
        std::coroutine_handle<> m_continuation{std::noop_coroutine()};
    };

    template<class T>
    class child_owner {
    public:
        using handle = typename task<T>::handle;

        explicit child_owner(task<T> child) noexcept
            :
            m_handle(std::move(child).release())
        {
            DEBUG_ASSERT(m_handle != nullptr);
        }
        child_owner(const child_owner&) = delete;
        child_owner& operator=(const child_owner&) = delete;
        child_owner(child_owner&& rhs) noexcept
            :
            m_handle(std::exchange(rhs.m_handle, {}))
        {}
        ~child_owner() noexcept {
            if(m_handle != nullptr) {
                m_handle.destroy();
            }
        }

        void attach(join_state& state) noexcept {
            m_handle.promise().completion(&join_state::complete, std::addressof(state));
        }

        [[nodiscard]] handle get() const noexcept { return m_handle; }

        joined_result<T> result() {
            if constexpr(std::is_void_v<T>) {
                m_handle.promise().result();
                return {};
            }
            else {
                return m_handle.promise().result();
            }
        }
    private:
        handle m_handle;
    };

    template<class Scheduler, class... T>
    class all_awaiter {
    public:
        explicit all_awaiter(Scheduler& scheduler, task<T>... children) noexcept
            :
            m_scheduler(scheduler),
            m_children(child_owner<T>{std::move(children)}...),
            m_join(sizeof...(T))
        {}

        [[nodiscard]] static bool await_ready() noexcept { return sizeof...(T) == 0; }

        [[nodiscard]] std::coroutine_handle<> await_suspend(const std::coroutine_handle<> next) noexcept {
            if constexpr(sizeof...(T) == 0) {
                return next;
            }
            else {
                m_join.continuation(next);
                std::apply([this](auto&... child) { (child.attach(m_join), ...); }, m_children);
                const auto last = std::get<sizeof...(T) - 1>(m_children).get();

                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    (m_scheduler.post(as_work(std::get<I>(m_children).get())), ...);
                }(std::make_index_sequence<sizeof...(T) - 1>{});

                return last;
            }
        }

        std::tuple<joined_result<T>...> await_resume() {
            return std::apply([](auto&... child) {
                return std::tuple<joined_result<T>...>{child.result()...};
            }, m_children);
        }
    private:
        Scheduler& m_scheduler;
        std::tuple<child_owner<T>...> m_children;
        join_state m_join;
    };

    template<class Scheduler, class T>
    class vector_awaiter {
    public:
        explicit vector_awaiter(Scheduler& scheduler, std::vector<task<T>> children)
            :
            m_scheduler(scheduler),
            m_join(children.size())
        {
            m_children.reserve(children.size());
            for(auto& child : children) {
                m_children.emplace_back(std::move(child));
            }
        }

        [[nodiscard]] bool await_ready() const noexcept { return m_children.empty(); }

        [[nodiscard]] std::coroutine_handle<> await_suspend(const std::coroutine_handle<> next) noexcept {
            m_join.continuation(next);

            for(auto& child : m_children) {
                child.attach(m_join);
            }

            const auto last = m_children.back().get();

            for(std::size_t i = 0; i + 1 < m_children.size(); ++i) {
                m_scheduler.post(as_work(m_children[i].get()));
            }

            return last;
        }

        std::vector<joined_result<T>> await_resume() {
            std::vector<joined_result<T>> results;
            results.reserve(m_children.size());

            for(auto& child : m_children) {
                results.emplace_back(child.result());
            }

            return results;
        }
    private:
        Scheduler& m_scheduler;
        std::vector<child_owner<T>> m_children;
        join_state m_join;
    };

    export template<class Scheduler, class... T>
    [[nodiscard]] auto when_all(Scheduler& scheduler, task<T>... children) -> task<std::tuple<joined_result<T>...>> {
        co_await scheduler.schedule();
        co_return co_await all_awaiter<Scheduler, T...>{scheduler, std::move(children)...};
    }

    export template<class Scheduler, class T>
    [[nodiscard]] task<std::vector<joined_result<T>>> when_all(Scheduler& scheduler, std::vector<task<T>> children) {
        co_await scheduler.schedule();
        co_return co_await vector_awaiter<Scheduler, T>{scheduler, std::move(children)};
    }

    export template<class Scheduler, class T>
    [[nodiscard]] auto fork(Scheduler& scheduler, task<T> child) {
        return scheduler.submit(std::move(child));
    }

    template<class Future>
    auto await_future(Future child) -> task<typename Future::value_type> {
        co_return co_await std::move(child);
    }

    export template<class Scheduler, class... Future>
    [[nodiscard]] auto join(Scheduler& scheduler, Future... children) {
        return when_all(scheduler, await_future(std::move(children))...);
    }

    export template<class Scheduler, class T>
    T sync_wait(Scheduler& scheduler, task<T> child) {
        return scheduler.submit(std::move(child)).get();
    }

    export template<class Scheduler, class T>
    void spawn(Scheduler& scheduler, task<T> child) {
        scheduler.submit_detached(std::move(child));
    }
}
