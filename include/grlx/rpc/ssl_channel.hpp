#pragma once

#include "session.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

#include <string>
#include <string_view>

namespace grlx::rpc {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

template <typename EncoderT>
class ssl_channel {
public:
  using encoder_type  = EncoderT;
  using buffer_type   = typename EncoderT::buffer_type;
  using ssl_socket    = asio::ssl::stream<tcp::socket>;
  using session_type  = session<ssl_socket, EncoderT>;
  using executor_type = tcp::acceptor::executor_type;

  ssl_channel(asio::ssl::context ctx)
    : ssl_ctx_(std::move(ctx)) {
  }

  ssl_channel(ssl_channel&&)                 = default;
  ssl_channel(ssl_channel const&)            = delete;
  ssl_channel& operator=(ssl_channel const&) = delete;

  auto get_executor() -> executor_type {
    if (acceptor_) {
      return acceptor_->get_executor();
    }
    throw std::runtime_error("ssl_channel: get_executor() called before bind()");
  }

  auto bind(tcp::endpoint const& endpoint) -> asio::awaitable<void> {
    auto executor = co_await asio::this_coro::executor;
    acceptor_     = std::make_unique<tcp::acceptor>(executor, endpoint);
    acceptor_->set_option(asio::socket_base::reuse_address(true));
    acceptor_->listen(asio::socket_base::max_listen_connections);
    co_return;
  }

  auto close() -> asio::awaitable<void> {
    if (acceptor_) {
      acceptor_->cancel();
      acceptor_->close();
    }
    co_return;
  }

  auto endpoint() -> tcp::endpoint {
    if (acceptor_) {
      return acceptor_->local_endpoint();
    }
    return tcp::endpoint();
  }

  template <typename... ArgsT>
  auto accept(ArgsT&&... args) -> asio::awaitable<std::shared_ptr<session_type>> {
    auto tcp_sock = co_await acceptor_->async_accept(asio::use_awaitable);
    ssl_socket ssl_sock(std::move(tcp_sock), ssl_ctx_);
    co_await ssl_sock.async_handshake(asio::ssl::stream_base::server, asio::use_awaitable);
    co_return std::make_shared<session_type>(std::move(ssl_sock), std::forward<ArgsT>(args)...);
  }

  auto connect(std::string const& address) -> asio::awaitable<std::shared_ptr<session_type>> {
    auto          executor = co_await asio::this_coro::executor;
    tcp::resolver resolver(executor);

    auto colon_pos = address.rfind(':');
    auto host      = colon_pos != std::string::npos ? address.substr(0, colon_pos) : address;
    auto port      = colon_pos != std::string::npos ? address.substr(colon_pos + 1) : "0";

    auto endpoints = co_await resolver.async_resolve(host, port, asio::use_awaitable);
    co_return co_await connect(*std::begin(endpoints));
  }

  auto connect(tcp::endpoint const& endpoint) -> asio::awaitable<std::shared_ptr<session_type>> {
    auto       executor = co_await asio::this_coro::executor;
    ssl_socket ssl_sock(executor, ssl_ctx_);
    co_await ssl_sock.next_layer().async_connect(endpoint, asio::use_awaitable);
    co_await ssl_sock.async_handshake(asio::ssl::stream_base::client, asio::use_awaitable);
    co_return std::make_shared<session_type>(std::move(ssl_sock));
  }

private:
  asio::ssl::context             ssl_ctx_;
  std::unique_ptr<tcp::acceptor> acceptor_;
};

} // namespace grlx::rpc
