#pragma once

#include "buffer_pool.hpp"
#include "dispatcher.hpp"
#include "message.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>

#include <spdlog/spdlog.h>

#include <array>
#include <functional>
#include <future>
#include <istream>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace grlx::rpc {

namespace asio = boost::asio;

template <typename ChannelT>
class client {
public:
  using buffer_type               = typename ChannelT::buffer_type;
  using encoder_type              = typename ChannelT::encoder_type;
  using session_type              = typename ChannelT::session_type;
  using dispatcher_type           = dispatcher<encoder_type>;
  using notification_handler_type = std::function<void(buffer_type const&)>;

  template <typename... ArgsT>
  client(ArgsT&&... args)
    : channel_(std::forward<ArgsT>(args)...)
    , buffer_pool_(32, 1024, 4096) {
  }

  client(client&& other)
    : channel_(std::move(other.channel_))
    , client_session_(std::move(other.client_session_))
    , buffer_pool_(std::move(other.buffer_pool_))
    , function_hash_cache_(std::move(other.function_hash_cache_)) {
  }

  ~client() = default;

  client(client const&)            = delete;
  client& operator=(client const&) = delete;

  template <typename EndpointT>
  asio::awaitable<void> connect(EndpointT const& endpoint) {
    auto executor = co_await asio::this_coro::executor;

    client_session_ = co_await channel_.connect(endpoint);
    co_await client_session_->handshake();

    // Set up notification handling in the session
    setup_notification_handling();

    asio::co_spawn(executor, client_session_->dispatch_requests(), [](std::exception_ptr error) {
      if (error) {
        spdlog::error("rpc::client::dispatch_requests error:");
        std::rethrow_exception(error);
      }
    });

    co_return;
  }

  template <typename ReturnT, typename... ArgsT>
  auto invoke(std::string const& func_name, ArgsT&&... args) -> asio::awaitable<ReturnT> {
    // Optimize: Check connection first before any allocations
    if (!client_session_) [[unlikely]] {
      throw std::runtime_error("not connected");
    }

    // Fix: Use buffer pool correctly - the session.call() needs a const reference, not moved buffer
    auto  req_buffer_guard = buffer_pool_.get_buffer();
    auto& req_buffer       = *req_buffer_guard;

    message_request<typename std::decay<ArgsT>::type...> request{std::make_tuple(std::forward<ArgsT>(args)...)};

    if (!encoder_type::encode(req_buffer, request)) [[unlikely]] {
      throw std::runtime_error("Failed to encode request");
    }

    // Fix: Pass const reference, not moved buffer
    auto rsp_buffer = co_await client_session_->call(func_name, req_buffer);

    message_response<typename std::decay<ReturnT>::type> response;

    if (!encoder_type::decode(rsp_buffer, response)) [[unlikely]] {
      throw std::runtime_error("Failed to decode response");
    }

    if constexpr (std::is_void<ReturnT>::value) {
      co_return;
    } else {
      spdlog::debug("Invoke finished: {}", func_name);
      co_return std::move(response.result); // Move result for efficiency
    }
  }

  // template <typename ReturnT, typename... ArgsT>
  // auto invoke_async(std::string const& func_name, ArgsT&&... args) -> std::future<ReturnT> {
  //   if (!client_session_) {
  //     auto promise = std::promise<ReturnT>();
  //     promise.set_exception(std::make_exception_ptr(std::runtime_error("not connected")));
  //     return promise.get_future();
  //   }

  //   auto  call_id          = get_cached_function_hash(func_name);
  //   auto  req_buffer_guard = buffer_pool_.get_buffer();
  //   auto& req_buffer       = *req_buffer_guard;

  //   message_request<typename std::decay<ArgsT>::type...> request{std::make_tuple(std::forward<ArgsT>(args)...)};

  //   if (!encoder_type::encode(req_buffer, request)) {
  //     auto promise = std::promise<ReturnT>();
  //     promise.set_exception(std::make_exception_ptr(std::runtime_error("Failed to encode request")));
  //     return promise.get_future();
  //   }

  //   auto [operation, guard] = client_session_->call_future(func_name, req_buffer);

  //   auto promise = std::make_shared<std::promise<ReturnT>>();
  //   auto future  = promise->get_future();

  //   operation.set_completion_handler([promise](buffer_type&& rsp_buffer) {
  //     try {
  //       message_response<typename std::decay<ReturnT>::type> response;
  //       if (!encoder_type::decode(rsp_buffer, response)) {
  //         promise->set_exception(std::make_exception_ptr(std::runtime_error("Failed to decode response")));
  //         return;
  //       }

  //       if constexpr (std::is_void<ReturnT>::value) {
  //         promise->set_value();
  //       } else {
  //         promise->set_value(std::move(response.result));
  //       }
  //     } catch (...) {
  //       promise->set_exception(std::current_exception());
  //     }
  //   });

  //   return future;
  // }

  // Notification handling methods - with automatic type deduction!
  template <typename... ArgsT>
  void register_notification_handler(std::string const& notification_name, std::function<void(ArgsT...)>&& handler) {
    auto wrapper = [handler = std::move(handler)](buffer_type const& buffer) {
      message_request<typename std::decay<ArgsT>::type...> request;
      if (encoder_type::decode(buffer, request)) {
        std::apply(handler, request.args);
      }
    };

    auto hash_value                    = shash64(notification_name).value();
    notification_handlers_[hash_value] = std::move(wrapper);
  }

  // Automatic type deduction for lambdas and function objects (like server's attach!)
  template <typename F>
  void register_notification_handler(std::string const& notification_name, F&& func) {
    register_notification_handler(notification_name, std::function{std::forward<F>(func)});
  }

  void unregister_notification_handler(std::string const& notification_name) {
    auto hash_value = shash64(notification_name).value();
    notification_handlers_.erase(hash_value);
  }

  void clear_notification_handlers() {
    notification_handlers_.clear();
  }

  bool has_notification_handler(std::string const& notification_name) const {
    auto hash_value = shash64(notification_name).value();
    return notification_handlers_.find(hash_value) != notification_handlers_.end();
  }

  size_t notification_handler_count() const {
    return notification_handlers_.size();
  }

  ChannelT& channel() {
    return channel_;
  }

  const ChannelT& channel() const {
    return channel_;
  }

  bool is_connected() const noexcept {
    return client_session_ != nullptr;
  }

  void disconnect() {
    // channel_.close();
    client_session_.reset();
    function_hash_cache_.clear();
    notification_handlers_.clear();
  }

  struct performance_stats {
    size_t total_calls        = 0;
    size_t buffer_pool_hits   = 0;
    size_t buffer_pool_misses = 0;
    size_t hash_cache_hits    = 0;
    size_t hash_cache_misses  = 0;

    double buffer_pool_hit_ratio() const {
      auto total = buffer_pool_hits + buffer_pool_misses;
      return total > 0 ? static_cast<double>(buffer_pool_hits) / total : 0.0;
    }

    double hash_cache_hit_ratio() const {
      auto total = hash_cache_hits + hash_cache_misses;
      return total > 0 ? static_cast<double>(hash_cache_hits) / total : 0.0;
    }
  };

  performance_stats get_performance_stats() const {
    performance_stats stats;
    stats.total_calls        = total_calls_;
    stats.buffer_pool_hits   = buffer_pool_.hit_count();
    stats.buffer_pool_misses = buffer_pool_.miss_count();
    stats.hash_cache_hits    = hash_cache_hits_;
    stats.hash_cache_misses  = hash_cache_misses_;
    return stats;
  }

private:
  std::uint64_t get_cached_function_hash(const std::string& func_name) {
    auto it = function_hash_cache_.find(func_name);
    if (it != function_hash_cache_.end()) {
      hash_cache_hits_++;
      return it->second;
    }

    auto hash_value                 = shash64(func_name).value();
    function_hash_cache_[func_name] = hash_value;
    hash_cache_misses_++;
    return hash_value;
  }

  void setup_notification_handling() {
    if (!client_session_) {
      return;
    }

    // Set up the notification callback for the session
    auto notification_callback = [this](std::uint64_t call_id, buffer_type const& buffer) {
      handle_notification(call_id, buffer);
    };

    client_session_->set_notification_callback(std::move(notification_callback));
  }

  void handle_notification(std::uint64_t call_id, buffer_type const& buffer) {
    auto it = notification_handlers_.find(call_id);
    if (it != notification_handlers_.end()) {
      try {
        it->second(buffer);
      } catch (const std::exception& e) {
        // Log error or handle notification processing error
        // For now, we'll silently ignore errors in notification handling
        spdlog::error("Error in notification handling: {}", e.what());
      } catch (...) {
        // Ignore unknown errors in notification handling
        spdlog::error("Unknown error in notification handling");
      }
    }
  }

private:
  ChannelT                      channel_;
  std::shared_ptr<session_type> client_session_;

  mutable buffer_pool buffer_pool_;

  std::unordered_map<std::string, std::uint64_t>               function_hash_cache_;
  std::unordered_map<std::uint64_t, notification_handler_type> notification_handlers_;

  mutable std::atomic<size_t> total_calls_{0};
  mutable std::atomic<size_t> hash_cache_hits_{0};
  mutable std::atomic<size_t> hash_cache_misses_{0};
};
} // namespace grlx::rpc