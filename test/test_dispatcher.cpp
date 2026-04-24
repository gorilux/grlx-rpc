// Direct dispatcher tests — no sockets, pure attach+dispatch over encoded buffers.

#include <grlx/rpc/dispatcher.hpp>
#include <grlx/rpc/encoder.hpp>
#include <grlx/rpc/message.hpp>
#include <grlx/rpc/string_hash.hpp>

#include <gtest/gtest.h>

#include <string>

using encoder_t    = grlx::rpc::binary_encoder;
using buffer_t     = encoder_t::buffer_type;
using dispatcher_t = grlx::rpc::dispatcher<encoder_t>;

TEST(rpc_dispatcher_test, attach_then_dispatch_invokes_handler) {
  dispatcher_t disp;
  disp.attach("triple", std::function<int(int)>{[](int x) { return x * 3; }});

  grlx::rpc::message_request<int> req{std::make_tuple(7)};
  buffer_t                        req_buf;
  ASSERT_TRUE(encoder_t::encode(req_buf, req));

  auto rsp_buf = disp.dispatch(grlx::rpc::shash64("triple").value(), req_buf);

  grlx::rpc::message_response<int> rsp;
  ASSERT_TRUE(encoder_t::decode(rsp_buf, rsp));
  EXPECT_EQ(rsp.result, 21);
}

TEST(rpc_dispatcher_test, unknown_method_throws) {
  dispatcher_t disp;
  buffer_t     empty;
  EXPECT_THROW(disp.dispatch(grlx::rpc::shash64("nope").value(), empty), std::runtime_error);
}

TEST(rpc_dispatcher_test, handler_is_async_for_coroutine_attach) {
  dispatcher_t disp;
  disp.attach("co_answer", std::function<boost::asio::awaitable<int>()>{
                                []() -> boost::asio::awaitable<int> { co_return 99; }});

  EXPECT_TRUE(disp.is_async(grlx::rpc::shash64("co_answer").value()));
}
