// Native mode (gc3ds): absl's hash containers as the standard ones (Dusklight's layer uses only their map interface).
#pragma once
#include <functional>
#include <memory>
#include <unordered_map>
#include "absl/hash/hash.h"
namespace absl {
template <class K, class V, class Hash = absl::Hash<K>, class Eq = std::equal_to<>,
          class Alloc = std::allocator<std::pair<const K, V>>>
using flat_hash_map = std::unordered_map<K, V, Hash, Eq, Alloc>;
template <class K, class V, class Hash = absl::Hash<K>, class Eq = std::equal_to<>,
          class Alloc = std::allocator<std::pair<const K, V>>>
using node_hash_map = std::unordered_map<K, V, Hash, Eq, Alloc>;
}  // namespace absl
