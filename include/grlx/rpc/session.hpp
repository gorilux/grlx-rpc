#pragma once

#include "async_logger.hpp"
#include "async_manager.hpp"
#include "dispatcher.hpp"

#include <grlx/tmpl/string_hash.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <memory>
#include <unordered_map>

namespace grlx::rpc {

namespace asio = boost::asio;

template <typename AsyncStreamT, typename EncoderT>
class session : public std::enable_shared_from_this<session<AsyncStreamT, EncoderT>> {

public:
  using next_layer_type    = AsyncStreamT;
  using encoder_type       = EncoderT;
  using buffer_type        = typename EncoderT::buffer_type;
  using dispatcher_type    = dispatcher<encoder_type>;
  using header_type        = std::array<std::uint64_t, 5>;
  using executor_type      = typename AsyncStreamT::executor_type;
  using async_manager_type = async_manager<buffer_type>;

  // Response channel type
  using response_channel           = asio::experimental::concurrent_channel<void(boost::system::error_code, buffer_type)>;
  using notification_callback_type = std::function<void(std::uint64_t, buffer_type const&)>;

  // Header layout
  static inline const std::uint64_t MAGIC_HEADER_NUMBER = 0xBADC0FFEE;
  static inline const int           MAGIC_HEADER_IDX    = 0;
  static inline const int           MSG_SIZE_IDX        = 1;
  static inline const int           MSG_TYPE_IDX        = 2;
  static inline const int           CALL_ID_IDX         = 3;
  static inline const int           USER_TOKEN_IDX      = 4;

  enum class msg_type : std::uint32_t {
    nop,
    request,
    response,
    notification,
    error
  };

  session(AsyncStreamT&& stream)
    : session(std::move(stream), std::make_shared<dispatcher_type>()) {
  }

  session(AsyncStreamT&& stream, std::shared_ptr<dispatcher_type> const& disp)
    : stream_(std::move(stream))
    , dispatcher_(disp)
    , logger_(std::make_shared<async_logger>(stream_.get_executor()))
    , strand_(asio::make_strand(stream_.get_executor()))
    , async_manager_(strand_) {
    // Start the logger worker after construction
    logger_->start();
  }

  session(session&& other)
    : stream_(std::move(other.stream_))
    , dispatcher_(std::move(other.dispatcher_))
    , logger_(std::move(other.logger_))
    , strand_(std::move(other.strand_)) {
  }

  virtual ~session() {
    close();
  }

  auto handshake() -> asio::awaitable<bool> {
    co_return true;
  }

  auto call(std::string const& call_name, buffer_type const& req_buffer, std::chrono::milliseconds timeout = std::chrono::seconds(30))
      -> asio::awaitable<buffer_type> {

    auto executor = co_await asio::this_coro::executor;
    auto call_id  = shash64(call_name);

    // Create a channel for this request (capacity 1)

    // Register the channel and get a token
    auto async_op = async_manager_.create_operation();

    // Build and send request
    header_type req_header;
    req_header[MAGIC_HEADER_IDX] = MAGIC_HEADER_NUMBER;
    req_header[MSG_SIZE_IDX]     = req_buffer.size();
    req_header[MSG_TYPE_IDX]     = static_cast<std::uint64_t>(msg_type::request) << 32;
    req_header[CALL_ID_IDX]      = call_id.value();
    req_header[USER_TOKEN_IDX]   = async_op.token;

    std::array<boost::asio::const_buffer, 2> buffers = {asio::buffer(req_header.data(), req_header.size() * sizeof(header_type::value_type)),
                                                        asio::buffer(req_buffer)};

    // spdlog::debug("Write {}: 0x{:X} {} 0x{:X} {} {}",
    //               __FUNCTION__,
    //               req_header[MAGIC_HEADER_IDX],
    //               msg_type_to_string(to_msg_type(req_header[MSG_TYPE_IDX])),
    //               req_header[CALL_ID_IDX],
    //               req_header[USER_TOKEN_IDX],
    //               req_header[MSG_SIZE_IDX]);

    co_await asio::async_write(stream_, buffers, asio::transfer_all(), asio::use_awaitable);

    // Wait for response with timeout (using built-in timeout support)
    auto [ec, buffer] = co_await async_op.async_wait(timeout, asio::as_tuple(asio::use_awaitable));

    if (ec) {
      if (ec == asio::error::timed_out) {
        throw std::runtime_error("RPC call timeout: " + call_name);
      }
      throw boost::system::system_error(ec, "RPC call failed");
    }

    co_return buffer;
  }

  auto dispatch_requests() -> asio::awaitable<void> {
    co_await msg_reader();
    co_return;
  }

  auto notify(std::string const& call_name, buffer_type const& buffer) -> asio::awaitable<void> {
    auto call_id = shash64(call_name);

    header_type notification_header;
    notification_header[MAGIC_HEADER_IDX] = MAGIC_HEADER_NUMBER;
    notification_header[MSG_SIZE_IDX]     = buffer.size();
    notification_header[CALL_ID_IDX]      = call_id.value();
    notification_header[USER_TOKEN_IDX]   = 0;
    notification_header[MSG_TYPE_IDX]     = (static_cast<std::uint64_t>(msg_type::notification) << 32);

    std::array<boost::asio::const_buffer, 2> buffers = {
        asio::buffer(notification_header.data(), notification_header.size() * sizeof(header_type::value_type)),
        asio::buffer(buffer)};

    // spdlog::debug("Write {}: 0x{:X} {} 0x{:X} {} {}",
    //               __FUNCTION__,
    //               notification_header[MAGIC_HEADER_IDX],
    //               msg_type_to_string(to_msg_type(notification_header[MSG_TYPE_IDX])),
    //               notification_header[CALL_ID_IDX],
    //               notification_header[USER_TOKEN_IDX],
    //               notification_header[MSG_SIZE_IDX]);
    co_await asio::async_write(stream_, buffers, asio::use_awaitable);
  }

  next_layer_type& next_layer() {
    return stream_;
  }
  const next_layer_type& next_layer() const {
    return stream_;
  }

  void close() {
    // Prevent double-close using atomic flag
    bool expected = false;
    if (!is_closed_.compare_exchange_strong(expected, true)) {
      return; // Already closed
    }

    // Cancel all pending operations before closing the stream
    try {
      async_manager_.cancel_all_operations();
    } catch (...) {
      // Ignore errors during cancellation
    }

    // Close the underlying stream - use error_code version to avoid exceptions
    // boost::system::error_code ec;
    stream_.close();
    // Ignore any errors from close - we're shutting down anyway
  }

  void set_notification_callback(notification_callback_type callback) {
    notification_callback_ = std::move(callback);
  }

private:
  asio::awaitable<void> msg_reader() {
    try {
      for (;;) {
        header_type msg_header;

        co_await asio::async_read(stream_, asio::buffer(msg_header.data(), msg_header.size() * sizeof(header_type::value_type)), asio::use_awaitable);

        // spdlog::debug("Read {}: 0x{:X} {} 0x{:X} {} {}",
        //               __FUNCTION__,
        //               msg_header[MAGIC_HEADER_IDX],
        //               msg_type_to_string(to_msg_type(msg_header[MSG_TYPE_IDX])),
        //               msg_header[CALL_ID_IDX],
        //               msg_header[USER_TOKEN_IDX],
        //               msg_header[MSG_SIZE_IDX]);

        if (MAGIC_HEADER_NUMBER != msg_header[MAGIC_HEADER_IDX]) {
          // Log error asynchronously without blocking
          log_error_async("Invalid magic header: " + std::to_string(msg_header[MSG_TYPE_IDX] >> 32));
          co_await respond(msg_type::error, msg_header);
          continue;
        }

        switch (static_cast<msg_type>(msg_header[MSG_TYPE_IDX] >> 32)) {
          case msg_type::nop:
            co_await respond(msg_type::nop, msg_header);
            break;
          case msg_type::request:
            co_await dispatch_request(msg_header);
            break;
          case msg_type::response:
            co_await handle_response(msg_header);
            break;
          case msg_type::notification:
            co_await handle_notification(msg_header);
            break;
          case msg_type::error:
            co_await handle_error(msg_header);
            break;
        }
      }
    } catch (boost::system::system_error const& e) {
      if (e.code() == asio::error::eof || e.code() == asio::error::connection_reset || e.code() == asio::error::broken_pipe ||
          e.code() == asio::error::operation_aborted) {
        // Connection closed - this is expected during shutdown
        log_error_async("Connection closed: " + e.code().message());
      } else {
        // Unexpected error
        log_error_async("Unexpected error in msg_reader: " + e.code().message());
      }
    } catch (std::exception const& e) {
      // Catch any other standard exceptions
      log_error_async(std::string("Exception in msg_reader: ") + e.what());
    } catch (...) {
      // Catch everything else to prevent crashes
      log_error_async("Unknown exception in msg_reader");
    }

    // Clean up on exit - close() is now exception-safe
    close();
    co_return;
  }

  asio::awaitable<void> respond(msg_type msgtype, header_type const& related_msg, buffer_type const& rsp_buffer = buffer_type{}) {
    header_type rsp_header;
    rsp_header[MAGIC_HEADER_IDX] = MAGIC_HEADER_NUMBER;
    rsp_header[MSG_TYPE_IDX]     = static_cast<std::uint64_t>(msgtype) << 32;
    rsp_header[CALL_ID_IDX]      = related_msg[CALL_ID_IDX];
    rsp_header[USER_TOKEN_IDX]   = related_msg[USER_TOKEN_IDX];
    rsp_header[MSG_SIZE_IDX]     = rsp_buffer.size();

    std::array<boost::asio::const_buffer, 2> buffers = {asio::buffer(rsp_header.data(), rsp_header.size() * sizeof(header_type::value_type)),
                                                        asio::buffer(rsp_buffer)};

    co_await asio::async_write(stream_, buffers, asio::transfer_all(), asio::use_awaitable);

    // spdlog::debug("Write {}: 0x{:X} {} 0x{:X} {} {}",
    //               __FUNCTION__,
    //               rsp_header[MAGIC_HEADER_IDX],
    //               msg_type_to_string(msgtype),
    //               rsp_header[CALL_ID_IDX],
    //               rsp_header[USER_TOKEN_IDX],
    //               rsp_header[MSG_SIZE_IDX]);

    co_return;
  }

  asio::awaitable<void> dispatch_request(header_type const& req_header) {
    auto executor = co_await asio::this_coro::executor;
    auto req_size = req_header[MSG_SIZE_IDX];

    buffer_type req_buffer;
    req_buffer.resize(req_size);

    co_await asio::async_read(stream_, asio::buffer(req_buffer), asio::transfer_exactly(req_size), asio::use_awaitable);
    // spdlog::debug("Read payload for {}: 0x{:X} {} 0x{:X} {} {}",
    //               __FUNCTION__,
    //               req_header[MAGIC_HEADER_IDX],
    //               msg_type_to_string(to_msg_type(req_header[MSG_TYPE_IDX])),
    //               req_header[CALL_ID_IDX],
    //               req_header[USER_TOKEN_IDX],
    //               req_header[MSG_SIZE_IDX]);

    // Spawn request handling to avoid blocking the reader
    // IMPORTANT: Capture shared_from_this() to keep session alive while processing
    auto self = this->shared_from_this();
    asio::co_spawn(
        executor,
        [self, req_buffer = std::move(req_buffer), req_header]() -> asio::awaitable<void> {
          bool send_error = false;
          try {
            auto& call_id = req_header[CALL_ID_IDX];

            // Check if the function is async and dispatch accordingly
            if (self->dispatcher_->is_async(call_id)) {
              auto rsp_buffer = co_await self->dispatcher_->dispatch_async(call_id, req_buffer);
              co_await self->respond(msg_type::response, req_header, rsp_buffer);
            } else {
              auto rsp_buffer = self->dispatcher_->dispatch(call_id, req_buffer);
              co_await self->respond(msg_type::response, req_header, rsp_buffer);
            }

          } catch (std::exception const& e) {
            self->log_error_async(std::string("Request dispatch error: ") + e.what());
            send_error = true;
          } catch (...) {
            self->log_error_async("Unknown request dispatch error");
            send_error = true;
          }

          if (send_error) {
            try {
              co_await self->respond(msg_type::error, req_header);
            } catch (...) {
              // Ignore errors when sending error response (connection might be closed)
            }
          }
        },
        asio::detached);

    co_return;
  }

  asio::awaitable<void> handle_response(header_type const& resp_header) {
    auto        rsp_size = resp_header[MSG_SIZE_IDX];
    buffer_type rsp_buffer;
    rsp_buffer.resize(rsp_size);

    // spdlog::debug("Reading payload for {}: 0x{:X} {} 0x{:X} {} {}",
    //               __FUNCTION__,
    //               resp_header[MAGIC_HEADER_IDX],
    //               msg_type_to_string(to_msg_type(resp_header[MSG_TYPE_IDX])),
    //               resp_header[CALL_ID_IDX],
    //               resp_header[USER_TOKEN_IDX],
    //               resp_header[MSG_SIZE_IDX]);

    co_await asio::async_read(stream_, asio::buffer(rsp_buffer), asio::transfer_exactly(rsp_size), asio::use_awaitable);

    // spdlog::debug("Read payload for {}: 0x{:X} {} 0x{:X} {} {}",
    //               __FUNCTION__,
    //               resp_header[MAGIC_HEADER_IDX],
    //               msg_type_to_string(to_msg_type(resp_header[MSG_TYPE_IDX])),
    //               resp_header[CALL_ID_IDX],
    //               resp_header[USER_TOKEN_IDX],
    //               resp_header[MSG_SIZE_IDX]);
    auto token = resp_header[USER_TOKEN_IDX];
    // spdlog::debug("Completing {}: {} {}", __FUNCTION__, token, rsp_buffer.size());

    if (!co_await async_manager_.complete_operation(token, std::move(rsp_buffer))) {

      // spdlog::debug("Error completing {}: {} {}", __FUNCTION__, token, rsp_buffer.size());

      // Token not found - response arrived for unknown/cancelled request
    }

    co_return;
  }

  asio::awaitable<void> handle_notification(header_type const& msg_header) {
    auto call_id  = msg_header[CALL_ID_IDX];
    auto req_size = msg_header[MSG_SIZE_IDX];

    buffer_type buffer;
    buffer.resize(req_size);

    co_await asio::async_read(stream_, asio::buffer(buffer), asio::transfer_exactly(req_size), asio::use_awaitable);

    // Handle notification dispatch in a separate coroutine to avoid blocking
    // IMPORTANT: Capture shared_from_this() to keep session alive while processing
    auto executor = co_await asio::this_coro::executor;
    auto self     = this->shared_from_this();
    asio::co_spawn(
        executor,
        [self, call_id, buffer_data = std::move(buffer)]() mutable -> asio::awaitable<void> {
          try {
            // Use notification callback if set (for client-side), otherwise use dispatcher (for server-side)
            if (self->notification_callback_) {
              self->notification_callback_(call_id, buffer_data);
            } else {
              self->dispatcher_->dispatch(call_id, buffer_data);
            }
          } catch (std::exception const& e) {
            self->log_error_async(std::string("Notification handling error: ") + e.what());
          } catch (...) {
            self->log_error_async("Unknown notification handling error");
          }
          co_return;
        },
        asio::detached);

    co_return;
  }

  asio::awaitable<void> handle_error(header_type const& related_msg) {
    log_error_async("Received error message");
    co_return;
  }

  // Helper method for non-blocking error logging
  void log_error_async(const std::string& message) {
    // Capture logger by shared_ptr to avoid accessing 'this' after session destruction
    auto logger = logger_;
    if (logger) {
      asio::co_spawn(
          logger->strand_,
          [logger, message]() -> asio::awaitable<void> {
            co_await logger->error(message);
            co_return;
          },
          asio::detached);
    }
  }

private:
  std::string msg_type_to_string(msg_type type) const {

    switch (type) {
      case msg_type::nop:
        return "nop";
      case msg_type::request:
        return "request";
      case msg_type::response:
        return "response";
      case msg_type::notification:
        return "notification";
      case msg_type::error:
        return "error";
    }
    return "unknown";
  }

  msg_type to_msg_type(std::uint64_t type) const {
    return static_cast<msg_type>(type >> 32);
  }

private:
  AsyncStreamT                     stream_;
  std::shared_ptr<dispatcher_type> dispatcher_;
  std::shared_ptr<async_logger>    logger_;
  asio::strand<executor_type>      strand_;
  async_manager_type               async_manager_;
  notification_callback_type       notification_callback_;
  std::atomic<bool>                is_closed_{false};
};

} // namespace grlx::rpc