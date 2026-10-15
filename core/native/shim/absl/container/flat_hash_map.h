// Native mode (gc3ds): absl's hash maps as std::unordered_map, plus the heterogeneous lookups absl allows and the
// standard one does not before C++26 (Dusklight's config.cpp: map[string_view], erase(string_view)). Keys hash
// through absl::Hash, which is transparent for strings.
#pragma once
#include <functional>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include "absl/hash/hash.h"
namespace absl {
template <class K, class V, class Hash = absl::Hash<K>, class Eq = std::equal_to<>,
          class Alloc = std::allocator<std::pair<const K, V>>>
class flat_hash_map : public std::unordered_map<K, V, Hash, Eq, Alloc> {
    using Base = std::unordered_map<K, V, Hash, Eq, Alloc>;
    template <class Q>
    static constexpr bool kHetero = !std::is_same_v<std::remove_cvref_t<Q>, K> && std::is_constructible_v<K, const Q&>;

public:
    using Base::Base;
    using Base::operator[];
    using Base::erase;
    template <class Q>
        requires kHetero<Q>
    V& operator[](const Q& key) {
        auto it = this->find(key);
        if (it != this->end()) return it->second;
        return Base::operator[](K(key));
    }
    template <class Q>
        requires(kHetero<Q> && !std::is_convertible_v<const Q&, typename Base::const_iterator>)
    typename Base::size_type erase(const Q& key) {
        auto it = this->find(key);
        if (it == this->end()) return 0;
        Base::erase(it);
        return 1;
    }
};
template <class K, class V, class Hash = absl::Hash<K>, class Eq = std::equal_to<>,
          class Alloc = std::allocator<std::pair<const K, V>>>
using node_hash_map = flat_hash_map<K, V, Hash, Eq, Alloc>;
}  // namespace absl
