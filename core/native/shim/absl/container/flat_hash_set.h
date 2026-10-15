// Native mode (gc3ds): absl's hash sets as the standard one.
#pragma once
#include <functional>
#include <memory>
#include <unordered_set>
namespace absl {
template <class K, class Hash = std::hash<K>, class Eq = std::equal_to<K>, class Alloc = std::allocator<K>>
using flat_hash_set = std::unordered_set<K, Hash, Eq, Alloc>;
template <class K, class Hash = std::hash<K>, class Eq = std::equal_to<K>, class Alloc = std::allocator<K>>
using node_hash_set = std::unordered_set<K, Hash, Eq, Alloc>;
}  // namespace absl
