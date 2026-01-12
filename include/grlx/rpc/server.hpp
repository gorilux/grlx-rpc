#pragma once

#include "async_logger.hpp"
#include "dispatcher.hpp"
#include "session.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>

#include <memory>
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
              auto session = co_await channel_.accept(dispatcher_);

              // Add session using strand - guarantees serialized access
              asio::post(*sessions_strand_, [this, session]() {
                active_sessions_.insert(session);
              });

              // Create individual strand for each session
              auto session_strand = asio::make_strand(executor);

              asio::co_spawn(session_strand, session->dispatch_requests(), [session, this](std::exception_ptr error) {
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

    // Notify all sessions in parallel for better performance
    std::vector<asio::awaitable<void>> notify_tasks;
    notify_tasks.reserve(sessions_copy.size());

    for (auto& session : sessions_copy) {
      notify_tasks.emplace_back([this, session, &func_name, &buffer]() -> asio::awaitable<void> {
        try {
          co_await session->notify(func_name, buffer);
        } catch (const std::exception& e) {
          // Log warning asynchronously without blocking
          log_warning_async(std::string("Failed to notify session: ") + e.what());
          // Remove failed session using strand
          asio::post(*sessions_strand_, [this, session]() {
            active_sessions_.erase(session);
          });
        } catch (...) {
          // Log warning asynchronously without blocking
          log_warning_async("Unknown error notifying session");
          // Remove failed session using strand
          asio::post(*sessions_strand_, [this, session]() {
            active_sessions_.erase(session);
          });
        }
      }());
    }

    // Wait for all notifications to complete
    for (auto& task : notify_tasks) {
      co_await std::move(task);
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
};

} // namespace grlx::rpc