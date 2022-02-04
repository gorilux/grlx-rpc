#pragma once

#include "buffer_pool.hpp"
#include "buffer_type.hpp"
#include "message.hpp"

#include <cereal/types/array.hpp>
#include <cereal/types/bitset.hpp>
#include <cereal/types/chrono.hpp>
#include <cereal/types/common.hpp>
#include <cereal/types/complex.hpp>
#include <cereal/types/deque.hpp>
#include <cereal/types/forward_list.hpp>
#include <cereal/types/list.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/polymorphic.hpp>
#include <cereal/types/queue.hpp>
#include <cereal/types/set.hpp>
#include <cereal/types/stack.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/tuple.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/unordered_set.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/variant.hpp>
#include <cereal/types/vector.hpp>

#include <cereal/archives/portable_binary.hpp>
#include <cereal/details/polymorphic_impl.hpp>
#include <cereal/details/util.hpp>

#include <boost/interprocess/streams/bufferstream.hpp>
#include <boost/interprocess/streams/vectorstream.hpp>
#include <sstream>
#include <type_traits>

namespace grlx::rpc {

using ibufferstream = boost::interprocess::basic_ibufferstream<buffer_type::value_type>;
using ovectorstream = boost::interprocess::basic_ovectorstream<buffer_type>;

// Compiler-compatible branch prediction hints
#if defined(__GNUC__) || defined(__clang__)
#define GRLX_LIKELY(x)   __builtin_expect(!!(x), 1)
#define GRLX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define GRLX_LIKELY(x)   (x)
#define GRLX_UNLIKELY(x) (x)
#endif

template <typename InputArchiveT, typename OutputArchiveT>
class generic_message_encoder {
private:
  // Helper to determine if we need nvp (only for text-based archives)
  template <typename Archive>
  static constexpr bool needs_nvp_v = !std::is_same_v<Archive, cereal::PortableBinaryInputArchive> && !std::is_same_v<Archive, cereal::PortableBinaryOutputArchive> && !std::is_same_v<Archive, cereal::BinaryInputArchive> && !std::is_same_v<Archive, cereal::BinaryOutputArchive>;

  template <typename Archive, typename T>
  static void serialize_with_optimal_method(Archive& archive, T&& obj, const char* name) {
    if constexpr (needs_nvp_v<Archive>) {
      archive(cereal::make_nvp(name, obj));
    } else {
      archive(obj); // Direct serialization for binary archives
    }
  }

public:
  using buffer_type = grlx::rpc::buffer_type;

  template <typename BufferT, typename... TArgs>
  static bool decode(BufferT const& buffer, message_request<TArgs...>& request) {
    try {
      if (buffer.size() > 0) [[likely]] {
        ibufferstream istream(&buffer[0], buffer.size());
        InputArchiveT archive(istream);
        serialize_with_optimal_method(archive, request, "message_request");
        return true;
      } else {
        return false;
      }

    } catch (...) {
      return false;
    }
  }

  template <typename BufferT, typename... TArgs>
  static bool encode(BufferT& buffer, message_request<TArgs...> const& request) {
    try {
      ovectorstream  ostream;
      OutputArchiveT archive(ostream);
      serialize_with_optimal_method(archive, request, "message_request");
      ostream.swap_vector(buffer);
      return true;
    } catch (...) {
      return false;
    }
  }

  template <typename BufferT, typename TReturn>
  static bool decode(BufferT const& buffer, message_response<TReturn>& response) {
    try {
      if (GRLX_LIKELY(buffer.size() > 0)) {
        ibufferstream istream(&buffer[0], buffer.size());
        InputArchiveT archive(istream);
        serialize_with_optimal_method(archive, response, "message_response");
        return true;
      } else {
        return false;
      }
    } catch (...) {
      return false;
    }
  }

  template <typename BufferT, typename TReturn>
  static bool encode(BufferT& buffer, message_response<TReturn> const& response) {
    try {
      ovectorstream  ostream;
      OutputArchiveT archive(ostream);
      serialize_with_optimal_method(archive, response, "message_response");
      ostream.swap_vector(buffer);
      return true;
    } catch (...) {
      return false;
    }
  }
};

using binary_encoder = generic_message_encoder<cereal::PortableBinaryInputArchive, cereal::PortableBinaryOutputArchive>;

// Clean up macros
#undef GRLX_LIKELY
#undef GRLX_UNLIKELY

} // namespace grlx::rpc