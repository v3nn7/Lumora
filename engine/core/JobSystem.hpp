#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace lumora {

/// Fixed pool of worker threads executing submitted jobs FIFO.
///
/// - submit() returns a std::future (exceptions propagate through it).
/// - parallelFor() splits a range into chunks; the calling thread works too, so it is safe to
///   call from the main thread and never deadlocks waiting for busy workers.
/// Jobs must not block waiting for other queued jobs (use the caller-runs pattern instead).
class JobSystem {
public:
    /// workers == 0: hardware_concurrency() - 1 (at least one).
    explicit JobSystem(unsigned workers = 0);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    /// Process-wide pool, created on first use.
    [[nodiscard]] static JobSystem& global();

    template <class F>
    auto submit(F&& job) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        using R = std::invoke_result_t<std::decay_t<F>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(job));
        std::future<R> future = task->get_future();
        enqueue([task] { (*task)(); });
        return future;
    }

    /// Calls fn(begin, end) over [0, count) in chunks of at most `grain` items; blocks until done.
    void parallelFor(std::size_t count, std::size_t grain, const std::function<void(std::size_t, std::size_t)>& fn);

    [[nodiscard]] unsigned workerCount() const noexcept { return static_cast<unsigned>(workers_.size()); }
    /// Jobs waiting to start (diagnostics).
    [[nodiscard]] std::size_t pending() const;

private:
    void enqueue(std::move_only_function<void()> job);
    bool runOne();  // executes one queued job on the calling thread if available
    void workerLoop(std::stop_token stop);

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<std::move_only_function<void()>> queue_;
    std::vector<std::jthread> workers_;
};

} // namespace lumora
