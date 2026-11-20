#pragma once

#include "../gfx/lru_cache.hpp"

#include <cstddef>
#include <exception>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace aurora::gx {

template <typename Module>
class ShaderModuleCache {
public:
  // Native module sizes are unavailable, so each completed module costs one unit.
  explicit ShaderModuleCache(size_t readyLimit) : m_ready({.bytes = readyLimit}) {}

  template <typename Compile>
  std::shared_future<Module> get_or_create(const std::string& key, Compile&& compile) {
    std::promise<Module> promise;
    std::shared_future<Module> shared;
    size_t generation;
    {
      std::lock_guard lock{m_mutex};
      if (const auto* ready = m_ready.find(key)) {
        return *ready;
      }
      if (const auto it = m_pending.find(key); it != m_pending.end()) {
        return it->second;
      }
      shared = promise.get_future().share();
      m_pending.emplace(key, shared);
      generation = m_generation;
    }

    bool succeeded = false;
    try {
      promise.set_value(std::forward<Compile>(compile)());
      succeeded = true;
    } catch (...) {
      promise.set_exception(std::current_exception());
    }
    {
      std::lock_guard lock{m_mutex};
      // A completion from before clear must not populate the new device's cache.
      if (generation == m_generation) {
        if (succeeded) {
          m_ready.insert(key, shared, 1);
        }
        m_pending.erase(key);
      }
    }
    return shared;
  }

  void clear() {
    std::lock_guard lock{m_mutex};
    ++m_generation;
    m_pending.clear();
    m_ready.clear();
  }

  size_t ready_count() const {
    std::lock_guard lock{m_mutex};
    return m_ready.cached_count();
  }

  size_t pending_count() const {
    std::lock_guard lock{m_mutex};
    return m_pending.size();
  }

private:
  mutable std::mutex m_mutex;
  std::unordered_map<std::string, std::shared_future<Module>> m_pending;
  gfx::LruCache<std::string, std::shared_future<Module>> m_ready;
  size_t m_generation = 0;
};

} // namespace aurora::gx
