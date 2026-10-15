// Native mode (gc3ds): absl::Hash for the absl container stand-ins. Types hashable with std::hash use it; types that
// define absl's AbslHashValue(H, const T&) protocol (found by ADL) are hashed through a small HashState.
#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
namespace absl {
class HashState;
template <class T> struct Hash;
namespace hash_internal {
template <class T> concept StdHashable = requires(const T& t) { std::hash<T>{}(t); };
}
class HashState {
public:
    template <class... Ts>
    static HashState combine(HashState s, const Ts&... values) {
        ((s.mix(Hash<Ts>{}(values))), ...);
        return s;
    }
    std::size_t value() const { return mValue; }
private:
    void mix(std::size_t h) { mValue ^= h + 0x9E3779B9u + (mValue << 6) + (mValue >> 2); }
    std::size_t mValue = 0;
};
template <class T>
struct Hash {
    std::size_t operator()(const T& t) const {
        if constexpr (hash_internal::StdHashable<T>) return std::hash<T>{}(t);
        else return AbslHashValue(HashState{}, t).value();
    }
};
}  // namespace absl
namespace absl {
template <>
struct Hash<std::string> {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
template <>
struct Hash<std::string_view> {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};
}  // namespace absl
