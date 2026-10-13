#pragma once

#ifndef _WIN32
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <locale>
#include <string>

using std::abs;

#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#define sprintf_s std::snprintf

inline int wcscpy_s(wchar_t* dest, size_t capacity, const wchar_t* source) {
    if (!dest || !capacity || !source) return EINVAL;
    const size_t length = std::wcslen(source);
    if (length >= capacity) { dest[0] = L'\0'; return ERANGE; }
    std::wmemcpy(dest, source, length + 1);
    return 0;
}
template<size_t N> int wcscpy_s(wchar_t (&dest)[N], const wchar_t* source) {
    return wcscpy_s(dest, N, source);
}
inline int strcpy_s(char* dest, size_t capacity, const char* source) {
    if (!dest || !capacity || !source) return EINVAL;
    const size_t length = std::strlen(source);
    if (length >= capacity) { dest[0] = '\0'; return ERANGE; }
    std::memcpy(dest, source, length + 1);
    return 0;
}
#endif
