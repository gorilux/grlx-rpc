#pragma once

#include "message.hpp"

#include "string_hash.hpp"

#include <boost/asio/awaitable.hpp>

#include <istream>
#include <ostream>
#include <type_traits>
#include <unordered_map>

namespace grlx::rpc {

namespace asio = boost::asio;

template <typename EncoderT>
class dispatcher {

  using encoder_type   = EncoderT;
  using table_key_type = std::uint64_t;
  using buffer_type    = typename EncoderT::buffer_type;

  struct call_base {
    call_base(dispatcher* self)
      : self_(self) {
    }
    virtual ~call_base()                                                           = default;
    virtual buffer_type                  dispatch(buffer_type const& buffer)       = 0;
    virtual asio::awaitable<buffer_type> dispatch_async(buffer_type const& buffer) = 0;
    virtual bool                         is_async() const                          = 0;

    dispatcher* self_;
  };

  template <typename SignatureT>
  struct call : call_base {

    using call_type = std::function<SignatureT>;

    call_type the_call;

    call(dispatcher* self, call_type&& call)
      : call_base(self)
      , the_call(std::forward<call_type>(call)) {
    }

    buffer_type dispatch(buffer_type const& req_buffer) override {
      return dispatch_sync(req_buffer, the_call);
    }

    asio::awaitable<buffer_type> dispatch_async(buffer_type const& req_buffer) override {
      // For synchronous functions, we just wrap the result in a coroutine
      co_return dispatch_sync(req_buffer, the_call);
    }

    bool is_async() const override {
      return false;
    }

    template <typename ReturnT, typename... ArgsT>
    static buffer_type dispatch_sync(buffer_type const& req_buffer, std::function<ReturnT(ArgsT...)>& func) {
      message_request<typename std::decay<ArgsT>::type...> request;
      message_response<typename std::decay<ReturnT>::type> response;

      encoder_type::decode(req_buffer, request);

      if constexpr (std::is_void<ReturnT>::value) {
        std::apply(func, request.args);
      } else {
        response.result = std::apply(func, request.args);
      }

      buffer_type rsp_buffer;
      encoder_type::encode(rsp_buffer, response);
      return rsp_buffer;
    }
  };

  // Specialization for asynchronous functions returning asio::awaitable<T>
  template <typename T, typename... ArgsT>
  struct call<asio::awaitable<T>(ArgsT...)> : call_base {

    using call_type = std::function<asio::awaitable<T>(ArgsT...)>;

    call_type the_call;

    call(dispatcher* self, call_type&& call)
      : call_base(self)
      , the_call(std::forward<call_type>(call)) {
    }

    buffer_type dispatch(buffer_type const& req_buffer) override {
      // This should not be called for async functions
      throw std::runtime_error("Synchronous dispatch called on async function");
    }

    asio::awaitable<buffer_type> dispatch_async(buffer_type const& req_buffer) override {
      co_return co_await dispatch_async_impl(req_buffer, the_call);
    }

    bool is_async() const override {
      return true;
    }

    template <typename ReturnT, typename... Args>
    static asio::awaitable<buffer_type> dispatch_async_impl(buffer_type const& req_buffer, std::function<asio::awaitable<ReturnT>(Args...)>& func) {
      message_request<typename std::decay<Args>::type...>  request;
      message_response<typename std::decay<ReturnT>::type> response;

      encoder_type::decode(req_buffer, request);

      if constexpr (std::is_void<ReturnT>::value) {
        co_await std::apply(func, request.args);
      } else {
        response.result = co_await std::apply(func, request.args);
      }

      buffer_type rsp_buffer;
      encoder_type::encode(rsp_buffer, response);
      co_return rsp_buffer;
    }
  };

public:
  dispatcher()                        = default;
  dispatcher(dispatcher const& other) = delete;

  dispatcher(dispatcher&& other)
    : dispatch_table(std::move(other.dispatch_table)) {
  }

  buffer_type dispatch(table_key_type func_index, buffer_type const& req_buffer) {
    auto itr = dispatch_table.find(func_index);
    if (itr != dispatch_table.end()) {
      auto& handler = itr->second;
      return handler->dispatch(req_buffer);
    }
    throw std::runtime_error("Handler not found: " + std::to_string(func_index) + " table has " + std::to_string(dispatch_table.size()) + " entries");
  }

  asio::awaitable<buffer_type> dispatch_async(table_key_type func_index, buffer_type const& req_buffer) {
    auto itr = dispatch_table.find(func_index);
    if (itr != dispatch_table.end()) {
      auto& handler = itr->second;
      co_return co_await handler->dispatch_async(req_buffer);
    }
    throw std::runtime_error("Handler not found: " + std::to_string(func_index) + " table has " + std::to_string(dispatch_table.size()) + " entries");
  }

  bool is_async(table_key_type func_index) const {
    auto itr = dispatch_table.find(func_index);
    if (itr != dispatch_table.end()) {
      return itr->second->is_async();
    }
    return false;
  }

  template <typename SignatureT>
  void attach(std::string const& func_name, std::function<SignatureT>&& func) {

    using call_t = call<SignatureT>;

    auto callable = std::make_unique<call_t>(this, std::forward<std::function<SignatureT>>(func));

    attach_impl(func_name, std::move(callable));
  }

private:
  void attach_impl(std::string const& func_name, std::unique_ptr<call_base>&& handler) {

    dispatch_table.emplace(shash64(func_name).value(), std::move(handler));
  }

private:
  std::unordered_map<table_key_type, std::unique_ptr<call_base>> dispatch_table;
};
} // namespace grlx::rpc