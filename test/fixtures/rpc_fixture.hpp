#pragma once

// Shared GTest base and helpers for grlx-rpc tests.
//
// Each fixture owns a single asio::io_context + work_guard. Tests drive the
// context by posting a coroutine that runs the assertions, then stops the
// context. All tests use ephemeral ports (0) so they can run in parallel.

#include "test_certs.hpp"

#include <grlx/rpc/client.hpp>
#include <grlx/rpc/server.hpp>
#include <grlx/rpc/ssl_channel.hpp>
#include <grlx/rpc/tcp_channel.hpp>
#include <grlx/rpc/encoder.hpp>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <string>

namespace grlx::rpc::testing {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

using tcp_ch = grlx::rpc::tcp_channel<grlx::rpc::binary_encoder>;
using ssl_ch = grlx::rpc::ssl_channel<grlx::rpc::binary_encoder>;

// Build an ssl_context pre-loaded with test material.
//   side == server_base → server cert + key, trusts primary CA for peer verification
//   side == client_base → primary CA in trust store, peer verification on by default
inline asio::ssl::context make_server_ctx(pem_bundle const& server_bundle,
                                          pem_bundle const& ca_bundle) {
  asio::ssl::context ctx(asio::ssl::context::tls_server);
  ctx.use_certificate_chain(asio::buffer(server_bundle.cert_pem));
  ctx.use_private_key(asio::buffer(server_bundle.key_pem), asio::ssl::context::pem);
  ctx.add_certificate_authority(asio::buffer(ca_bundle.cert_pem));
  return ctx;
}

inline asio::ssl::context make_client_ctx(pem_bundle const& ca_bundle) {
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  ctx.add_certificate_authority(asio::buffer(ca_bundle.cert_pem));
  ctx.set_verify_mode(asio::ssl::verify_peer);
  return ctx;
}

// A relaxed client context that trusts no CA and does not verify — used by
// baseline tests that just need a TLS handshake to succeed. Phase 1 tests
// will use stricter contexts.
inline asio::ssl::context make_insecure_client_ctx() {
  asio::ssl::context ctx(asio::ssl::context::tls_client);
  ctx.set_verify_mode(asio::ssl::verify_none);
  return ctx;
}

// Base fixture: io_context + work_guard. Tests spawn coroutines that drive
// the body of the test and stop the context when done.
class rpc_fixture : public ::testing::Test {
public:
  void SetUp() override {
    work_guard.emplace(io_context.get_executor());
  }

  void TearDown() override {
    work_guard.reset();
    io_context.stop();
  }

  // Run a coroutine body to completion on the fixture's io_context.
  // The coroutine must stop the context (call io_context.stop()) when its
  // assertions finish, or fail the test via FAIL()/ASSERT_*.
  template <typename BodyT>
  void run(BodyT body) {
    asio::co_spawn(
        io_context,
        [this, body = std::move(body)]() mutable -> asio::awaitable<void> {
          try {
            co_await body();
          } catch (std::exception const& e) {
            ADD_FAILURE() << "coroutine threw: " << e.what();
          } catch (...) {
            ADD_FAILURE() << "coroutine threw unknown exception";
          }
          io_context.stop();
          co_return;
        },
        asio::detached);
    io_context.run();
  }

  // Helper: spin up a TCP server on an ephemeral port with the provided setup
  // lambda that registers RPC methods. Returns the address "127.0.0.1:<port>".
  template <typename SetupT>
  auto start_tcp_server(grlx::rpc::server<tcp_ch>& server, SetupT&& setup)
      -> asio::awaitable<std::string> {
    setup(server);
    co_await server.start(tcp::endpoint(tcp::v4(), 0));
    co_return std::format("127.0.0.1:{}", server.channel().endpoint().port());
  }

  template <typename SetupT>
  auto start_ssl_server(grlx::rpc::server<ssl_ch>& server, SetupT&& setup)
      -> asio::awaitable<tcp::endpoint> {
    setup(server);
    co_await server.start(tcp::endpoint(tcp::v4(), 0));
    co_return server.channel().endpoint();
  }

  asio::io_context                                                          io_context;
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_guard;
};

}  // namespace grlx::rpc::testing
