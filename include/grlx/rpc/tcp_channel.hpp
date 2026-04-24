#pragma once

#include "session.hpp"

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace grlx::rpc {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

// Thrown when the pre-session filter rejects a freshly accepted TCP peer.
// The server's accept loop catches and continues.
class tcp_connection_rejected_error : public std::runtime_error {
public:
  explicit tcp_connection_rejected_error(std::string reason)
    : std::runtime_error("connection rejected: " + reason) {}
};

// Predicate that inspects a freshly accepted peer endpoint and decides
// whether to proceed. Return false to reject the connection.
using tcp_pre_session_filter = std::function<bool(tcp::endpoint const& peer,
                                                  std::string&        reject_reason)>;

namespace detail {
// Simple host:port parser to avoid URL library dependency
inline std::pair<std::string, std::string> parse_address(std::string_view address) {
  // Find the last colon (to handle IPv6 addresses like [::1]:8080)
  auto colon_pos = address.rfind(':');

  if (colon_pos == std::string_view::npos) {
    // No port specified, assume default port
    return {std::string(address), "0"};
  }

  // Check if this might be an IPv6 address without brackets
  if (address.find(':') != colon_pos) {
    // Multiple colons - likely IPv6, need brackets or assume no port
    return {std::string(address), "0"};
  }

  std::string host = std::string(address.substr(0, colon_pos));
  std::string port = std::string(address.substr(colon_pos + 1));

  return {host, port};
}
} // namespace detail

template <typename EncoderT>
class tcp_channel {
public:
  using encoder_type  = EncoderT;
  using buffer_type   = typename EncoderT::buffer_type;
  using session_type  = session<tcp::socket, EncoderT>;
  using executor_type = tcp::acceptor::executor_type;

  tcp_channel() = default;
  tcp_channel(tcp_channel&& other)
    : acceptor_(std::move(other.acceptor_)) {
  }

  tcp_channel(tcp_channel const&)           = delete;
  tcp_channel operator=(tcp_channel const&) = delete;

  auto get_executor() -> executor_type {
    if (acceptor_) {
      return acceptor_->get_executor();
    }
    // Return a default-constructed executor if acceptor is not available
    // This should only happen before bind() is called
    throw std::runtime_error("tcp_channel: get_executor() called before bind()");
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
    if (acceptor_ != nullptr) {
      return acceptor_->local_endpoint();
    }
    return tcp::endpoint();
  }

  void set_pre_handshake_filter(tcp_pre_session_filter f) {
    pre_filter_ = std::move(f);
  }

  template <typename... ArgsT>
  auto accept(ArgsT&&... args) -> asio::awaitable<std::shared_ptr<session_type>> {
    auto tcp_socket = co_await acceptor_->async_accept(asio::use_awaitable);

    boost::system::error_code ep_ec;
    auto                      peer = tcp_socket.remote_endpoint(ep_ec);
    if (pre_filter_ && !ep_ec) {
      std::string reason;
      if (!pre_filter_(peer, reason)) {
        boost::system::error_code ignored;
        tcp_socket.close(ignored);
        throw tcp_connection_rejected_error(reason.empty() ? "filter denied" : reason);
      }
    }

    auto sess = std::make_shared<session_type>(std::move(tcp_socket), std::forward<ArgsT>(args)...);
    if (!ep_ec) {
      sess->set_peer_ip(peer.address().to_string());
      sess->set_peer_address(peer.address().to_string() + ":" + std::to_string(peer.port()));
    }
    co_return sess;
  }

  auto connect(std::string const& address) -> asio::awaitable<std::shared_ptr<session_type>> {
    auto          executor = co_await asio::this_coro::executor;
    tcp::resolver resolver(executor);

    auto [host, port] = detail::parse_address(address);
    auto endpoints    = co_await resolver.async_resolve(host, port, asio::use_awaitable);
    auto endpoint     = std::begin(endpoints);

    co_return co_await connect(*endpoint);
  }

  auto connect(tcp::endpoint const& endpoint) -> asio::awaitable<std::shared_ptr<session_type>> {
    auto        executor = co_await asio::this_coro::executor;
    tcp::socket tcp_socket(executor);
    co_await tcp_socket.async_connect(endpoint, asio::use_awaitable);
    co_return std::make_shared<session_type>(std::move(tcp_socket));
  }

private:
  std::unique_ptr<tcp::acceptor> acceptor_;
  tcp_pre_session_filter         pre_filter_;
};

} // namespace grlx::rpc