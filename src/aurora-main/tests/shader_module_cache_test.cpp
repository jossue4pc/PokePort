#include "gx/shader_module_cache.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace aurora::gx {
namespace {
using namespace std::chrono_literals;

class PendingCompilation {
public:
  PendingCompilation(ShaderModuleCache<int>& cache, const std::string& key, int value,
                     std::atomic<int>& compilations, bool fail = false)
  : m_started(m_start.get_future()),
    m_result(std::async(std::launch::async, [&, key, value, fail] {
      return cache.get_or_create(key, [&] {
        ++compilations;
        const auto release = m_release.get_future();
        m_start.set_value();
        if (release.wait_for(10s) != std::future_status::ready) {
          throw std::runtime_error("compile release timed out");
        }
        if (fail) {
          throw std::runtime_error("compile failed");
        }
        return value;
      });
    })) {}

  ~PendingCompilation() {
    release();
    if (m_result.valid()) {
      m_result.wait();
    }
  }

  bool started() { return m_started.wait_for(5s) == std::future_status::ready; }

  std::shared_future<int> finish() {
    release();
    return m_result.get();
  }

private:
  void release() {
    if (!m_released) {
      m_release.set_value();
      m_released = true;
    }
  }

  std::promise<void> m_start;
  std::promise<void> m_release;
  std::future<void> m_started;
  std::future<std::shared_future<int>> m_result;
  bool m_released = false;
};

TEST(ShaderModuleCacheTest, PendingRequestsShareCompilationAcrossEviction) {
  ShaderModuleCache<int> cache{2};
  std::atomic<int> compilations{0};
  PendingCompilation owner{cache, "pending", 42, compilations};
  ASSERT_TRUE(owner.started());

  std::vector<std::shared_future<int>> waiters;
  for (int i = 0; i < 8; ++i) {
    auto request = std::async(std::launch::async, [&] {
      return cache.get_or_create("pending", [&] { return ++compilations; });
    });
    waiters.push_back(request.get());
    EXPECT_EQ(waiters.back().wait_for(0s), std::future_status::timeout);
  }
  for (int i = 0; i < 64; ++i) {
    EXPECT_EQ(cache.get_or_create(std::to_string(i), [i] { return i; }).get(), i);
    EXPECT_LE(cache.ready_count(), 2);
  }
  EXPECT_EQ(cache.pending_count(), 1);
  EXPECT_EQ(compilations.load(), 1);
  EXPECT_EQ(owner.finish().get(), 42);
  EXPECT_EQ(cache.pending_count(), 0);
  EXPECT_EQ(cache.ready_count(), 2);

  cache.get_or_create("evict1", [] { return 1; });
  cache.get_or_create("evict2", [] { return 2; });
  for (const auto& waiter : waiters) {
    EXPECT_EQ(waiter.get(), 42);
  }
  EXPECT_EQ(cache.get_or_create("pending", [&] { return 41 + ++compilations; }).get(), 43);
  EXPECT_EQ(compilations.load(), 2);
}

TEST(ShaderModuleCacheTest, DifferentKeysCompileOutsideTheCacheLock) {
  ShaderModuleCache<int> cache{1};
  std::atomic<int> compilations{0};
  PendingCompilation first{cache, "first", 1, compilations};
  ASSERT_TRUE(first.started());
  PendingCompilation second{cache, "second", 2, compilations};
  ASSERT_TRUE(second.started());
  EXPECT_EQ(cache.pending_count(), 2);
  EXPECT_EQ(cache.ready_count(), 0);
  EXPECT_EQ(second.finish().get(), 2);
  EXPECT_EQ(first.finish().get(), 1);
  EXPECT_EQ(cache.ready_count(), 1);
  EXPECT_EQ(compilations.load(), 2);
}

TEST(ShaderModuleCacheTest, ReadyHitsRefreshLruOrder) {
  ShaderModuleCache<int> cache{2};
  int compilations = 0;
  const auto get = [&](const char* key) {
    return cache.get_or_create(key, [&] { return ++compilations; }).get();
  };
  EXPECT_EQ(get("a"), 1);
  EXPECT_EQ(get("b"), 2);
  EXPECT_EQ(get("a"), 1);
  EXPECT_EQ(get("c"), 3);
  EXPECT_EQ(get("a"), 1);
  EXPECT_EQ(get("b"), 4);
  EXPECT_EQ(cache.ready_count(), 2);
}

TEST(ShaderModuleCacheTest, EvictionAndClearReleaseOnlyCacheOwnership) {
  ShaderModuleCache<std::shared_ptr<int>> cache{1};
  auto first = cache.get_or_create("first", [] { return std::make_shared<int>(42); });
  std::weak_ptr<int> resource = first.get();
  std::weak_ptr<int> cachedOnly;
  cache.get_or_create("second", [&] {
    auto module = std::make_shared<int>(0);
    cachedOnly = module;
    return module;
  });
  EXPECT_FALSE(resource.expired());
  EXPECT_FALSE(cachedOnly.expired());
  EXPECT_EQ(*first.get(), 42);
  cache.clear();
  EXPECT_TRUE(cachedOnly.expired());
  EXPECT_FALSE(resource.expired());
  first = {};
  EXPECT_TRUE(resource.expired());
  EXPECT_EQ(cache.ready_count(), 0);
  EXPECT_EQ(cache.pending_count(), 0);
}

TEST(ShaderModuleCacheTest, ClearDoesNotPublishOldCompletionOrEraseNewPendingWork) {
  ShaderModuleCache<int> cache{1};
  std::atomic<int> compilations{0};
  PendingCompilation old{cache, "same", 1, compilations};
  ASSERT_TRUE(old.started());
  const auto oldWaiter = cache.get_or_create("same", [] { return -1; });
  cache.clear();
  EXPECT_EQ(cache.pending_count(), 0);
  EXPECT_EQ(cache.ready_count(), 0);
  PendingCompilation current{cache, "same", 2, compilations};
  ASSERT_TRUE(current.started());

  EXPECT_EQ(old.finish().get(), 1);
  EXPECT_EQ(oldWaiter.get(), 1);
  EXPECT_EQ(cache.pending_count(), 1);
  EXPECT_EQ(cache.ready_count(), 0);
  const auto currentWaiter = cache.get_or_create("same", [] { return -1; });
  EXPECT_EQ(currentWaiter.wait_for(0s), std::future_status::timeout);
  EXPECT_EQ(current.finish().get(), 2);
  EXPECT_EQ(currentWaiter.get(), 2);
  EXPECT_EQ(cache.get_or_create("same", [] { return -1; }).get(), 2);
  EXPECT_EQ(cache.ready_count(), 1);
  EXPECT_EQ(compilations.load(), 2);
}

TEST(ShaderModuleCacheTest, ExceptionsReachEveryWaiterAndAllowRetry) {
  ShaderModuleCache<int> cache{1};
  std::atomic<int> compilations{0};
  PendingCompilation failed{cache, "failed", 0, compilations, true};
  ASSERT_TRUE(failed.started());
  const auto waiter = cache.get_or_create("failed", [] { return -1; });
  EXPECT_THROW(failed.finish().get(), std::runtime_error);
  EXPECT_THROW(waiter.get(), std::runtime_error);
  EXPECT_EQ(cache.pending_count(), 0);
  EXPECT_EQ(cache.ready_count(), 0);
  EXPECT_EQ(cache.get_or_create("failed", [&] { return ++compilations; }).get(), 2);
  EXPECT_EQ(cache.ready_count(), 1);
}

TEST(ShaderModuleCacheTest, FailedOldCompletionCannotEraseReplacement) {
  ShaderModuleCache<int> cache{1};
  std::atomic<int> compilations{0};
  PendingCompilation old{cache, "same", 0, compilations, true};
  ASSERT_TRUE(old.started());
  cache.clear();
  EXPECT_EQ(cache.get_or_create("same", [] { return 2; }).get(), 2);
  EXPECT_THROW(old.finish().get(), std::runtime_error);
  EXPECT_EQ(cache.get_or_create("same", [] { return -1; }).get(), 2);
  EXPECT_EQ(cache.ready_count(), 1);
}

TEST(ShaderModuleCacheTest, BinaryKeysRetainTheirFullConfiguration) {
  ShaderModuleCache<int> cache{4};
  const std::string first{"a\0b", 3};
  const std::string second{"a\0c", 3};
  EXPECT_EQ(cache.get_or_create(first, [] { return 1; }).get(), 1);
  EXPECT_EQ(cache.get_or_create(second, [] { return 2; }).get(), 2);
  EXPECT_EQ(cache.get_or_create(first, [] { return -1; }).get(), 1);
  EXPECT_EQ(cache.get_or_create(second, [] { return -1; }).get(), 2);
  EXPECT_EQ(cache.ready_count(), 2);
}

TEST(ShaderModuleCacheTest, EmptyModulesRetainTheExistingFailureResult) {
  ShaderModuleCache<std::shared_ptr<int>> cache{1};
  int compilations = 0;
  const auto compile = [&] {
    ++compilations;
    return std::shared_ptr<int>{};
  };
  EXPECT_EQ(cache.get_or_create("empty", compile).get(), nullptr);
  EXPECT_EQ(cache.get_or_create("empty", compile).get(), nullptr);
  EXPECT_EQ(compilations, 1);
  EXPECT_EQ(cache.ready_count(), 1);
  cache.clear();
  EXPECT_EQ(cache.get_or_create("empty", compile).get(), nullptr);
  EXPECT_EQ(compilations, 2);
}

TEST(ShaderModuleCacheTest, ZeroReadyBudgetStillDeduplicatesPendingWork) {
  ShaderModuleCache<int> cache{0};
  std::atomic<int> compilations{0};
  PendingCompilation owner{cache, "same", 1, compilations};
  ASSERT_TRUE(owner.started());
  const auto waiter = cache.get_or_create("same", [] { return -1; });
  EXPECT_EQ(owner.finish().get(), 1);
  EXPECT_EQ(waiter.get(), 1);
  EXPECT_EQ(cache.ready_count(), 0);
  EXPECT_EQ(cache.get_or_create("same", [&] { return ++compilations; }).get(), 2);
  EXPECT_EQ(cache.ready_count(), 0);
}

TEST(ShaderModuleCacheTest, DISABLED_BenchmarkWorkingSet) {
  for (const size_t limit : {128, 256, 512, 8192}) {
    for (const size_t workingSet : {128, 512, 8192}) {
      ShaderModuleCache<int> cache{limit};
      std::vector<std::string> keys;
      for (size_t i = 0; i < workingSet; ++i) {
        keys.push_back(std::to_string(i));
      }
      size_t compilations = 0;
      size_t checksum = 0;
      const auto start = std::chrono::steady_clock::now();
      constexpr size_t Requests = 131072;
      for (size_t i = 0; i < Requests; ++i) {
        checksum += cache.get_or_create(keys[i % workingSet], [&] {
          ++compilations;
          return 1;
        }).get();
      }
      const auto elapsed = std::chrono::steady_clock::now() - start;
      std::cout << "module_cache limit=" << limit << " working_set=" << workingSet
                << " requests=" << Requests << " compilations=" << compilations
                << " ready=" << cache.ready_count() << " ns_per_request="
                << std::chrono::duration<double, std::nano>(elapsed).count() / Requests << '\n';
      EXPECT_EQ(checksum, Requests);
      EXPECT_LE(cache.ready_count(), limit);
    }
  }
}

} // namespace
} // namespace aurora::gx
