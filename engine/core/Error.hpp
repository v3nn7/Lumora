#pragma once

#include <expected>
#include <format>
#include <string>
#include <utility>

namespace lumora {

/// Recoverable error carried through std::expected. Unrecoverable initialization
/// failures (e.g. missing Vulkan 1.4 support) are thrown as exceptions instead.
struct Error {
    std::string message;
};

template <class T = void>
using Result = std::expected<T, Error>;

template <class... Args>
[[nodiscard]] std::unexpected<Error> makeError(std::format_string<Args...> fmt, Args&&... args) {
    return std::unexpected(Error{std::format(fmt, std::forward<Args>(args)...)});
}

} // namespace lumora
