#pragma once

#include "async_logger.hpp"
#include "dispatcher.hpp"
#include "security.hpp"
#include "session.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace grlx::rpc {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

template <typename ChannelT>
class server {
public:
  using channel_type    = ChannelT;
  using buffer_type     = typename ChannelT::buffer_type;
  using encoder_type    = typename ChannelT::encoder_type;
  using session_type    = typename ChannelT::session_type;
  using dispatcher_type = dispatcher<encoder_type>;

  template <typename... ArgsT>
  server(ArgsT&&... args)
    : channel_(std::forward<ArgsT>(args)...) {
    // Don't initialize strand here - wait for start()
  }

  server(server&& other) noexcept
    : channel_(std::move(other.channel_))
    , dispatcher_(std::move(other.dispatcher_))
    , sessions_strand_(std::move(other.sessions_strand_))
    , logger_(std::move(other.logger_)) {
    // Move sessions if strand exists
    if (sessions_strand_) {
      asio::post(*sessions_strand_, [this, &other]() {
        active_sessions_ = std::move(other.active_sessions_);
      });
    } else {
      // If no strand yet, just move the sessions directly
      active_sessions_ = std::move(other.active_sessions_);
    }
  }

  ~server() = default;

  template <typename... ArgsT>
  auto start(ArgsT const&... args) -> asio::awaitable<void> {
    auto executor = co_await asio::this_coro::executor;

    co_await channel_.bind(args...);

    // Initialize strand and logger after bind() so channel has an executor
    if (!sessions_strand_) {
      sessions_strand_ = std::make_unique<asio::strand<typename ChannelT::executor_type>>(asio::make_strand(channel_.get_executor()));
    }

    // Install the connection-cap + rate-limit filter on the channel. Runs
    // pre-handshake so rejected peers never pay for (or force us to pay
    // for) a TLS handshake. Three checks in order:
    //   1. global session cap (cheap atomic)
    //   2. per-IP session cap (locked map)
    //   3. per-IP accept token bucket (same locked map)
    // Only step 3 consumes state; (1) and (2) are read-only probes.
    channel_.set_pre_handshake_filter(
        [this](auto const& peer, std::string& reason) -> bool {
          if (global_session_count_.load(std::memory_order_acquire) >= limits_.max_concurrent_sessions) {
            reason = "global session cap reached";
            return false;
          }
          std::string                 ip = peer.address().to_string();
          std::lock_guard<std::mutex> lock(per_ip_mutex_);

          auto it = per_ip_count_.find(ip);
          if (it != per_ip_count_.end() && it->second >= limits_.max_sessions_per_ip) {
            reason = "per-IP session cap reached for " + ip;
            return false;
          }

          // Bucket is created on first sight of this IP and initialized
          // with the server's current rate config.
          auto& bucket = per_ip_accept_bucket_[ip];
          if (bucket.capacity == 0.0) {
            bucket.capacity       = limits_.accept_burst_per_ip;
            bucket.refill_per_sec = limits_.accept_refill_per_ip_per_s;
          }
          if (!bucket.try_take()) {
            reason = "per-IP accept rate exceeded for " + ip;
            return false;
          }
          return true;
        });

    // if (!logger_) {
    //   logger_ = std::make_shared<async_logger>(executor);
    //   logger_->start();
    // }

    asio::co_spawn(
        executor,
        [this]() -> asio::awaitable<void> {
          auto executor = co_await asio::this_coro::executor;

          for (;;) {
            try {
              auto session = co_await channel_.accept(dispatcher_, session_limits_);

              // Admit the session: bump the global + per-IP counters that
              // back the pre-handshake filter. Order matters — we increment
              // BEFORE adding to active_sessions_, so a concurrent accept
              // sees the new count and can reject flooders before adding
              // more load. Matching decrement happens on session close.
              global_session_count_.fetch_add(1, std::memory_order_acq_rel);
              {
                std::lock_guard<std::mutex> lock(per_ip_mutex_);
                ++per_ip_count_[session->peer_ip()];
              }

              // Add session using strand - guarantees serialized access
              asio::post(*sessions_strand_, [this, session]() {
                active_sessions_.insert(session);
              });

              // Fire the application-visible "session opened" hook with the
              // peer identity captured during the TLS handshake. This is
              // where auth layers learn the cert fingerprint to bind tokens
              // to, and where operators wire up connection logging.
              if (on_session_open_) {
                try {
                  on_session_open_(session_info{
                      .peer_fingerprint = session->peer_fingerprint(),
                      .peer_address     = session->peer_address(),
                  });
                } catch (...) {
                  // A buggy hook must not kill the accept loop.
                  log_error_async("session_open hook threw");
                }
              }

              // Create individual strand for each session
              auto session_strand = asio::make_strand(executor);

              asio::co_spawn(session_strand, session->dispatch_requests(), [session, this](std::exception_ptr error) {
                // Release the seat we took in the global + per-IP counters
                // so a future connect from this IP isn't wrongly rejected.
                global_session_count_.fetch_sub(1, std::memory_order_acq_rel);
                {
                  std::lock_guard<std::mutex> lock(per_ip_mutex_);
                  auto it = per_ip_count_.find(session->peer_ip());
                  if (it != per_ip_count_.end()) {
                    if (--it->second == 0) {
                      per_ip_count_.erase(it);
                    }
                  }
                }

                // Remove session using strand - guarantees serialized access
                asio::post(*sessions_strand_, [this, session]() {
                  active_sessions_.erase(session);
                });

                if (error) {
                  // Log error asynchronously without blocking
                  log_error_async([error]() -> std::string {
                    try {
                      std::rethrow_exception(error);
                    } catch (const std::exception& e) {
                      return std::string("Session error: ") + e.what();
                    } catch (...) {
                      return std::string("Unknown session error");
                    }
                  }());
                }
              });

            } catch (const std::exception& e) {
              // Log error asynchronously without blocking
              log_error_async(std::string("Failed to accept connection: ") + e.what());
            } catch (...) {
              // Log error asynchronously without blocking
              log_error_async("Unknown error accepting connection");
            }
          }
        },
        asio::detached);

    co_return;
  }

  template <typename R, typename C, typename... ArgsT>
  void attach(std::string const& func_name, C* objPtr, R (C::*memFunc)(ArgsT...) const) {
    if (!objPtr) {
      throw std::invalid_argument("Object pointer cannot be null");
    }

    std::function<R(ArgsT...)> call = [objPtr, memFunc](ArgsT&&... args) -> R {
      return (objPtr->*memFunc)(std::forward<ArgsT>(args)...);
    };
    dispatcher_->attach(func_name, std::move(call));
  }

  template <typename R, typename C, typename... ArgsT>
  void attach(std::string const& func_name, C* objPtr, R (C::*memFunc)(ArgsT...)) {
    if (!objPtr) {
      throw std::invalid_argument("Object pointer cannot be null");
    }

    std::function<R(ArgsT...)> call = [objPtr, memFunc](ArgsT&&... args) -> R {
      return (objPtr->*memFunc)(std::forward<ArgsT>(args)...);
    };
    dispatcher_->attach(func_name, std::move(call));
  }

  template <typename F>
  void attach(std::string const& func_name, F&& func) {
    dispatcher_->attach(func_name, std::function{func});
  }

  // visibility-aware overloads. Mark a handler public_ to skip the auth
  // callback, admin to require both allow AND is_admin from the callback.
  template <typename F>
  void attach(std::string const& func_name, visibility vis, F&& func) {
    dispatcher_->attach(func_name, vis, std::function{func});
  }

  template <typename R, typename C, typename... ArgsT>
  void attach(std::string const& func_name, visibility vis, C* objPtr, R (C::*memFunc)(ArgsT...) const) {
    if (!objPtr) throw std::invalid_argument("Object pointer cannot be null");
    std::function<R(ArgsT...)> call = [objPtr, memFunc](ArgsT&&... args) -> R {
      return (objPtr->*memFunc)(std::forward<ArgsT>(args)...);
    };
    dispatcher_->attach(func_name, vis, std::move(call));
  }

  template <typename R, typename C, typename... ArgsT>
  void attach(std::string const& func_name, visibility vis, C* objPtr, R (C::*memFunc)(ArgsT...)) {
    if (!objPtr) throw std::invalid_argument("Object pointer cannot be null");
    std::function<R(ArgsT...)> call = [objPtr, memFunc](ArgsT&&... args) -> R {
      return (objPtr->*memFunc)(std::forward<ArgsT>(args)...);
    };
    dispatcher_->attach(func_name, vis, std::move(call));
  }

  // Install the pre-dispatch auth callback. Called for every non-public
  // method before its handler runs. Must be installed *before* clients start
  // calling authenticated endpoints, or those calls will be denied.
  void set_auth_callback(auth_callback cb) {
    dispatcher_->set_auth_callback(std::move(cb));
  }

  // Connection limits applied before the TLS handshake. Set these before
  // start(), or (safely) while running — changes take effect on the next
  // accept. The safe-by-default values live in server_limits; only override
  // if you have a reason and have thought about DoS consequences.
  void set_server_limits(server_limits limits) {
    limits_ = limits;
  }

  server_limits const& get_server_limits() const noexcept {
    return limits_;
  }

  template <typename... ArgsT>
  auto notify(std::string const& func_name, ArgsT&&... args) -> asio::awaitable<void> {
    if (!sessions_strand_) {
      co_return; // No sessions to notify if strand not initialized
    }

    auto executor = co_await asio::this_coro::executor;

    message_request<typename std::decay<ArgsT>::type...> request{std::make_tuple(std::forward<ArgsT>(args)...)};

    buffer_type buffer;
    encoder_type::encode(buffer, request);

    // Get sessions copy using strand - avoiding data races
    auto sessions_copy = co_await asio::co_spawn(
        *sessions_strand_,
        [this]() -> asio::awaitable<std::unordered_set<std::shared_ptr<session_type>>> {
          co_return active_sessions_; // Copy the set
        },
        asio::use_awaitable);

    // Notify all sessions non-blocking — drop notifications for slow clients
    for (auto& session : sessions_copy) {
      if (!session->try_notify(func_name, buffer)) {
        log_warning_async("Session write channel full, disconnecting slow client");
        asio::post(*sessions_strand_, [this, session]() {
          active_sessions_.erase(session);
        });
        session->close();
      }
    }

    co_return;
  }

  // Get number of active sessions (strand-safe)
  auto session_count() -> asio::awaitable<std::size_t> {
    if (!sessions_strand_) {
      co_return 0;
    }

    co_return co_await asio::co_spawn(
        *sessions_strand_,
        [this]() -> asio::awaitable<std::size_t> {
          co_return active_sessions_.size();
        },
        asio::use_awaitable);
  }

  // Gracefully close all sessions (strand-safe)
  auto close_all_sessions() -> asio::awaitable<void> {
    if (!sessions_strand_) {
      co_return;
    }

    co_return co_await asio::co_spawn(
        *sessions_strand_,
        [this]() -> asio::awaitable<void> {
          for (auto& session : active_sessions_) {
            try {
              session->close();
            } catch (...) {
              // Ignore errors during cleanup
            }
          }
          active_sessions_.clear();
          co_return;
        },
        asio::use_awaitable);
  }

  auto stop() -> asio::awaitable<void> {
    co_await close_all_sessions();
    co_return;
  }

  channel_type& channel() {
    return channel_;
  }

  const channel_type& channel() const {
    return channel_;
  }

  // Configure the per-session security limits applied to every newly accepted
  // connection. Must be called before start() to take effect for the first
  // clients; changes after start() affect only subsequently accepted sessions.
  void set_session_limits(session_limits limits) {
    session_limits_ = limits;
  }

  session_limits const& get_session_limits() const {
    return session_limits_;
  }

  // Fires once per accepted session, right after the TLS handshake has
  // completed and the session has been registered. The callback receives
  // peer identity (cert fingerprint, remote address) captured during the
  // handshake and should be fast and non-throwing. Use it for per-session
  // auth setup, structured connection logs, or metrics.
  using session_open_callback = std::function<void(session_info const&)>;
  void on_session_open(session_open_callback cb) {
    on_session_open_ = std::move(cb);
  }

private:
  // Helper methods for non-blocking logging
  void log_error_async(const std::string& message) {
    // if (logger_) {
    //   asio::co_spawn(
    //       logger_->strand_,
    //       [this, message]() -> asio::awaitable<void> {
    //         co_await logger_->error(message);
    //         co_return;
    //       },
    //       asio::detached);
    // }
  }

  void log_warning_async(const std::string& message) {
    // if (logger_) {
    //   asio::co_spawn(
    //       logger_->strand_,
    //       [this, message]() -> asio::awaitable<void> {
    //         co_await logger_->warning(message);
    //         co_return;
    //       },
    //       asio::detached);
    // }
  }

private:
  channel_type                                                    channel_;
  std::shared_ptr<dispatcher_type>                                dispatcher_ = std::make_shared<dispatcher_type>();
  std::unordered_set<std::shared_ptr<session_type>>               active_sessions_;
  std::unique_ptr<asio::strand<typename ChannelT::executor_type>> sessions_strand_; // Strand for session management - initialized in start()
  std::shared_ptr<async_logger>                                   logger_;
  session_limits                                                  session_limits_{};
  session_open_callback                                           on_session_open_;
  server_limits                                                   limits_{};
  std::atomic<std::size_t>                                        global_session_count_{0};
  std::mutex                                                      per_ip_mutex_;
  std::unordered_map<std::string, std::size_t>                    per_ip_count_;
  std::unordered_map<std::string, token_bucket>                   per_ip_accept_bucket_;
};

} // namespace grlx::rpc