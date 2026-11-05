#include "core/JobSystem.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <stdexcept>

using namespace lumora;

TEST(JobSystem, SubmitReturnsResults) {
    JobSystem jobs(3);
    EXPECT_EQ(jobs.workerCount(), 3u);
    std::vector<std::future<int>> results;
    for (int i = 0; i < 100; ++i) results.push_back(jobs.submit([i] { return i * i; }));
    int sum = 0;
    for (auto& r : results) sum += r.get();
    EXPECT_EQ(sum, 328350);
}

TEST(JobSystem, ExceptionsPropagateThroughFutures) {
    JobSystem jobs(1);
    auto f = jobs.submit([]() -> int { throw std::runtime_error("boom"); });
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(JobSystem, ParallelForCoversEveryIndexOnce) {
    JobSystem jobs(4);
    std::vector<std::atomic<int>> hits(10007);
    jobs.parallelFor(hits.size(), 64, [&](std::size_t b, std::size_t e) {
        for (std::size_t i = b; i < e; ++i) hits[i].fetch_add(1);
    });
    for (const auto& h : hits) ASSERT_EQ(h.load(), 1);
}

TEST(JobSystem, ParallelForRethrowsAndStillFinishes) {
    JobSystem jobs(2);
    std::atomic<int> processed{0};
    EXPECT_THROW(jobs.parallelFor(100, 1,
                                  [&](std::size_t b, std::size_t) {
                                      processed.fetch_add(1);
                                      if (b == 50) throw std::logic_error("chunk 50");
                                  }),
                 std::logic_error);
    EXPECT_EQ(processed.load(), 100) << "other chunks still ran";
}

TEST(JobSystem, NestedParallelForFromAJobDoesNotDeadlock) {
    JobSystem jobs(2);
    auto f = jobs.submit([&] {
        std::atomic<int> n{0};
        jobs.parallelFor(1000, 10, [&](std::size_t b, std::size_t e) { n.fetch_add(static_cast<int>(e - b)); });
        return n.load();
    });
    EXPECT_EQ(f.get(), 1000);
}
