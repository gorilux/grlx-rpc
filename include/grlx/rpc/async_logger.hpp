#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <queue>
#include <string>

namespace grlx::rpc {

namespace asio = boost::asio;

enum class log_level {
  debug   = 0,
  info    = 1,
  warning = 2,
  error   = 3
};

class async_logger : public std::enable_shared_from_this<async_logger> {
public:
  struct log_entry {
    log_level   level;
    std::string message;
  };

  explicit async_logger(asio::any_io_executor     executor,
                        log_level                 min_level      = log_level::info,
                        std::chrono::milliseconds flush_interval = std::chrono::milliseconds(100))
    : strand_(asio::make_strand(executor))
    , min_level_(min_level)
    , flush_timer_(strand_, flush_interval)
    , flush_interval_(flush_interval)
    , is_stopped_(false) {
    // Don't start the worker in constructor - call start() after construction
  }

  // Must be called after construction to start the logger
  void start() {
    // Start the background logging coroutine - capture shared_from_this()
    auto self = shared_from_this();
    asio::co_spawn(
        strand_,
        [self]() {
          return self->log_worker();
        },
        asio::detached);
  }

  // Stop the logger gracefully
  void stop() {
    is_stopped_.store(true);
    flush_timer_.cancel();
  }

  ~async_logger() {
    stop();
  }

  auto log(log_level level, std::string message) -> asio::awaitable<void> {
    if (level < min_level_) {
      co_return; // Skip logging below threshold
    }

    if (is_stopped_.load()) {
      co_return; // Don't log after stopped
    }

    log_entry entry{.level = level, .message = std::move(message)};

    auto self = shared_from_this();
    co_await asio::co_spawn(
        strand_,
        [self, entry = std::move(entry)]() mutable -> asio::awaitable<void> {
          if (self->is_stopped_.load()) {
            co_return; // Don't log after stopped
          }

          self->pending_logs_.push(std::move(entry));

          // If this is the first entry, restart the timer
          if (self->pending_logs_.size() == 1) {
            self->flush_timer_.expires_after(self->flush_interval_);
          }

          co_return;
        },
        asio::use_awaitable);
  }

  // Convenience methods
  auto debug(std::string message) -> asio::awaitable<void> {
    co_return co_await log(log_level::debug, std::move(message));
  }

  auto info(std::string message) -> asio::awaitable<void> {
    co_return co_await log(log_level::info, std::move(message));
  }

  auto warning(std::string message) -> asio::awaitable<void> {
    co_return co_await log(log_level::warning, std::move(message));
  }

  auto error(std::string message) -> asio::awaitable<void> {
    co_return co_await log(log_level::error, std::move(message));
  }

  auto flush() -> asio::awaitable<void> {
    auto self = shared_from_this();
    co_await asio::co_spawn(
        strand_,
        [self]() -> asio::awaitable<void> {
          self->flush_pending_logs();
          co_return;
        },
        asio::use_awaitable);
  }

private:
  auto log_worker() -> asio::awaitable<void> {
    while (!is_stopped_.load()) {
      try {
        co_await flush_timer_.async_wait(asio::use_awaitable);
        if (!is_stopped_.load()) {
          flush_pending_logs();
        }
      } catch (const boost::system::system_error& e) {
        if (e.code() == asio::error::operation_aborted) {
          // Timer was cancelled, likely shutting down
          break;
        }
        // Real error, continue anyway
      }
    }

    // Flush any remaining logs before exiting
    flush_pending_logs();
    co_return;
  }

  void flush_pending_logs() {
    while (!pending_logs_.empty()) {
      const auto& entry = pending_logs_.front();

      // Use spdlog for logging
      switch (entry.level) {
        case log_level::debug:
          spdlog::debug("{}", entry.message);
          break;
        case log_level::info:
          spdlog::info("{}", entry.message);
          break;
        case log_level::warning:
          spdlog::warn("{}", entry.message);
          break;
        case log_level::error:
          spdlog::error("{}", entry.message);
          break;
        default:
          spdlog::info("{}", entry.message);
          break;
      }

      pending_logs_.pop();
    }

    // Reset timer for next batch
    if (!pending_logs_.empty()) {
      flush_timer_.expires_after(flush_interval_);
    }
  }

public: // Made public so server can access it
  asio::strand<asio::any_io_executor> strand_;

private:
  log_level                 min_level_;
  asio::steady_timer        flush_timer_;
  std::chrono::milliseconds flush_interval_;
  std::queue<log_entry>     pending_logs_;
  std::atomic<bool>         is_stopped_;
};

} // namespace grlx::rpc
