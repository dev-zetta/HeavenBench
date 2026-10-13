#ifndef HEAVEN_TEST_FAKE_HANDLE_H
#define HEAVEN_TEST_FAKE_HANDLE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <type_traits>

namespace heaven::test {

// Host fixtures never submit these values to a native driver. One token table
// preserves identity through the SDK's void-pointer / Vulkan-pointer aliases.
struct alignas(std::max_align_t) FakeHandleToken {};

inline FakeHandleToken* fake_handle_token(std::uintptr_t value) {
    static std::map<std::uintptr_t, FakeHandleToken> tokens;
    return &tokens.try_emplace(value).first->second;
}

template<class T> T fake_handle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) {
        return value ? reinterpret_cast<T>(fake_handle_token(value)) : nullptr;
    } else {
        return static_cast<T>(value);
    }
}

} // namespace heaven::test

#endif // HEAVEN_TEST_FAKE_HANDLE_H
