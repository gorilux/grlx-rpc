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

// ---------------------------------------------------------------------------
// WI-10 — pre-auth container-size (OOM) guard.
//
// cereal deserializes a container by reading a size prefix and then resize()-ing
// the container to that size BEFORE reading any element bytes. A hostile peer can
// send a tiny frame whose size prefix claims e.g. 10^18 elements and drive a
// multi-terabyte allocation → an out-of-memory kill — all PRE-authentication (the
// public_ handshake frame is decoded before any auth runs). The 8 MiB frame cap
// does NOT bound these in-band sizes.
//
// `bounded_binary_input_archive` is a byte-compatible clone of
// cereal::PortableBinaryInputArchive (it reads exactly what
// PortableBinaryOutputArchive writes, so the wire format is unchanged) that
// additionally rejects any container size prefix which cannot be backed by the
// bytes remaining in the frame. It is a DISTINCT archive type — not a subclass —
// because cereal's CRTP bakes the concrete archive type into dispatch (a
// subclass's overrides are never selected), and a distinct type keeps the bounded
// SizeTag handler scoped to RPC decoding via ADL, with no ODR hazard for other
// cereal users (persistence, snapshots) in the process.
class bounded_binary_input_archive
  : public cereal::InputArchive<bounded_binary_input_archive, cereal::AllowEmptyClassElision> {
public:
  explicit bounded_binary_input_archive(std::istream& stream)
    : cereal::InputArchive<bounded_binary_input_archive, cereal::AllowEmptyClassElision>(this)
    , stream_(stream) {
    // Measure the frame length once so container sizes can be bounded against the
    // bytes that actually remain. ibufferstream supports seeking; if measurement
    // ever fails we leave total_ = 0, which disables the bound (fail-open to the
    // pre-WI-10 behavior rather than risk false-rejecting legitimate traffic).
    auto const start = stream.tellg();
    if (start >= 0 && stream.seekg(0, std::ios::end)) {
      auto const end = stream.tellg();
      if (end >= 0) total_ = static_cast<std::size_t>(end);
      stream.seekg(start);
    }
    stream.clear();
    // Consume the endianness header byte exactly like PortableBinaryInputArchive.
    std::uint8_t stream_little_endian = 0;
    this->operator()(stream_little_endian);
    convert_endianness_ =
      (cereal::portable_binary_detail::is_little_endian() ? 1u : 0u) ^ stream_little_endian;
  }

  ~bounded_binary_input_archive() CEREAL_NOEXCEPT = default;

  //! Reads size bytes from the stream (mirrors PortableBinaryInputArchive).
  template <std::streamsize DataSize>
  void loadBinary(void* const data, std::streamsize size) {
    auto const read = stream_.rdbuf()->sgetn(reinterpret_cast<char*>(data), size);
    if (read != size)
      throw cereal::Exception("Failed to read " + std::to_string(size) + " bytes from input stream! Read " + std::to_string(read));
    if (convert_endianness_) {
      auto* ptr = reinterpret_cast<std::uint8_t*>(data);
      for (std::streamsize i = 0; i < size; i += DataSize)
        cereal::portable_binary_detail::swap_bytes<DataSize>(ptr + i);
    }
  }

  bool        bounded() const noexcept { return total_ > 0; }
  std::size_t remaining_bytes() {
    auto const pos      = stream_.tellg();
    auto const consumed = pos < 0 ? total_ : static_cast<std::size_t>(pos);
    return total_ > consumed ? total_ - consumed : 0;
  }

private:
  std::istream& stream_;
  std::size_t   total_              = 0;
  std::uint8_t  convert_endianness_ = 0;
};

// POD load — identical to cereal's PortableBinaryInputArchive path.
template <class T>
inline typename std::enable_if<std::is_arithmetic<T>::value, void>::type
CEREAL_LOAD_FUNCTION_NAME(bounded_binary_input_archive& ar, T& t) {
  ar.template loadBinary<sizeof(T)>(std::addressof(t), sizeof(t));
}

// NameValuePair passthrough (binary archives ignore the name).
template <class T>
inline void CEREAL_SERIALIZE_FUNCTION_NAME(bounded_binary_input_archive& ar, cereal::NameValuePair<T>& t) {
  ar(t.value);
}

// SizeTag load WITH the bound — the guard. Read the size, then reject it if it
// exceeds what the remaining frame bytes could possibly back. Every container
// element in this protocol occupies >= 1 byte on the wire, so a legitimate size
// is always <= remaining bytes; anything larger is a forged prefix aimed at the
// allocator. The small slack absorbs framing/position rounding.
template <class T>
inline void CEREAL_SERIALIZE_FUNCTION_NAME(bounded_binary_input_archive& ar, cereal::SizeTag<T>& t) {
  ar(t.size);
  if (ar.bounded() &&
      static_cast<std::uint64_t>(t.size) > static_cast<std::uint64_t>(ar.remaining_bytes()) + 64ull) {
    throw cereal::Exception("grlx-rpc: container size prefix exceeds frame bounds (rejected pre-allocation)");
  }
}

// BinaryData load — identical to cereal's.
template <class T>
inline void CEREAL_LOAD_FUNCTION_NAME(bounded_binary_input_archive& ar, cereal::BinaryData<T>& bd) {
  typedef typename std::remove_pointer<T>::type TT;
  ar.template loadBinary<sizeof(TT)>(bd.data, static_cast<std::streamsize>(bd.size));
}

template <typename InputArchiveT, typename OutputArchiveT>
class generic_message_encoder {
private:
  // Helper to determine if we need nvp (only for text-based archives)
  template <typename Archive>
  static constexpr bool needs_nvp_v = !std::is_same_v<Archive, cereal::PortableBinaryInputArchive> && !std::is_same_v<Archive, cereal::PortableBinaryOutputArchive> && !std::is_same_v<Archive, cereal::BinaryInputArchive> && !std::is_same_v<Archive, cereal::BinaryOutputArchive> && !std::is_same_v<Archive, bounded_binary_input_archive>;

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

// Decode with the bounded archive (WI-10 OOM guard); encode with the stock
// PortableBinaryOutputArchive so the wire format is byte-identical — an unchanged
// peer's PortableBinaryInputArchive still reads our responses, and we still read
// theirs. Server-only deploy stays wire-compatible.
using binary_encoder = generic_message_encoder<bounded_binary_input_archive, cereal::PortableBinaryOutputArchive>;

// Clean up macros
#undef GRLX_LIKELY
#undef GRLX_UNLIKELY

} // namespace grlx::rpc

// cereal requires every input archive to name its paired OUTPUT archive (the
// save_minimal/load_minimal trait machinery resolves it via get_output_from_input;
// without it cereal static-asserts "Could not find an associated output archive").
// Pair the bounded input archive with the stock PortableBinaryOutputArchive — our
// encode side. We specialize ONLY get_output_from_input, NOT the full
// CEREAL_SETUP_ARCHIVE_TRAITS macro: that macro also defines
// get_input_from_output<PortableBinaryOutputArchive>, which cereal already claims
// for PortableBinaryInputArchive, so using it would be a conflicting redefinition.
namespace cereal { namespace traits { namespace detail {
template <>
struct get_output_from_input<grlx::rpc::bounded_binary_input_archive> {
  using type = cereal::PortableBinaryOutputArchive;
};
}}} // namespace cereal::traits::detail