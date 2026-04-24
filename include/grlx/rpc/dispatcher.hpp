#pragma once

#include "message.hpp"
#include "security.hpp"
#include "string_hash.hpp"

#include <boost/asio/awaitable.hpp>

#include <istream>
#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace grlx::rpc {

// Thrown by the dispatcher when the auth callback denies a call. Session
// translates this into a msg_type::error frame without revealing the
// specific denial reason to the peer (logged server-side only).
class dispatch_denied_error : public std::runtime_error {
public:
  explicit dispatch_denied_error(std::string const& reason)
    : std::runtime_error("dispatch denied: " + reason) {}
};

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

  struct method_entry {
    std::string                name;
    visibility                 vis;
    std::unique_ptr<call_base> handler;
  };

public:
  dispatcher()                        = default;
  dispatcher(dispatcher const& other) = delete;

  dispatcher(dispatcher&& other)
    : dispatch_table(std::move(other.dispatch_table))
    , auth_callback_(std::move(other.auth_callback_)) {
  }

  // Backwards-compatible overload — no client_context means no auth gate is
  // applied. Used by unit tests that exercise the dispatcher directly and by
  // any caller that hasn't installed a context yet.
  buffer_type dispatch(table_key_type func_index, buffer_type const& req_buffer) {
    auto& entry = find_entry_or_throw(func_index);
    return entry.handler->dispatch(req_buffer);
  }

  buffer_type dispatch(table_key_type func_index, buffer_type const& req_buffer, client_context const& ctx) {
    auto& entry = find_entry_or_throw(func_index);
    enforce_auth(entry, ctx);
    return entry.handler->dispatch(req_buffer);
  }

  asio::awaitable<buffer_type> dispatch_async(table_key_type func_index, buffer_type const& req_buffer) {
    auto& entry = find_entry_or_throw(func_index);
    co_return co_await entry.handler->dispatch_async(req_buffer);
  }

  asio::awaitable<buffer_type> dispatch_async(table_key_type func_index, buffer_type const& req_buffer, client_context const& ctx) {
    auto& entry = find_entry_or_throw(func_index);
    enforce_auth(entry, ctx);
    co_return co_await entry.handler->dispatch_async(req_buffer);
  }

  bool is_async(table_key_type func_index) const {
    auto itr = dispatch_table.find(func_index);
    if (itr != dispatch_table.end()) {
      return itr->second.handler->is_async();
    }
    return false;
  }

  // Register a handler. Defaults to visibility::authenticated — the safe
  // choice because it means a freshly attached method is auth-gated unless
  // the caller explicitly opts out.
  template <typename SignatureT>
  void attach(std::string const& func_name, std::function<SignatureT>&& func) {
    attach(func_name, visibility::authenticated, std::move(func));
  }

  template <typename SignatureT>
  void attach(std::string const& func_name, visibility vis, std::function<SignatureT>&& func) {
    using call_t  = call<SignatureT>;
    auto callable = std::make_unique<call_t>(this, std::forward<std::function<SignatureT>>(func));
    attach_impl(func_name, vis, std::move(callable));
  }

  void set_auth_callback(auth_callback cb) {
    auth_callback_ = std::move(cb);
  }

  bool has_auth_callback() const noexcept {
    return static_cast<bool>(auth_callback_);
  }

private:
  method_entry& find_entry_or_throw(table_key_type func_index) {
    auto itr = dispatch_table.find(func_index);
    if (itr == dispatch_table.end()) {
      throw std::runtime_error("Handler not found: " + std::to_string(func_index)
                               + " table has " + std::to_string(dispatch_table.size()) + " entries");
    }
    return itr->second;
  }

  void enforce_auth(method_entry const& entry, client_context const& ctx) {
    if (entry.vis == visibility::public_) {
      return;  // public endpoints run without consulting the callback
    }
    if (!auth_callback_) {
      // No callback installed: treat every visibility as public. This keeps
      // apps that haven't opted into gating working. Once an app installs a
      // callback, authenticated/admin methods start consulting it.
      return;
    }
    auto result = auth_callback_(ctx, entry.name);
    if (!result.allow) {
      throw dispatch_denied_error(result.deny_reason.empty() ? "denied" : result.deny_reason);
    }
    if (entry.vis == visibility::admin && !result.is_admin) {
      throw dispatch_denied_error("admin required for '" + entry.name + "'");
    }
  }

  void attach_impl(std::string const& func_name, visibility vis, std::unique_ptr<call_base>&& handler) {
    dispatch_table.insert_or_assign(
        shash64(func_name).value(),
        method_entry{.name = func_name, .vis = vis, .handler = std::move(handler)});
  }

  std::unordered_map<table_key_type, method_entry> dispatch_table;
  auth_callback                                    auth_callback_;
};
} // namespace grlx::rpc