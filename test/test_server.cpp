// Server-level regression tests: multi-client concurrency, notifications,
// and graceful shutdown.

#include "fixtures/rpc_fixture.hpp"

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace asio = boost::asio;
using grlx::rpc::testing::rpc_fixture;
using grlx::rpc::testing::tcp_ch;

class rpc_server_test : public rpc_fixture {};

TEST_F(rpc_server_test, notification_reaches_connected_client) {
  run([this]() -> asio::awaitable<void> {
    grlx::rpc::server<tcp_ch> server;
    auto address = co_await start_tcp_server(server, [](auto&) {});

    grlx::rpc::client<tcp_ch> client;
    co_await client.connect(address);

    auto received = std::make_shared<std::string>();
    client.register_notification_handler("tick", [received](std::string const& payload) {
      *received = payload;
    });

    // Give the client's notification-handling coroutine time to register with
    // its session before the server sends.
    asio::steady_timer warmup(co_await asio::this_coro::executor);
    warmup.expires_after(std::chrono::milliseconds(50));
    co_await warmup.async_wait(asio::use_awaitable);

    co_await server.notify("tick", std::string("hello"));

    for (int attempt = 0; attempt < 100; ++attempt) {
      asio::steady_timer t(co_await asio::this_coro::executor);
      t.expires_after(std::chrono::milliseconds(10));
      co_await t.async_wait(asio::use_awaitable);
      if (!received->empty()) break;
    }
    EXPECT_EQ(*received, "hello");
  });
}

TEST_F(rpc_server_test, multi_client_concurrent_calls) {
  run([this]() -> asio::awaitable<void> {
    grlx::rpc::server<tcp_ch> server;
    auto address = co_await start_tcp_server(server, [](auto& s) {
      s.attach("double", [](int x) -> int { return x * 2; });
    });

    constexpr int kClients      = 5;
    constexpr int kCallsPerClient = 10;

    std::vector<std::shared_ptr<grlx::rpc::client<tcp_ch>>> clients;
    for (int i = 0; i < kClients; ++i) {
      auto c = std::make_shared<grlx::rpc::client<tcp_ch>>();
      co_await c->connect(address);
      clients.push_back(c);
    }

    auto executor = co_await asio::this_coro::executor;

    // Fire off every call in parallel, collect them sequentially.
    using namespace asio::experimental::awaitable_operators;

    std::atomic<int> successes{0};
    std::atomic<int> failures{0};

    for (int ci = 0; ci < kClients; ++ci) {
      for (int k = 0; k < kCallsPerClient; ++k) {
        int expected = (ci * 100 + k) * 2;
        int input    = ci * 100 + k;
        asio::co_spawn(
            executor,
            [c = clients[ci], input, expected, &successes, &failures]() -> asio::awaitable<void> {
              try {
                int got = co_await c->invoke<int>("double", input);
                if (got == expected) ++successes;
                else ++failures;
              } catch (...) {
                ++failures;
              }
              co_return;
            },
            asio::detached);
      }
    }

    // Wait for completions.
    for (int attempt = 0; attempt < 500; ++attempt) {
      asio::steady_timer t(executor);
      t.expires_after(std::chrono::milliseconds(10));
      co_await t.async_wait(asio::use_awaitable);
      if (successes + failures >= kClients * kCallsPerClient) break;
    }

    EXPECT_EQ(successes.load(), kClients * kCallsPerClient);
    EXPECT_EQ(failures.load(), 0);
  });
}

TEST_F(rpc_server_test, server_stop_closes_all_sessions) {
  run([this]() -> asio::awaitable<void> {
    grlx::rpc::server<tcp_ch> server;
    auto address = co_await start_tcp_server(server, [](auto& s) {
      s.attach("ping", []() -> int { return 0; });
    });

    grlx::rpc::client<tcp_ch> client;
    co_await client.connect(address);
    EXPECT_EQ(co_await client.invoke<int>("ping"), 0);

    // Wait until the server has recorded the session.
    for (int attempt = 0; attempt < 50; ++attempt) {
      asio::steady_timer t(co_await asio::this_coro::executor);
      t.expires_after(std::chrono::milliseconds(10));
      co_await t.async_wait(asio::use_awaitable);
      if (co_await server.session_count() >= 1u) break;
    }
    // Can't use ASSERT_EQ inside a coroutine (macro expands to `return;`).
    auto count = co_await server.session_count();
    EXPECT_EQ(count, 1u);
    if (count != 1u) {
      co_return;  // bail out cleanly
    }

    co_await server.stop();
    EXPECT_EQ(co_await server.session_count(), 0u);
  });
}
