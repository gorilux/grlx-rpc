#pragma once

#include "buffer_type.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>

namespace grlx::rpc {

class buffer_pool {
public:
  static constexpr size_t DEFAULT_INITIAL_SIZE = 32;
  static constexpr size_t DEFAULT_MAX_SIZE     = 1024;
  static constexpr size_t DEFAULT_BUFFER_SIZE  = 4096;

  explicit buffer_pool(size_t initial_size = DEFAULT_INITIAL_SIZE, size_t max_size = DEFAULT_MAX_SIZE, size_t buffer_size = DEFAULT_BUFFER_SIZE)
    : max_size_(max_size)
    , default_size_(buffer_size) {

    // Pre-allocate buffers
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < initial_size; ++i) {
      auto buffer = std::make_unique<buffer_type>();
      buffer->reserve(buffer_size);
      available_buffers_.push(std::move(buffer));
    }
  }

  // RAII wrapper for automatic buffer return
  class buffer_guard {
  public:
    buffer_guard(std::unique_ptr<buffer_type> buffer, buffer_pool* pool)
      : buffer_(std::move(buffer))
      , pool_(pool) {
    }

    ~buffer_guard() {
      if (buffer_ && pool_) {
        pool_->return_buffer(std::move(buffer_));
      }
    }

    buffer_guard(const buffer_guard&)            = delete;
    buffer_guard& operator=(const buffer_guard&) = delete;

    buffer_guard(buffer_guard&& other) noexcept
      : buffer_(std::move(other.buffer_))
      , pool_(std::exchange(other.pool_, nullptr)) {
    }

    buffer_guard& operator=(buffer_guard&& other) noexcept {
      if (this != &other) {
        buffer_ = std::move(other.buffer_);
        pool_   = std::exchange(other.pool_, nullptr);
      }
      return *this;
    }

    buffer_type* get() const {
      return buffer_.get();
    }
    buffer_type& operator*() const {
      return *buffer_;
    }
    buffer_type* operator->() const {
      return buffer_.get();
    }

    void release() {
      buffer_.reset();
      pool_ = nullptr;
    }

  private:
    std::unique_ptr<buffer_type> buffer_;
    buffer_pool*                 pool_;
  };

  auto get_buffer() -> buffer_guard {
    std::lock_guard lock(mutex_);

    if (!available_buffers_.empty()) {
      auto buffer = std::move(available_buffers_.front());
      available_buffers_.pop();
      buffer->clear(); // Clear but keep capacity
      hits_.fetch_add(1, std::memory_order_relaxed);
      return buffer_guard(std::move(buffer), this);
    }

    // Pool empty, create new buffer
    auto buffer = std::make_unique<buffer_type>();
    buffer->reserve(default_size_);
    misses_.fetch_add(1, std::memory_order_relaxed);
    return buffer_guard(std::move(buffer), this);
  }

  void return_buffer(std::unique_ptr<buffer_type> buffer) {
    if (!buffer)
      return;

    std::lock_guard lock(mutex_);

    // Only return to pool if we haven't exceeded max size
    if (available_buffers_.size() < max_size_) {
      // Shrink if buffer grew too large
      if (buffer->capacity() > default_size_ * 4) {
        buffer->shrink_to_fit();
        buffer->reserve(default_size_);
      }
      available_buffers_.push(std::move(buffer));
    }
    // else: let buffer be destroyed
  }

  // Statistics
  size_t available_count() const {
    std::lock_guard lock(mutex_);
    return available_buffers_.size();
  }

  size_t hit_count() const {
    return hits_.load(std::memory_order_relaxed);
  }
  size_t miss_count() const {
    return misses_.load(std::memory_order_relaxed);
  }

  double hit_ratio() const {
    auto total = hit_count() + miss_count();
    return total > 0 ? static_cast<double>(hit_count()) / total : 0.0;
  }

private:
  mutable std::mutex                       mutex_;
  std::queue<std::unique_ptr<buffer_type>> available_buffers_;
  const size_t                             max_size_;
  const size_t                             default_size_;

  std::atomic<size_t> hits_{0};
  std::atomic<size_t> misses_{0};
};

// Thread-local buffer pools for even better performance
class thread_local_buffer_pool {
public:
  static auto get_buffer() -> buffer_pool::buffer_guard {
    thread_local buffer_pool pool;
    return pool.get_buffer();
  }
};

} // namespace grlx::rpc
