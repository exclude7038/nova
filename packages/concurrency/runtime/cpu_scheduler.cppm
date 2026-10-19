module;
#include "libassert/assert.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <exception>
#include <latch>
#include <memory>
#include <new>
#include <stdexcept>
#include <atomic>
#include <functional>
#include <tuple>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <coroutine>
#include <mutex>

export module cpu_scheduler;
import backoff;
import concurrentdeque;
import mpmc_queue;
import thread_parker;
export import scheduler_work;
export import coro_task;

namespace nova {
    constexpr inline std::size_t cache_line_size = 64;

    using work_type = schedulable_work;
    using work_pointer = work_type*;

    enum class completion_state : std::uint8_t {
        empty,
        queued,
        ready
    };

    template<class Function, class... Args>
    class invocation {
    public:
        template<class Fnc, class...A>
        explicit invocation(Fnc&& fnc, A&&... args) 
        noexcept(
                std::conjunction_v<
                    std::is_nothrow_constructible<Function, Fnc&&>, 
                    std::is_nothrow_constructible<std::tuple<Args...>, A&&...>
                >
        )
            :
            m_function(std::forward<Fnc>(fnc)),
            m_arguments(std::forward<A>(args)...)
        {}

        decltype(auto) operator()() {
            return std::apply(
                [this](auto&... args) -> decltype(auto) {
                    return std::invoke(std::move(m_function), std::move(args)...);
                },
                m_arguments
            );
        }
    private:
        [[no_unique_address]]
        Function m_function;

        [[no_unique_address]]
        std::tuple<Args...> m_arguments;
    };

    class fast_rng {
    public:
        explicit fast_rng(std::uint64_t seed) noexcept
            :
            m_state(mix(seed)) 
        {
            if(m_state == 0) {
                m_state = 0x9e3779b97f4a7c15ULL;
            }
        }

        [[nodiscard]] std::uint32_t bounded(const std::uint32_t bound) noexcept {
            DEBUG_ASSERT(bound != 0);

            const auto product = static_cast<std::uint64_t>(next()) * bound;
            return static_cast<std::uint32_t>(product >> 32);
        }

        [[nodiscard]] std::uint32_t next() noexcept {
            auto value = m_state;
    
            value ^= value >> 12;
            value ^= value << 25;
            value ^= value >> 27;

            m_state = value;

            return static_cast<std::uint32_t>((value * 0x2545f4914f6cdd1dULL) >> 32);
        }
    private:
        [[nodiscard]] static std::uint64_t mix(std::uint64_t value) noexcept {
            value += 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;

            return value ^ (value >> 31);
        }
        std::uint64_t m_state;
    };

    inline void terminate_on_unhandled_exception(std::exception_ptr) noexcept {
        std::terminate();
    }

    template<std::size_t StorageSize, std::size_t StorageAlignment>
    class task_pool;

    template<std::size_t StorageSize, std::size_t StorageAlignment>
    class alignas(std::max(cache_line_size, StorageAlignment)) task_slot final : public schedulable_work {
        using pool_type = task_pool<StorageSize, StorageAlignment>;

        using run_function = void(*)(task_slot&) noexcept;
        using destroy_function = void(*)(task_slot&) noexcept;
        using exception_handler = void(*)(std::exception_ptr) noexcept;
    public:
        task_slot() noexcept : schedulable_work(std::addressof(task_slot::dispatch_slot)) {}
        task_slot(const task_slot&) = delete;
        task_slot& operator=(const task_slot&) = delete;
        task_slot(task_slot&&) = delete;
        task_slot& operator=(task_slot&&) = delete;

        template<class Result, class Model, class... Args>
        void prepare(Args&&... args) {
            static_assert(sizeof(Model) <= StorageSize);
            static_assert(alignof(Model) <= StorageAlignment);
            static_assert(std::is_nothrow_destructible_v<Model>);

            if constexpr (!std::is_void_v<Result>) {
                static_assert(std::is_object_v<Result>);
                static_assert(!std::is_const_v<Result>);
                static_assert(sizeof(Result) <= StorageSize);
                static_assert(alignof(Result) <= StorageAlignment);
                static_assert(std::is_nothrow_destructible_v<Result>);
            }

            std::construct_at(raw_payload<Model>(), std::forward<Args>(args)...);
            m_destroy = &destroy_payload<Model>;
            m_run = &run<Result, Model>;

            m_references.store(2, std::memory_order::relaxed);
            m_state.store(completion_state::queued, std::memory_order::relaxed);
        }

        template<class Model, class... Args>
        void prepare_detached(const exception_handler handler, Args&&... args) {
            static_assert(sizeof(Model) <= StorageSize);
            static_assert(alignof(Model) <= StorageAlignment);
            static_assert(std::is_nothrow_destructible_v<Model>);

            std::construct_at(raw_payload<Model>(), std::forward<Args>(args)...);
            m_destroy = &destroy_payload<Model>;
            m_run = &run_detached<Model>;
            m_exception_handler = handler;

            m_references.store(1, std::memory_order::relaxed);
            m_state.store(completion_state::queued, std::memory_order::relaxed);
        }

        void execute() noexcept {
            DEBUG_ASSERT(m_run != nullptr);
            m_run(*this);
        }

        [[nodiscard]] bool ready() const noexcept {
            return m_state.load(std::memory_order::acquire) == completion_state::ready;
        }

        void wait() const noexcept  {
            auto state = m_state.load(std::memory_order::acquire);

            while(state != completion_state::ready) {
                m_state.wait(state, std::memory_order::acquire);
                state = m_state.load(std::memory_order::acquire);
            }
        }


        template<class Result>
        [[nodiscard]] Result take_result() {
            static_assert(!std::is_void_v<Result>);

            return std::move(*payload<Result>());
        }

        [[nodiscard]] std::exception_ptr exception() const noexcept {
            return m_exception;
        }

        void release_reference() noexcept;

        template<class Result>
        void prepare_external() noexcept {
            static_assert(std::is_void_v<Result> || std::is_object_v<Result>);
            if constexpr(!std::is_void_v<Result>) {
                static_assert(sizeof(Result) <= StorageSize);
                static_assert(alignof(Result) <= StorageAlignment);
                static_assert(std::is_nothrow_destructible_v<Result>);
            }
            m_references.store(2, std::memory_order::relaxed);
            m_state.store(completion_state::queued, std::memory_order::relaxed);
        }

        template<class Result>
        void complete_value(Result&& value) {
            using result_type = std::remove_cvref_t<Result>;
            std::construct_at(raw_payload<result_type>(), std::forward<Result>(value));
            m_destroy = &destroy_payload<result_type>;
            complete();
        }

        void complete_exception(std::exception_ptr exception) noexcept {
            m_exception = std::move(exception);
            complete();
        }

        void complete() noexcept {
            m_state.store(completion_state::ready, std::memory_order::release);
            m_state.notify_one();

            auto* const waiter = m_waiter.exchange(this, std::memory_order::acq_rel);
            if(waiter != nullptr) {
                m_wake(m_wait_context, waiter);
            }
        }

        [[nodiscard]] bool suspend(
            schedulable_work* const waiter, 
            void* const context, 
            void(*wake)(void*, schedulable_work*
        ) noexcept) noexcept {
            m_wait_context = context;
            m_wake = wake;
            schedulable_work* expected = nullptr;

            return m_waiter.compare_exchange_strong(
                expected, 
                waiter, 
                std::memory_order::release, 
                std::memory_order::acquire
            );
        }

        [[nodiscard]] static task_slot* make_overflow() {
            auto* const slot = new task_slot;
            slot->reset();
            return slot;
        }
    private:
        friend pool_type;

        void init(pool_type* const pool, const std::uint32_t index, const std::uint32_t next) noexcept {
            m_pool = pool;
            m_index = index;
            m_next_free.store(next, std::memory_order::relaxed);
        }

        void reset() noexcept {
            DEBUG_ASSERT(m_destroy == nullptr);

            m_exception = {};
            m_waiter.store(nullptr, std::memory_order::relaxed);
            m_exception_handler = nullptr;
            m_run = nullptr;
            m_references.store(1, std::memory_order::relaxed);
            m_state.store(completion_state::empty, std::memory_order::relaxed);
        }

        template<class Payload>
        [[nodiscard]] Payload* raw_payload() noexcept {
            return reinterpret_cast<Payload*>(m_storage.data());
        }

        template<class Payload>
        [[nodiscard]] Payload* payload() noexcept {
            return std::launder(reinterpret_cast<Payload*>(m_storage.data()));
        }

        template<class Payload>
        [[nodiscard]] const Payload* payload() const noexcept {
            return std::launder(reinterpret_cast<const Payload*>(m_storage.data()));
        }

        template<class Payload>
        static void destroy_payload(task_slot& slot) noexcept {
            std::destroy_at(slot.template payload<Payload>());
        }

        template<class Result, class Model>
        static void run(task_slot& slot) noexcept {
            auto* const model = slot.template payload<Model>();
            bool model_alive = true;

            try {
                if constexpr (std::is_void_v<Result>) {
                    (*model)();
                    std::destroy_at(model);
                    model_alive = false;
                    slot.m_destroy = nullptr;
                }
                else {
                    Result result = (*model)();

                    std::destroy_at(model);
                    model_alive = false;

                    std::construct_at(slot.template raw_payload<Result>(), std::move(result));
                    slot.m_destroy = &destroy_payload<Result>;
                }
            }
            catch(...) {
                if(model_alive) {
                    std::destroy_at(model);
                }

                slot.m_destroy = nullptr;
                slot.m_exception = std::current_exception();
            }
            slot.complete();
        }

        template<class Model>
        static void run_detached(task_slot& slot) noexcept {
            auto* const model = slot.template payload<Model>();

            try {
                static_cast<void>((*model)());
            }
            catch(...) {
                const auto exception = std::current_exception();

                std::destroy_at(model);
                slot.m_destroy = nullptr;
                slot.m_exception_handler(exception);
                return;
            }

            std::destroy_at(model);
            slot.m_destroy = nullptr;
        }

        static void dispatch_slot(schedulable_work& work) noexcept {
            auto& slot = static_cast<task_slot&>(work);
            slot.execute();
            slot.release_reference();
        }

        alignas(StorageAlignment)
        std::array<std::byte, StorageSize> m_storage{};

        std::atomic_uint32_t m_references{0};
        std::atomic<completion_state> m_state{completion_state::empty};
        std::atomic_uint32_t m_next_free{0};

        std::atomic<schedulable_work*> m_waiter{nullptr};
        void* m_wait_context{};
        void(*m_wake)(void*, schedulable_work*) noexcept{};

        pool_type* m_pool{nullptr};
        std::uint32_t m_index;

        run_function m_run{};
        destroy_function m_destroy{};
        exception_handler m_exception_handler{};
        std::exception_ptr m_exception;

    };

    template<std::size_t StorageSize, std::size_t StorageAlignment>
    class task_pool {
        using slot_type = task_slot<StorageSize, StorageAlignment>;

        static constexpr std::uint32_t index_bits = 20;
        static constexpr std::uint64_t index_mask = (std::uint64_t{1} << index_bits) - 1;
        static constexpr std::uint32_t empty_index = static_cast<std::uint32_t>(index_mask);
    public:
        explicit task_pool(const std::uint32_t capacity)
            :
            m_capacity(validate_capacity(capacity)),
            m_slots(std::make_unique<slot_type[]>(m_capacity))
        {
            for(uint32_t i = 0; i != capacity; ++i) {
                const auto next = i + 1 == m_capacity
                    ? empty_index
                    : i + 1;
                m_slots[i].init(this, i, next);
            }
            m_head.store(pack(0,0), std::memory_order::relaxed);
        }

        task_pool(const task_pool&) = delete;
        task_pool& operator=(const task_pool&) = delete;
        task_pool& operator=(task_pool&&) = delete;
        task_pool(task_pool&&) = delete;

        ~task_pool() noexcept {
            DEBUG_ASSERT(m_references.load(std::memory_order::relaxed) == 0);
        }

        [[nodiscard]] slot_type* try_acquire() noexcept {
            auto head = m_head.load(std::memory_order::acquire);

            for(;;) {
                const auto index = unpack_index(head);

                if(index == empty_index) {
                    return nullptr;
                }

                auto& slot = m_slots[index];
                const auto next = slot.m_next_free.load(std::memory_order::relaxed);

                if(m_head.compare_exchange_weak(head, pack(next, unpack_tag(head) + 1), std::memory_order::acquire, std::memory_order::acquire)) {
                    m_references.fetch_add(1, std::memory_order::relaxed);
                    slot.reset();

                    return std::addressof(slot);
                }
            }
        }

        void release_reference() noexcept {
            if(m_references.fetch_sub(1, std::memory_order::acq_rel) == 1) {
                delete this;
            }
        }

        struct deleter {
            void operator()(task_pool* const pool) const noexcept {
                pool->release_reference();
            }
        };

        [[nodiscard]] std::uint64_t live() const noexcept {
            return m_references.load(std::memory_order::acquire) - 1;
        }
    private:
        friend slot_type;

        static std::uint32_t validate_capacity(const std::uint32_t capacity) {
            if(capacity == 0 || capacity > empty_index) {
                throw std::invalid_argument("task pool capacity is outside the supported range");
            }
            return capacity;
        }

        void recycle(slot_type& slot) noexcept {
            auto head = m_head.load(std::memory_order::relaxed);

            for(;;) {
                slot.m_next_free.store(unpack_index(head), std::memory_order::relaxed);

                if(m_head.compare_exchange_weak(head, pack(slot.m_index, unpack_tag(head) + 1), std::memory_order::release, std::memory_order::relaxed)) {
                    release_reference();
                    return;
                }
            }
        }

        [[nodiscard]] static constexpr std::uint64_t pack(const std::uint32_t index, const std::uint64_t tag) noexcept {
            return (tag << index_bits) | static_cast<uint64_t>(index);
        }

        [[nodiscard]] static constexpr std::uint32_t unpack_index(const std::uint64_t val) noexcept {
            return static_cast<std::uint32_t>(val & index_mask);
        }

        [[nodiscard]] static constexpr std::uint64_t unpack_tag(const std::uint64_t val) noexcept {
            return val >> index_bits;
        }

        const std::uint32_t m_capacity;
        std::unique_ptr<slot_type[]> m_slots;

        alignas(cache_line_size)
        std::atomic_uint64_t m_head{0};

        alignas(cache_line_size)
        std::atomic_uint64_t m_references{1};
    };

    template<std::size_t StorageSize, std::size_t StorageAlignment>
    void task_slot<StorageSize, StorageAlignment>::release_reference() noexcept {
        const auto prev = m_references.fetch_sub(1, std::memory_order::acq_rel);

        DEBUG_ASSERT(prev != 0);

        if(prev != 1) {
            return;
        }

        if(m_destroy != nullptr) {
            m_destroy(*this);
            m_destroy = nullptr;
        }

        m_exception = {};
        m_run = nullptr;
        m_exception_handler = nullptr;

        m_state.store(completion_state::empty, std::memory_order::relaxed);
        if(m_pool != nullptr) {
            m_pool->recycle(*this);
        }
        else {
            delete this;
        }
    }

    export class scheduler_stopped final : public std::runtime_error {
    public:
        scheduler_stopped() : std::runtime_error{"CPU scheduler has stopped"} {}
    };

    export struct scheduler_config {
        std::uint32_t worker_count = std::max(1u, std::thread::hardware_concurrency());
        std::size_t local_queue_capacity = 4096;
        std::size_t global_queue_capacity = 16384;
        std::uint32_t task_slots_per_worker = 4096;
        std::uint32_t global_poll_interval = 61;
        std::uint32_t steal_batch_size = 32;
        void(*unhandled_exception)(std::exception_ptr) noexcept = &terminate_on_unhandled_exception;
    };

    export template<
        std::size_t TaskStorageSize = 128,
        std::size_t TaskStorageAlignment = 64,
        std::size_t MaximumStealBatch = 32
    >
    class cpu_scheduler {
        static_assert(TaskStorageSize != 0);
        static_assert(std::has_single_bit(TaskStorageAlignment));
        static_assert(MaximumStealBatch != 0);

        using slot_type = task_slot<TaskStorageSize, TaskStorageAlignment>;
        using pool_type = task_pool<TaskStorageSize, TaskStorageAlignment>;
        using task_pointer = slot_type*;

        struct worker_state {
            worker_state(const std::uint32_t worker_index, const scheduler_config& cfg, const std::uint64_t seed)
                :
                index(worker_index),
                local_queue(cfg.local_queue_capacity),
                task_slots(new pool_type(cfg.task_slots_per_worker)),
                rng(seed),
                global_budget(cfg.global_poll_interval)
            {}

            const std::uint32_t index;
            concurrent_deque<work_pointer, Flavor::Lifo> local_queue;
            std::unique_ptr<pool_type, typename pool_type::deleter> task_slots;
            thread_parker parker;
            fast_rng rng;
            std::array<work_pointer, MaximumStealBatch> stolen_tasks{};
            work_pointer lifo_slot{};
            std::uint32_t global_budget;
        };

        static constexpr std::uint64_t closed_bit = std::uint64_t{1} << 63;
        static constexpr std::uint64_t count_mask = closed_bit - 1;

        class admission_guard {
        public:
            explicit admission_guard(cpu_scheduler* scheduler = nullptr) noexcept
                :
                m_scheduler(scheduler)
            {}
            admission_guard(const admission_guard&) = delete;
            admission_guard& operator=(const admission_guard&) = delete;
            admission_guard(admission_guard&& rhs) noexcept
                :
                m_scheduler(std::exchange(rhs.m_scheduler, nullptr))
            {}
            ~admission_guard() noexcept {
                if(m_scheduler != nullptr) {
                    m_scheduler->finish_pending_one();
                }
            }
            [[nodiscard]] explicit operator bool() const noexcept {
                return m_scheduler != nullptr;
            }
            void release() noexcept { m_scheduler = nullptr; }
        private:
            cpu_scheduler* m_scheduler;
        };
    public:
        template<class Result>
        class future {
        public:
            using value_type = Result;
            future() noexcept = default;
            future(const future&) = delete;
            future& operator=(const future&) = delete;
            future(future&& rhs) noexcept
                :
                m_scheduler(std::exchange(rhs.m_scheduler, nullptr)),
                m_slot(std::exchange(rhs.m_slot, nullptr))
            {}

            future& operator=(future&& rhs) noexcept {
                if(this == std::addressof(rhs)) {
                    return *this;
                }

                reset();
                m_scheduler = std::exchange(rhs.m_scheduler, nullptr);
                m_slot = std::exchange(rhs.m_slot, nullptr);

                return *this;
            }

            ~future() noexcept {
                reset();
            }

            [[nodiscard]] bool valid() const noexcept {
                return m_slot != nullptr;
            }

            [[nodiscard]] bool ready() const noexcept {
                return m_slot != nullptr && m_slot->ready();
            }

            void wait() const {
                DEBUG_ASSERT(m_slot != nullptr);
                if(!m_slot->ready()) {
                    m_scheduler->wait_for(*m_slot);
                }
            }

            Result get() {
                DEBUG_ASSERT(m_slot != nullptr);
                auto* const slot = std::exchange(m_slot, nullptr);
                auto* const scheduler = std::exchange(m_scheduler, nullptr);

                struct release_on_exit {
                    slot_type* slot;
                    ~release_on_exit() noexcept { slot->release_reference(); }
                } cleanup{slot};

                if(!slot->ready()) {
                    scheduler->wait_for(*slot);
                }

                if(const auto exception = slot->exception()) {
                    std::rethrow_exception(exception);
                }

                if constexpr(!std::is_void_v<Result>) {
                    return slot->template take_result<Result>();
                }
            }

            class awaiter {
            public:
                awaiter(cpu_scheduler* const scheduler, slot_type* const slot) noexcept
                    :
                    m_scheduler(scheduler),
                    m_slot(slot)
                {}

                awaiter(const awaiter&) = delete;
                awaiter& operator=(const awaiter&) = delete;
                ~awaiter() noexcept {
                    if(m_slot != nullptr) {
                        m_slot->release_reference();
                    }
                }

                [[nodiscard]] bool await_ready() const noexcept {
                    return m_slot->ready();
                }

                template<schedulable_promise Promise>
                [[nodiscard]] bool await_suspend(const std::coroutine_handle<Promise> current) noexcept {
                    return m_slot->suspend(
                        as_work(current), 
                        m_scheduler,
                        [](void* context, schedulable_work* work) noexcept {
                            static_cast<cpu_scheduler*>(context)->post_existing(work);
                        }
                    );
                }

                Result await_resume() {
                    future result{m_scheduler, std::exchange(m_slot, nullptr)};
                    return result.get();
                }
            private:
                cpu_scheduler* m_scheduler;
                slot_type* m_slot;
            };

            [[nodiscard]] awaiter operator co_await() && noexcept {
                DEBUG_ASSERT(m_slot != nullptr);
                return awaiter{std::exchange(m_scheduler, nullptr), std::exchange(m_slot, nullptr)};
            }

            awaiter operator co_await() & = delete;
        private:
            friend cpu_scheduler;

            future(cpu_scheduler* const scheduler, slot_type* const slot) noexcept
                :
                m_scheduler(scheduler),
                m_slot(slot)
            {}

            void reset() noexcept {
                if(m_slot != nullptr) {
                    m_slot->release_reference();
                    m_slot = nullptr;
                    m_scheduler = nullptr;
                }
            }

            cpu_scheduler* m_scheduler{};
            slot_type* m_slot{};
        };

        explicit cpu_scheduler(scheduler_config config = {})
            :
            m_config(validate_config(config)),
            m_global_queue(m_config.global_queue_capacity),
            m_ready(m_config.worker_count),
            m_idle_block_count((m_config.worker_count + 63U) / 64U),
            m_idle_blocks(std::make_unique<std::atomic<std::uint64_t>[]>(m_idle_block_count))
        {
            for(std::size_t i = 0; i != m_idle_block_count; ++i) {
                m_idle_blocks[i].store(0, std::memory_order::relaxed);
            }

            m_workers.reserve(m_config.worker_count);
            m_threads.reserve(m_config.worker_count);

            const auto scheduler_seed = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this));

            for(std::uint32_t i = 0; i != m_config.worker_count; ++i) {
                m_workers.emplace_back(
                    std::make_unique<worker_state>(
                        i,
                        m_config,
                        scheduler_seed ^ (0x9e3779b97f4a7c15ULL * (static_cast<std::uint64_t>(i) + 1))
                    )
                );
            }

            try {
                for(std::uint32_t i = 0; i != m_config.worker_count; ++i) {
                    m_threads.emplace_back([this, i] { worker_loop(*m_workers[i]); });
                }
            }
            catch(...) {
                request_stop();
                for(auto& thread : m_threads) {
                    thread.join();
                }
                throw;
            }

            m_ready.wait();
        }

        cpu_scheduler(const cpu_scheduler&) = delete;
        cpu_scheduler& operator=(const cpu_scheduler&) = delete;
        cpu_scheduler(cpu_scheduler&&) = delete;
        cpu_scheduler& operator=(cpu_scheduler&&) = delete;

        ~cpu_scheduler() noexcept {
            shutdown();
        }

        [[nodiscard]] std::uint32_t worker_count() const noexcept {
            return m_config.worker_count;
        }

        [[nodiscard]] static cpu_scheduler* current() noexcept {
            return tls_scheduler;
        }

        [[nodiscard]] std::size_t pending() const noexcept {
            return static_cast<std::size_t>(m_work_state.load(std::memory_order::acquire) & count_mask);
        }

        template<class Fnc, class... Args>
        [[nodiscard]] auto submit(Fnc&& fnc, Args&&... args) {
            using fnc_type = std::decay_t<Fnc>;
            using model_type = invocation<fnc_type, std::decay_t<Args>...>;

            using invocation_result = std::invoke_result_t<fnc_type, std::decay_t<Args>...>;
            using result_type = std::remove_cv_t<invocation_result>;

            static_assert(!std::is_reference_v<result_type>);
            static_assert(sizeof(model_type) <= TaskStorageSize);
            static_assert(alignof(model_type) <= TaskStorageAlignment);

            auto admission = try_admit();
            if(!admission) {
                throw scheduler_stopped{};
            }
            auto* const slot = acquire_slot();

            try {
                slot->template prepare<result_type, model_type>(
                    std::forward<Fnc>(fnc),
                    std::forward<Args>(args)...
                );
            }
            catch(...) {
                slot->release_reference();
                throw;
            }

            future<result_type> result {this, slot};
            admission.release();
            publish(slot);
            return result;
        }

        template<class Fnc, class... Args>
        [[nodiscard]] auto try_submit(Fnc&& fnc, Args&&... args) {
            using function_type = std::decay_t<Fnc>;
            using model_type = invocation<function_type, std::decay_t<Args>...>;

            using invocation_result = std::invoke_result_t<function_type, std::decay_t<Args>...>;
            using result_type = std::remove_cv_t<invocation_result>;

            static_assert(!std::is_reference_v<result_type>);
            static_assert(sizeof(model_type) <= TaskStorageSize);
            static_assert(alignof(model_type) <= TaskStorageAlignment);

            using future_type = future<result_type>;

            auto admission = try_admit();
            if(!admission) {
                return std::optional<future_type>{};
            }

            auto* const slot = try_acquire_slot();

            if(slot == nullptr) {
                return std::optional<future_type>{};
            }

            try {
                slot->template prepare<result_type, model_type>(
                    std::forward<Fnc>(fnc),
                    std::forward<Args>(args)...
                );
            }
            catch(...) {
                slot->release_reference();
                throw;
            }

            future_type result{this, slot};
            admission.release();
            publish(slot);

            return std::optional<future_type>{ std::move(result) };
        }

        template<class Fnc, class... Args>
        void submit_detached(Fnc&& fnc, Args&&... args) {
            using model_type = invocation<std::decay_t<Fnc>, std::decay_t<Args>...>;

            static_assert(sizeof(model_type) <= TaskStorageSize);
            static_assert(alignof(model_type) <= TaskStorageAlignment);

            auto admission = try_admit();
            if(!admission) {
                throw scheduler_stopped{};
            }
            auto* const slot = acquire_slot();

            try {
                slot->template prepare_detached<model_type>(
                    m_config.unhandled_exception,
                    std::forward<Fnc>(fnc),
                    std::forward<Args>(args)...
                );
            }
            catch(...) {
                slot->release_reference();
                throw;
            }

            admission.release();
            publish(slot);
        }

        template<class Fnc, class... Args>
        [[nodiscard]] bool try_submit_detached(Fnc&& fnc, Args&&... args) {
            using model_type = invocation<std::decay_t<Fnc>, std::decay_t<Args>...>;

            static_assert(sizeof(model_type) <= TaskStorageSize);
            static_assert(alignof(model_type) <= TaskStorageAlignment);

            auto admission = try_admit();
            if(!admission) {
                return false;
            }

            auto* const slot = try_acquire_slot();

            if(slot == nullptr) {
                return false;
            }

            try {
                slot->template prepare_detached<model_type>(
                    m_config.unhandled_exception,
                    std::forward<Fnc>(fnc),
                    std::forward<Args>(args)...
                );
            }
            catch(...) {
                slot->release_reference();
                throw;
            }

            admission.release();
            publish(slot);
            return true;
        }

        void request_stop() noexcept {
            m_work_state.fetch_or(closed_bit, std::memory_order::acq_rel);
            wake_all();
        }

        void shutdown() noexcept {
            // A worker can request stop, but cannot join its own thread.
            if(is_worker_thread()) [[unlikely]] {
                std::terminate();
            }

            if(m_shutdown_started.exchange(true, std::memory_order::acq_rel)) {
                m_shutdown_complete.wait(false, std::memory_order::acquire);
                return;
            }

            request_stop();
            for(auto& thread : m_threads) {
                if(thread.joinable()) {
                    thread.join();
                }
            }

            m_shutdown_complete.store(true, std::memory_order::release);
            m_shutdown_complete.notify_all();
        }

        [[nodiscard]] bool is_worker_thread() const noexcept {
            return tls_scheduler == this;
        }

        class schedule_awaiter {
        public:
            schedule_awaiter(cpu_scheduler& scheduler, const bool yield, const bool admitted = false) noexcept
                :
                m_scheduler(scheduler),
                m_yield(yield),
                m_admitted(admitted)
            {}

            [[nodiscard]] bool await_ready() const noexcept {
                return !m_yield && m_scheduler.is_worker_thread();
            }

            template<schedulable_promise Promise>
            void await_suspend(const std::coroutine_handle<Promise> current) {
                auto* const work = as_work(current);

                if(m_admitted) {
                    m_scheduler.post_existing(work, m_yield);
                    return;
                }

                auto admission = m_scheduler.try_admit();
                
                if(!admission) {
                    throw scheduler_stopped{};
                }

                auto* const scheduler = std::addressof(m_scheduler);
                const auto yield = m_yield;
                admission.release();
                scheduler->publish(work, true, yield);
            }

            static void await_resume() noexcept {}
        private:
            cpu_scheduler& m_scheduler;
            bool m_yield;
            bool m_admitted;
        };

        [[nodiscard]] schedule_awaiter schedule() noexcept { return {*this, false}; }
        [[nodiscard]] schedule_awaiter yield() noexcept { return {*this, true}; }

        template<class Result>
        [[nodiscard]] coro::task<Result> schedule(coro::task<Result> child) {
            co_await schedule();
            co_return co_await std::move(child);
        }

        void post(schedulable_work* const work) noexcept {
            DEBUG_ASSERT(is_worker_thread());
            post_existing(work);
        }

        [[nodiscard]] bool post_external(schedulable_work* const work) noexcept {
            DEBUG_ASSERT(work != nullptr);

            auto admission = try_admit();
            if(!admission) [[unlikely]] {
                return false;
            }

            admission.release();
            publish(work, true, true);

            return true;
        }

        template<class Result>
        [[nodiscard]] future<Result> submit(coro::task<Result> child) {
            DEBUG_ASSERT(child.valid());
            auto admission = try_admit();

            if(!admission) {
                throw scheduler_stopped{};
            }

            auto* const slot = acquire_slot();
            slot->template prepare_external<Result>();
            future<Result> result{this, slot};

            try {
                complete_task(this, slot, std::move(child), std::move(admission));
            }
            catch(...) {
                slot->release_reference();
                throw;
            }

            return result;
        }

        template<class Result>
        [[nodiscard]] std::optional<future<Result>> try_submit(coro::task<Result>&& child) {
            DEBUG_ASSERT(child.valid());
            auto admission = try_admit();

            if(!admission) {
                return {};
            }

            auto* const slot = try_acquire_slot();

            if(slot == nullptr) {
                return {};
            }

            slot->template prepare_external<Result>();
            future<Result> result{this, slot};

            try {
                complete_task(this, slot, std::move(child), std::move(admission));
            }
            catch(...) {
                slot->release_reference();
                throw;
            }

            return std::optional<future<Result>>{std::move(result)};
        }

        template<class Result>
        void submit_detached(coro::task<Result> child) {
            DEBUG_ASSERT(child.valid());
            auto admission = try_admit();
            if(!admission) {
                throw scheduler_stopped{};
            }
            complete_detached(this, std::move(child), std::move(admission));
        }
    private:
        template<class Result>
        static coro::detached_task complete_task(cpu_scheduler* scheduler, slot_type* slot,
            coro::task<Result> child, [[maybe_unused]] admission_guard admission) {
            co_await schedule_awaiter{*scheduler, true, true};

            try {
                if constexpr(std::is_void_v<Result>) {
                    co_await std::move(child);
                    slot->complete();
                }
                else {
                    slot->complete_value(co_await std::move(child));
                }
            }
            catch(...) {
                slot->complete_exception(std::current_exception());
            }
            slot->release_reference();
        }

        template<class Result>
        static coro::detached_task complete_detached(cpu_scheduler* scheduler,
            coro::task<Result> child, [[maybe_unused]] admission_guard admission) {
            co_await schedule_awaiter{*scheduler, true, true};

            try {
                static_cast<void>(co_await std::move(child));
            }
            catch(...) {
                scheduler->m_config.unhandled_exception(std::current_exception());
            }
        }

        [[nodiscard]] static scheduler_config validate_config(scheduler_config cfg) {
            if(cfg.worker_count == 0) {
                throw std::invalid_argument("worker_count must be greater than zero");
            }

            if(cfg.local_queue_capacity < 2 || !std::has_single_bit(cfg.local_queue_capacity)) {
                throw std::invalid_argument("local_queue_capacity must be a power of two greater than one");
            }

            if(cfg.global_queue_capacity < 2 || !std::has_single_bit(cfg.global_queue_capacity)) {
                throw std::invalid_argument("global_queue_capacity must be a power of two");
            }

            if(cfg.task_slots_per_worker == 0) {
                throw std::invalid_argument("task_slots_per_worker must be greater than zero");
            }

            if(cfg.global_poll_interval == 0) {
                throw std::invalid_argument("global_poll_interval must be greater than zero");
            }


            if(cfg.steal_batch_size == 0 || cfg.steal_batch_size > MaximumStealBatch || cfg.steal_batch_size > cfg.local_queue_capacity) {
                throw std::invalid_argument("steal_batch_size is outside the supported range");
            }

            if(cfg.unhandled_exception == nullptr) {
                cfg.unhandled_exception = std::addressof(terminate_on_unhandled_exception);
            }

            return cfg;
        }

        [[nodiscard]] admission_guard try_admit() noexcept {
            if(is_worker_thread()) {
                m_work_state.fetch_add(1, std::memory_order::relaxed);
                return admission_guard{this};
            }

            auto state = m_work_state.load(std::memory_order::relaxed);

            while((state & closed_bit) == 0) {
                if(m_work_state.compare_exchange_weak(state, state + 1,
                    std::memory_order::acq_rel, std::memory_order::relaxed)) {
                    return admission_guard{this};
                }
            }
            return admission_guard{};
        }

        void post_existing(schedulable_work* const work, const bool global = false) noexcept {
            m_work_state.fetch_add(1, std::memory_order::relaxed);
            publish(work, true, global);
        }

        [[nodiscard]] slot_type* try_acquire_slot() noexcept {
            if(tls_scheduler == this) {
                if(auto* slot = tls_worker->task_slots->try_acquire()) {
                    return slot;
                }
            }

            const auto count = static_cast<uint32_t>(m_workers.size());
            auto index = external_rng.bounded(count);

            for(std::uint32_t i = 0; i != count; ++i) {
                if(auto* slot = m_workers[index]->task_slots->try_acquire()) {
                    return slot;
                }

                if(++index == count) {
                    index = 0;
                }
            }
            return nullptr;
        }

        [[nodiscard]] slot_type* acquire_slot() {
            if(auto* const slot = try_acquire_slot()) {
                return slot;
            }
            return slot_type::make_overflow();
        }

        void publish(work_pointer task, const bool stealable = false, const bool global = false) noexcept {
            if(is_worker_thread() && !global) {
                auto& worker = *tls_worker;
                
                if(!stealable) {
                    task = std::exchange(worker.lifo_slot, task);
                    if(task == nullptr) {
                        return;
                    }
                }

                if(worker.local_queue.push(task)) {
                    wake_one();
                    return;
                }
            }

            if(!m_global_queue.try_push(task)) {
                push_overflow(task);
            }
            wake_one();
        }

        void push_overflow(work_pointer work) noexcept {
            std::lock_guard lock{m_overflow_mutex};
            work->next_work = nullptr;

            if(m_overflow_tail != nullptr) {
                m_overflow_tail->next_work = work;
            }
            else {
                m_overflow_head = work;
            }

            m_overflow_tail = work;
            m_has_overflow.store(true, std::memory_order::release);
        }

        [[nodiscard]] bool pop_overflow(work_pointer& out) noexcept {
            if(!m_has_overflow.load(std::memory_order::acquire)) {
                return false;
            }

            std::lock_guard lock{m_overflow_mutex};
            
            if(m_overflow_head == nullptr) {
                return false;
            }

            out = m_overflow_head;
            m_overflow_head = out->next_work;

            if(m_overflow_head == nullptr) {
                m_overflow_tail = nullptr;
                m_has_overflow.store(false, std::memory_order::release);
            }

            return true;
        }

        [[nodiscard]] bool try_get_work(worker_state& worker, work_pointer& out) noexcept {
            bool checked_global = false;

            if(--worker.global_budget == 0) {
                worker.global_budget = m_config.global_poll_interval;
                checked_global = true;

                if(pop_overflow(out) || m_global_queue.try_pop(out)) {
                    return true;
                }
            }

            if(worker.lifo_slot != nullptr) {
                out = std::exchange(worker.lifo_slot, nullptr);
                return true;
            }

            if(const auto work = worker.local_queue.pop()) {
                out = *work;
                return true;
            }

            if(!checked_global && m_global_queue.try_pop(out)) {
                return true;
            }

            if(pop_overflow(out)) {
                return true;
            }
            return steal_batch(worker, out);
        }

        [[nodiscard]] bool steal_batch(worker_state& worker, work_pointer& out) noexcept {
            const auto count = static_cast<std::uint32_t>(m_workers.size());

            if(count <= 1) {
                return false;
            }

            auto victim_index = worker.rng.bounded(count);

            for(std::uint32_t i = 0; i != count; ++i) {
                if(victim_index != worker.index) {
                    auto& victim = *m_workers[victim_index];

                    const auto available = victim.local_queue.size();

                    if(available != 0) {
                        const auto desired = std::min(
                            {
                                static_cast<std::size_t>(m_config.steal_batch_size),
                                worker.stolen_tasks.size(),
                                (available + 1) / 2
                            }
                        );

                        auto dst = std::span<work_pointer>{
                            worker.stolen_tasks.data(),
                            desired
                        };

                        const auto stolen = victim.local_queue.steal_batch(dst);

                        if(stolen != 0) {
                            out = worker.stolen_tasks[0];

                            for(std::size_t j = stolen; j-- > 1;) {
                                auto* const work = worker.stolen_tasks[j];

                                if(worker.local_queue.push(work)) {
                                    continue;
                                }

                                if(m_global_queue.try_push(work)) {
                                    continue;
                                }

                                push_overflow(work);
                            }

                            if(stolen > 1) {
                                wake_one();
                            }

                            return true;
                        }
                    }
                }

                if(++victim_index == count) {
                    victim_index = 0;
                }
            }

            return false;
        }

        void finish_pending_one() noexcept {
            const auto previous = m_work_state.fetch_sub(1, std::memory_order::acq_rel);
            DEBUG_ASSERT((previous & count_mask) != 0);
            if(previous == (closed_bit | 1)) {
                wake_all();
            }
        }

        void execute_work(work_pointer work) noexcept {
            DEBUG_ASSERT(work != nullptr);

            work->dispatch();
            finish_pending_one();
        }

        void wait_for(slot_type& awaited) {
            if(!is_worker_thread()) {
                awaited.wait();
                return;
            }

            backoff bo;
            while(!awaited.ready()) {
                work_pointer work = nullptr;
                if(try_get_work(*tls_worker, work)) {
                    execute_work(work);
                    bo.reset();
                }
                else {
                    bo.snooze();
                }
            }
        }

        [[nodiscard]] bool should_stop() const noexcept {
            return m_work_state.load(std::memory_order::acquire) == closed_bit;
        }

        void worker_loop(worker_state& worker) noexcept {
            tls_scheduler = this;
            tls_worker = &worker;

            m_ready.count_down();

            backoff bo;

            for(;;) {
                work_pointer work = nullptr;

                if(try_get_work(worker, work)) {
                    bo.reset();
                    execute_work(work);
                    continue;
                }

                if(should_stop()) {
                    break;
                }

                bo.snooze();

                if(!bo.is_completed()) {
                    continue;
                }

                const auto block = worker.index / 64U;
                const auto bit = std::uint64_t{1} << (worker.index % 64U);

                m_idle_blocks[block].fetch_or(bit, std::memory_order::acq_rel);
                std::atomic_thread_fence(std::memory_order::seq_cst);

                if(try_get_work(worker, work)) {
                    m_idle_blocks[block].fetch_and(~bit, std::memory_order::seq_cst);
                    bo.reset();

                    execute_work(work);
                    continue;
                }

                if(should_stop()) {
                    m_idle_blocks[block].fetch_and(~bit, std::memory_order::seq_cst);
                    break;
                }

                worker.parker.park();

                m_idle_blocks[block].fetch_and(~bit, std::memory_order::seq_cst);
                bo.reset();
            }

            tls_worker = nullptr;
            tls_scheduler = nullptr;
        }

        bool wake_one() noexcept {
            std::atomic_thread_fence(std::memory_order::seq_cst);

            if(m_idle_block_count == 0) {
                return false;
            }

            auto block = m_wake_cursor.load(std::memory_order::relaxed);
            if(block >= m_idle_block_count) {
                block = 0;
            }

            for(std::size_t i = 0; i != m_idle_block_count; ++i) {
                auto bits = m_idle_blocks[block].load(std::memory_order::seq_cst);

                while(bits != 0) {
                    const auto position = std::countr_zero(bits);
                    const auto mask = std::uint64_t{1} << position;

                    if(m_idle_blocks[block].compare_exchange_weak(bits, bits & ~mask, std::memory_order::seq_cst, std::memory_order::relaxed)) {
                        const auto index = static_cast<std::uint32_t>(block * std::size_t{64} + static_cast<std::size_t>(position));

                        if(index < m_workers.size()) {
                            m_wake_cursor.store(block + 1, std::memory_order::relaxed);
                            m_workers[index]->parker.unpark();
                            return true;
                        }
                    }
                }

                if(++block == m_idle_block_count) {
                    block = 0;
                }
            }
            return false;
        }

        void wake_all() noexcept {
            for(auto& worker : m_workers) {
                worker->parker.unpark();
            }
        }

        scheduler_config m_config;
        mpmc::queue<work_pointer, mpmc::wait_mode::backoff_spin> m_global_queue;
        std::vector<std::unique_ptr<worker_state>> m_workers;
        std::vector<std::jthread> m_threads;
        std::latch m_ready;
        const std::size_t m_idle_block_count;
        std::unique_ptr<std::atomic<std::uint64_t>[]> m_idle_blocks;

        alignas(cache_line_size) std::atomic<std::uint64_t> m_work_state{0};
        alignas(cache_line_size) std::atomic<std::size_t> m_wake_cursor{0};

        std::mutex m_overflow_mutex;
        work_pointer m_overflow_head{};
        work_pointer m_overflow_tail{};
        std::atomic<bool> m_has_overflow{false};
        std::atomic<bool> m_shutdown_complete{false};
        std::atomic<bool> m_shutdown_started{false};

        static inline thread_local fast_rng external_rng {
            static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()))
        };
        static inline thread_local cpu_scheduler* tls_scheduler = nullptr;
        static inline thread_local worker_state* tls_worker = nullptr;
    };
}
