#pragma once

#include <tuple>
#include <utility>

namespace grlx::rpc {
namespace details {

template <typename Tuple, typename ArchiveT, std::size_t... I>
void for_each_arg_impl(Tuple&& tuple, ArchiveT& archive, std::index_sequence<I...>) {
  (archive(std::get<I>(tuple)), ...);
}

template <typename Tuple, typename ArchiveT>
constexpr void for_each_arg(Tuple&& tuple, ArchiveT& archive) {
  for_each_arg_impl(std::forward<Tuple>(tuple), archive, std::make_index_sequence<std::tuple_size<std::remove_reference_t<Tuple>>::value>{});
}
} // namespace details

template <typename Tuple, typename ArchiveT>
constexpr void for_each_arg(Tuple&& tuple, ArchiveT& archive) {
  details::for_each_arg_impl(std::forward<Tuple>(tuple), archive, std::make_index_sequence<std::tuple_size<std::remove_reference_t<Tuple>>::value>{});
}

template <typename... ArgsT>
struct message_request {
  using ArgsType = std::tuple<ArgsT...>;
  ArgsType args;

  template <typename Archive>
  void serialize(Archive& archive) {
    for_each_arg(args, archive);
  }
};

template <typename TResult>
struct message_response {
  TResult result;
  template <class Archive>
  void serialize(Archive& archive) {
    archive(result);
  }
};

template <>
struct message_response<void> {
  template <class Archive>
  void serialize(Archive&) {
  }
};
} // namespace grlx::rpc