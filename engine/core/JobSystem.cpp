#include "core/JobSystem.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <latch>

namespace lumora {

JobSystem::JobSystem(unsigned workers) {
    if (workers == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        workers = hw > 1 ? hw - 1 : 1;
    }
    workers_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        workers_.emplace_back([this](std::stop_token stop) { workerLoop(stop); });
    }
}

JobSystem::~JobSystem() {
    for (auto& w : workers_) w.request_stop();
    wake_.notify_all();
    workers_.clear();  // joins; queued jobs that never started are dropped with their futures broken
}

JobSystem& JobSystem::global() {
    static JobSystem system;
    return system;
}

void JobSystem::enqueue(std::move_only_function<void()> job) {
    {
        std::scoped_lock lock(mutex_);
        queue_.push_back(std::move(job));
    }
    wake_.notify_one();
}

std::size_t JobSystem::pending() const {
    std::scoped_lock lock(mutex_);
    return queue_.size();
}

bool JobSystem::runOne() {
    std::move_only_function<void()> job;
    {
        std::scoped_lock lock(mutex_);
        if (queue_.empty()) return false;
        job = std::move(queue_.front());
        queue_.pop_front();
    }
    job();
    return true;
}

void JobSystem::workerLoop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::move_only_function<void()> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (stop.stop_requested() || queue_.empty()) continue;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job();
    }
}

void JobSystem::parallelFor(std::size_t count, std::size_t grain,
                            const std::function<void(std::size_t, std::size_t)>& fn) {
    if (count == 0) return;
    grain = std::max<std::size_t>(grain, 1);
    const std::size_t chunks = (count + grain - 1) / grain;
    if (chunks == 1) {
        fn(0, count);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::exception_ptr failure;
    std::mutex failureMutex;
    auto drain = [&] {
        for (std::size_t c = next.fetch_add(1); c < chunks; c = next.fetch_add(1)) {
            try {
                fn(c * grain, std::min(count, (c + 1) * grain));
            } catch (...) {
                std::scoped_lock lock(failureMutex);
                if (!failure) failure = std::current_exception();
            }
        }
    };
    // Helpers grab chunks while the caller does the same; the latch counts finished helpers.
    const auto helpers = static_cast<std::ptrdiff_t>(std::min<std::size_t>(workers_.size(), chunks - 1));
    std::latch done(helpers);
    for (std::ptrdiff_t i = 0; i < helpers; ++i) {
        enqueue([&] {
            drain();
            done.count_down();
        });
    }
    drain();
    // While waiting, help with any queued job (possibly our own helpers) instead of idling.
    while (!done.try_wait()) {
        if (!runOne()) std::this_thread::yield();
    }
    if (failure) std::rethrow_exception(failure);
}

} // namespace lumora
