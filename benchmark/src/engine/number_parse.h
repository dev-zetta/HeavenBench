#ifndef HEAVENBENCH_SRC_ENGINE_NUMBER_PARSE_H_
#define HEAVENBENCH_SRC_ENGINE_NUMBER_PARSE_H_

#include <algorithm>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <expected>
#include <string_view>
#include <system_error>

#if defined(__aocc__) || defined(__ANDROID__)
#include <array>
#include <cerrno>
#include <cstdlib>
#include <locale.h>
#include <string>
#endif

namespace heaven {

enum class NumberParseError {
    Empty,
    Invalid,
    OutOfRange,
    TrailingCharacters,
    NonFinite,
};

template <typename Number>
struct ParsedNumberPrefix {
    Number value{};
    std::size_t consumed = 0;
};

namespace detail {

struct NumberParseResult {
    const char* next = nullptr;
    std::errc error{};
};

#if defined(__aocc__) || defined(__ANDROID__)
[[nodiscard]] inline locale_t c_numeric_locale() {
    static locale_t locale = newlocale(LC_NUMERIC_MASK, "C", nullptr);
    return locale;
}
#endif

template <typename Number>
requires std::integral<Number> || std::floating_point<Number>
[[nodiscard]] inline NumberParseResult parse_chars(
        const char* begin, const char* end, Number& value) {
#if defined(__aocc__) || defined(__ANDROID__)
    // AOCC 5.2 (libc++ 17) and Android NDK r28c (libc++ 19) do not provide
    // floating-point std::from_chars.
    // Keep integer parsing on charconv and use the POSIX C locale for the
    // Linux/Android floating path so parsing remains locale-independent.
    if constexpr (std::floating_point<Number>) {
        const auto locale = c_numeric_locale();
        if (locale == nullptr) {
            return {begin, std::errc::invalid_argument};
        }

        const std::string owned(begin, end);
        // The POSIX API writes a mutable char pointer into this output slot;
        // subsequent parsing only reads through the resulting cursor.
        std::array<char*, 1> parsed_end{nullptr};
        errno = 0;
        if constexpr (std::same_as<Number, float>) {
            value = strtof_l(owned.c_str(), parsed_end.data(), locale);
        } else if constexpr (std::same_as<Number, double>) {
            value = strtod_l(owned.c_str(), parsed_end.data(), locale);
        } else {
            value = strtold_l(owned.c_str(), parsed_end.data(), locale);
        }

        const char* const next = parsed_end.front();
        if (next == owned.c_str()) {
            return {begin, std::errc::invalid_argument};
        }
        const auto consumed = static_cast<std::size_t>(
            next - owned.c_str());
        if (errno == ERANGE) {
            return {begin + consumed, std::errc::result_out_of_range};
        }
        return {begin + consumed, {}};
    } else {
        const auto [next, error] = std::from_chars(begin, end, value);
        return {next, error};
    }
#else
    const auto [next, error] = std::from_chars(begin, end, value);
    return {next, error};
#endif
}

[[nodiscard]] constexpr std::string_view
trim_ascii_left(std::string_view token) noexcept {
    constexpr std::string_view whitespace = " \t\n\r\f\v";
    token.remove_prefix(std::min(token.find_first_not_of(whitespace), token.size()));
    return token;
}

template <typename Number>
requires std::integral<Number> || std::floating_point<Number>
[[nodiscard]] inline std::expected<Number, NumberParseError>
parse_number(std::string_view token, bool require_full_token) {
    token = trim_ascii_left(token);
    if (token.empty()) {
        return std::unexpected(NumberParseError::Empty);
    }

    const bool explicit_plus = token.front() == '+';
    if (explicit_plus) {
        token.remove_prefix(1);
        if (token.empty()) {
            return std::unexpected(NumberParseError::Invalid);
        }
    }
    Number value{};
    const char* const begin = token.data();
    const char* const end = begin + token.size();
    const auto [next, error] = parse_chars(begin, end, value);

    if (error == std::errc::invalid_argument || next == begin) {
        return std::unexpected(NumberParseError::Invalid);
    }
    if (error == std::errc::result_out_of_range) {
        return std::unexpected(NumberParseError::OutOfRange);
    }
    if (require_full_token && next != end) {
        return std::unexpected(NumberParseError::TrailingCharacters);
    }
    if constexpr (std::floating_point<Number>) {
        if (!std::isfinite(value)) {
            return std::unexpected(NumberParseError::NonFinite);
        }
    }
    return value;
}

} // namespace detail

template <typename Number>
requires std::integral<Number> || std::floating_point<Number>
[[nodiscard]] inline std::expected<ParsedNumberPrefix<Number>, NumberParseError>
parse_number_prefix_with_consumed(std::string_view token) {
    const std::size_t leading = token.size() - detail::trim_ascii_left(token).size();
    token.remove_prefix(leading);
    if (token.empty()) {
        return std::unexpected(NumberParseError::Empty);
    }

    const bool explicit_plus = token.front() == '+';
    if (explicit_plus) {
        token.remove_prefix(1);
        if (token.empty()) {
            return std::unexpected(NumberParseError::Invalid);
        }
    }
    Number value{};
    const char* const begin = token.data();
    const char* const end = begin + token.size();
    const auto [next, error] = detail::parse_chars(begin, end, value);
    if (error == std::errc::invalid_argument || next == begin) {
        return std::unexpected(NumberParseError::Invalid);
    }
    if (error == std::errc::result_out_of_range) {
        return std::unexpected(NumberParseError::OutOfRange);
    }
    if constexpr (std::floating_point<Number>) {
        if (!std::isfinite(value)) {
            return std::unexpected(NumberParseError::NonFinite);
        }
    }
    return ParsedNumberPrefix<Number>{
        .value = value,
        .consumed = leading + (explicit_plus ? 1u : 0u) +
                    static_cast<std::size_t>(next - begin),
    };
}

template <typename Number>
requires std::integral<Number> || std::floating_point<Number>
[[nodiscard]] inline std::expected<Number, NumberParseError>
parse_number_strict(std::string_view token) {
    return detail::parse_number<Number>(token, true);
}

template <typename Number>
requires std::integral<Number> || std::floating_point<Number>
[[nodiscard]] inline std::expected<Number, NumberParseError>
parse_number_prefix(std::string_view token) {
    return detail::parse_number<Number>(token, false);
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_NUMBER_PARSE_H_
