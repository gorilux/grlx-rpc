
#include <grlx/rpc/client.hpp>
#include <grlx/rpc/encoder.hpp>
#include <grlx/rpc/server.hpp>
#include <grlx/rpc/tcp_channel.hpp>

#include <boost/asio.hpp>

#include <format>
#include <gtest/gtest.h>
#include <string>
#include <tuple>

namespace asio = boost::asio;
using boost::asio::ip::tcp;

class grlx_rpc_test : public ::testing::Test {
public:
  void SetUp() override {
    work_guard = asio::make_work_guard(io_context);
  }

  void TearDown() override {
    work_guard.reset();
    io_context.stop();
  }

  std::tuple<int, double, std::string> my_func(int first, double second, std::string const& third) {
    return std::make_tuple(first, second, third);
  }

  asio::awaitable<int> async_func() {
    co_return 42;
  }

  asio::io_context                                           io_context;
  asio::executor_work_guard<asio::io_context::executor_type> work_guard{io_context.get_executor()};
};

TEST_F(grlx_rpc_test, test_tcp_channel) {

  using channel_type  = grlx::rpc::tcp_channel<grlx::rpc::binary_encoder>;
  using rpc_client    = grlx::rpc::client<channel_type>;
  using rpc_server    = grlx::rpc::server<channel_type>;
  grlx_rpc_test* self = this;

  asio::co_spawn(
      io_context,
      [&, this]() -> asio::awaitable<void> {
        auto       exec = co_await asio::this_coro::executor;
        rpc_server server;
        server.attach("my_func", self, &grlx_rpc_test::my_func);
        co_await server.start(tcp::endpoint(boost::asio::ip::tcp::v4(), 0));

        rpc_client  client;
        std::string address = std::format("127.0.0.1:{}", server.channel->endpoint().port());
        co_await client.connect(address);

        auto response = co_await client.invoke<std::tuple<int, double, std::string>>("my_func", 10, 20.0, std::string("300.0"));

        EXPECT_EQ(std::get<0>(response), 10);
        EXPECT_EQ(std::get<1>(response), 20.0);
        EXPECT_EQ(std::get<2>(response), "300.0");
        io_context.stop();
        co_return;
      },
      asio::detached);
  io_context.run();
}

TEST_F(grlx_rpc_test, test_coroutine_support) {

  using channel_type  = grlx::rpc::tcp_channel<grlx::rpc::binary_encoder>;
  using rpc_client    = grlx::rpc::client<channel_type>;
  using rpc_server    = grlx::rpc::server<channel_type>;
  grlx_rpc_test* self = this;

  asio::co_spawn(
      io_context,
      [&, this]() -> asio::awaitable<void> {
        auto       exec = co_await asio::this_coro::executor;
        rpc_server server;

        // Test member function coroutine
        server.attach("async_func", self, &grlx_rpc_test::async_func);

        // Test lambda coroutine
        server.attach("MyFunc", []() -> asio::awaitable<int> {
          co_return 1;
        });

        co_await server.start(tcp::endpoint(boost::asio::ip::tcp::v4(), 0));

        rpc_client  client;
        std::string address = std::format("127.0.0.1:{}", server.channel->endpoint().port());
        co_await client.connect(address);

        // Test member function coroutine
        auto response1 = co_await client.invoke<int>("async_func");
        EXPECT_EQ(response1, 42);

        // Test lambda coroutine
        auto response2 = co_await client.invoke<int>("MyFunc");
        EXPECT_EQ(response2, 1);

        io_context.stop();
        co_return;
      },
      asio::detached);
  io_context.run();
}
