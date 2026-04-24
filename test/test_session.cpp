// Session-level regression tests.
//
// These verify message framing behaviors surfaced through the server+client
// façade: unknown methods must return an error *and* keep the session alive
// for further calls, and clients that disconnect mid-session must be cleaned
// out of active_sessions_.

#include "fixtures/rpc_fixture.hpp"

#include <boost/asio/steady_timer.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace asio = boost::asio;
using grlx::rpc::testing::rpc_fixture;
using grlx::rpc::testing::tcp_ch;

class rpc_session_test : public rpc_fixture {};

TEST_F(rpc_session_test, unknown_method_is_surfaced_as_error_and_session_survives) {
  run([this]() -> asio::awaitable<void> {
    grlx::rpc::server<tcp_ch> server;
    auto address = co_await start_tcp_server(server, [](auto& s) {
      s.attach("ping", []() -> int { return 7; });
    });

    grlx::rpc::client<tcp_ch> client;
    co_await client.connect(address);

    // First a known call — baseline that the session works.
    EXPECT_EQ(co_await client.invoke<int>("ping"), 7);

    // Now call a non-attached method. The server dispatch path throws, which
    // propagates to the client as an error — but the transport must stay open.
    bool threw = false;
    try {
      co_await client.invoke<int>("no_such_method");
    } catch (std::exception const&) {
      threw = true;
    }
    EXPECT_TRUE(threw) << "unknown RPC method should raise on the client";

    // Session must still be usable.
    EXPECT_EQ(co_await client.invoke<int>("ping"), 7);
  });
}

TEST_F(rpc_session_test, client_disconnect_removes_session_from_server) {
  run([this]() -> asio::awaitable<void> {
    grlx::rpc::server<tcp_ch> server;
    auto address = co_await start_tcp_server(server, [](auto& s) {
      s.attach("ping", []() -> int { return 1; });
    });

    {
      grlx::rpc::client<tcp_ch> client;
      co_await client.connect(address);
      EXPECT_EQ(co_await client.invoke<int>("ping"), 1);

      // Give the server a moment to register the session in its set.
      asio::steady_timer t(co_await asio::this_coro::executor);
      t.expires_after(std::chrono::milliseconds(50));
      co_await t.async_wait(asio::use_awaitable);

      EXPECT_EQ(co_await server.session_count(), 1u);
    }  // client goes out of scope → socket closes

    // Server should notice the disconnect and clean up.
    for (int attempt = 0; attempt < 50; ++attempt) {
      asio::steady_timer t(co_await asio::this_coro::executor);
      t.expires_after(std::chrono::milliseconds(20));
      co_await t.async_wait(asio::use_awaitable);
      if (co_await server.session_count() == 0u) break;
    }
    EXPECT_EQ(co_await server.session_count(), 0u);
  });
}
