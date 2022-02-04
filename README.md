# grlx-rpc

A modern, header-only C++ RPC library built on Boost.Asio with coroutine support.

## Features

- **Header-only**: No compilation required, just include and use
- **Async/Await**: Built on C++20 coroutines with Boost.Asio
- **Type-safe**: Compile-time type checking for RPC calls
- **Flexible Transport**: TCP channel included, extensible for other transports
- **Zero Dependencies**: Only requires Boost.Asio and spdlog
- **Cross-platform**: Works on Linux, Windows, macOS, and even Emscripten/WebAssembly

## Quick Start

### Client Example

```cpp
#include <grlx/rpc/client.hpp>
#include <grlx/rpc/tcp_channel.hpp>
#include <grlx/rpc/encoder.hpp>

using namespace grlx::rpc;

asio::awaitable<void> example_client() {
  // Create client with TCP channel
  client<tcp_channel<binary_encoder>> rpc_client;

  // Connect to server
  co_await rpc_client.connect("localhost:8080");

  // Make RPC call
  auto result = co_await rpc_client.invoke<std::string>("hello", "World");
  std::cout << "Result: " << result << std::endl;
}
```

### Server Example

```cpp
#include <grlx/rpc/server.hpp>
#include <grlx/rpc/tcp_channel.hpp>
#include <grlx/rpc/encoder.hpp>

using namespace grlx::rpc;

asio::awaitable<void> example_server() {
  // Create server with TCP channel
  server<tcp_channel<binary_encoder>> rpc_server;

  // Register RPC handler
  rpc_server.attach("hello", [](std::string name) -> std::string {
    return "Hello, " + name + "!";
  });

  // Start listening
  tcp::endpoint endpoint(tcp::v4(), 8080);
  co_await rpc_server.start(endpoint);
}
```

## Components

- **`client.hpp`**: RPC client with async invoke
- **`server.hpp`**: RPC server with dispatcher
- **`tcp_channel.hpp`**: TCP transport implementation
- **`session.hpp`**: Session management
- **`dispatcher.hpp`**: Function call routing
- **`encoder.hpp`**: Message encoding/decoding
- **`buffer_pool.hpp`**: Efficient buffer management

## Requirements

- C++20 compiler (GCC 10+, Clang 12+, MSVC 2019+)
- Boost 1.75+ (Asio)
- spdlog (for logging)

## Building with Meson

```bash
# As a subproject
meson subprojects download grlx-rpc

# In your meson.build
grlx_rpc_dep = dependency('grlx-rpc')
```

## License

MIT License - See LICENSE file for details
