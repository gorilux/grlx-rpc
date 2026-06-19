#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace grlx::rpc {

namespace asio = boost::asio;

template <typename BufferType, typename TokenType = uint64_t>
class async_manager {
public:
  using token_type    = TokenType;
  using buffer_type   = BufferType;
  using executor_type = asio::any_io_executor;
  using result_type   = std::pair<boost::system::error_code, buffer_type>;
  using channel_type  = asio::experimental::concurrent_channel<executor_type, void(boost::system::error_code, result_type)>;
  using channel_ptr   = std::shared_ptr<channel_type>;

  explicit async_manager(executor_type executor)
    : executor_(std::move(executor))
    , next_token_(1) {
  }

private:
  friend struct operation_token;

  executor_type                               executor_;
  std::atomic<token_type>                     next_token_;
  mutable std::mutex                          mutex_;
  std::unordered_map<token_type, channel_ptr> channels_;

public:
  // Simple: Register handler immediately, get token
  // Returns: token that will be used in the RPC header
  template <typename Handler>
  token_type register_handler(Handler&& handler) {
    auto token   = generate_token();
    auto channel = create_channel();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      channels_[token] = channel;
    }

    // Spawn a coroutine to wait for the result and invoke the handler
    asio::co_spawn(
        executor_,
        [channel, handler = std::forward<Handler>(handler)]() mutable -> asio::awaitable<void> {
          auto [channel_ec, result] = co_await channel->async_receive(asio::as_tuple(asio::use_awaitable));

          if (!channel_ec) {
            auto [ec, buffer] = std::move(result);
            handler(ec, std::move(buffer));
          } else {
            // Channel error (e.g., closed)
            handler(channel_ec, buffer_type{});
          }
        },
        asio::detached);

    return token;
  }

  // Complete operation - send result through the channel
  auto complete_operation(token_type token, boost::system::error_code ec, buffer_type result) -> asio::awaitable<bool> {
    channel_ptr channel;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto                        it = channels_.find(token);
      if (it == channels_.end()) {
        co_return false;
      }
      channel = it->second;
      channels_.erase(it);
    }

    // Send the result through the channel
    co_await channel->async_send(boost::system::error_code{}, std::make_pair(ec, std::move(result)), asio::use_awaitable);
    co_return true;
  }

  // Convenience overload
  auto complete_operation(token_type token, buffer_type result) -> asio::awaitable<bool> {
    co_return co_await complete_operation(token, boost::system::error_code{}, std::move(result));
  }

  // Complete with error
  auto complete_operation_with_error(token_type token, boost::system::error_code ec = boost::asio::error::operation_aborted)
      -> asio::awaitable<bool> {
    co_return co_await complete_operation(token, ec, buffer_type{});
  }

  // Check if operation exists
  bool has_operation(token_type token) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return channels_.count(token) > 0;
  }

  // Get pending operations count
  size_t pending_operations_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return channels_.size();
  }

  // Cancel all operations
  void cancel_all_operations() {
    std::unordered_map<token_type, channel_ptr> channels_copy;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      channels_copy = std::move(channels_);
      channels_.clear();
    }

    // Close all channels (this will cause waiting operations to complete with error)
    // Use try-catch to handle any executor-related issues during cleanup
    for (auto& [token, channel] : channels_copy) {
      try {
        channel->cancel();
        channel->close();
      } catch (...) {
        // Ignore errors during cleanup - the executor might be destroyed
      }
    }
  }

  // ASIO-compatible async operation
  // Usage: co_await async_manager.async_operation<buffer_type>(token, use_awaitable);
  template <typename ResultType = buffer_type, typename CompletionToken>
  auto async_operation(token_type token, CompletionToken&& completion_token) {
    return asio::async_initiate<CompletionToken, void(boost::system::error_code, ResultType)>(
        [this, token](auto&& handler) {
          // Register the handler immediately
          register_handler([handler = std::decay_t<decltype(handler)>(std::forward<decltype(handler)>(handler))](boost::system::error_code ec,
                                                                                                                 buffer_type result) mutable {
            std::move(handler)(ec, std::move(result));
          });
        },
        completion_token);
  }

  // For cases where you need token first, then await
  struct operation_token {
    token_type  token;
    channel_ptr channel;

    operation_token(token_type t, channel_ptr ch)
      : token(t)
      , channel(std::move(ch)) {
    }

    // async_wait without timeout
    template <typename CompletionToken, typename ResultType = buffer_type>
    auto async_wait(CompletionToken&& completion_token) {
      return asio::async_initiate<CompletionToken, void(boost::system::error_code, ResultType)>(
          [this](auto&& handler) {
            auto ch = channel;
            asio::co_spawn(
                ch->get_executor(),
                [ch, handler = std::decay_t<decltype(handler)>(std::forward<decltype(handler)>(handler))]() mutable -> asio::awaitable<void> {
                  auto [channel_ec, result] = co_await ch->async_receive(asio::as_tuple(asio::use_awaitable));

                  if (!channel_ec) {
                    auto [ec, buffer] = std::move(result);
                    handler(ec, std::move(buffer));
                  } else {
                    // Channel error (e.g., closed)
                    handler(channel_ec, buffer_type{});
                  }
                },
                asio::detached);
          },
          completion_token);
    }

    // async_wait with timeout.
    //
    // Deliberately does NOT use awaitable_operators::`||` / parallel_group to
    // race the receive against the timer. Under SSL + the tight concurrent
    // call loads the file_sync chunked transfer generates, parallel_group's
    // per-call cancellation-state alloc/teardown corrupted the heap and
    // aborted in scudo with a misaligned-pointer free (the
    // ~parallel_group_op_handler crash session.hpp's deadline_read already
    // had to migrate away from). Instead we run a manual deadline timer that
    // cancels the channel on expiry — exactly the deadline_read pattern.
    template <typename CompletionToken, typename ResultType = buffer_type>
    auto async_wait(std::chrono::milliseconds timeout, CompletionToken&& completion_token) {
      return asio::async_initiate<CompletionToken, void(boost::system::error_code, ResultType)>(
          [this, timeout](auto&& handler) {
            auto ch = channel;
            asio::co_spawn(
                ch->get_executor(),
                [ch,
                 timeout,
                 handler = std::decay_t<decltype(handler)>(std::forward<decltype(handler)>(handler))]() mutable -> asio::awaitable<void> {
                  // Shared between this coroutine and the timer callback, which
                  // may run on a different worker thread of a multi-threaded
                  // io_context — hence atomics, not plain bools.
                  struct wait_state {
                    asio::steady_timer timer;
                    std::atomic<bool>  timed_out{false};
                    std::atomic<bool>  finished{false};
                    explicit wait_state(asio::any_io_executor ex) : timer(std::move(ex)) {}
                  };
                  auto st = std::make_shared<wait_state>(ch->get_executor());
                  st->timer.expires_after(timeout);
                  st->timer.async_wait([st, ch](boost::system::error_code ec) {
                    if (ec) return;                 // timer cancelled → receive already completed
                    if (st->finished.load()) return;
                    st->timed_out.store(true);
                    ch->cancel();                   // unblocks async_receive with operation_aborted
                  });

                  auto [channel_ec, data] =
                      co_await ch->async_receive(asio::as_tuple(asio::use_awaitable));
                  st->finished.store(true);
                  st->timer.cancel();

                  if (st->timed_out.load()) {
                    handler(asio::error::timed_out, buffer_type{});
                  } else if (!channel_ec) {
                    auto [ec, buffer] = std::move(data);
                    handler(ec, std::move(buffer));
                  } else {
                    // Channel error (e.g., closed / cancelled by teardown)
                    handler(channel_ec, buffer_type{});
                  }
                },
                asio::detached);
          },
          completion_token);
    }
  };

  // Create token first, register handler later when async_wait is called
  operation_token create_operation() {
    auto token   = generate_token();
    auto channel = create_channel();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      channels_[token] = channel;
    }

    return operation_token{token, channel};
  }

private:
  token_type generate_token() {
    return next_token_.fetch_add(1, std::memory_order_relaxed);
  }

  channel_ptr create_channel() {
    // Channel with buffer size of 1 (single result per operation)
    return std::make_shared<channel_type>(executor_, 1);
  }
};

} // namespace grlx::rpc