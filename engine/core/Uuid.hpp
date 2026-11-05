#pragma once

#include "core/Error.hpp"

#include <compare>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace lumora {

/// 64-bit random identifier used for persistent entity and asset identity.
/// Serialized as 16 lowercase hex digits (JSON numbers cannot hold 64-bit values portably).
class Uuid {
public:
    constexpr Uuid() noexcept = default;
    constexpr explicit Uuid(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] static Uuid generate();
    [[nodiscard]] static Result<Uuid> parse(std::string_view text);

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return value_ != 0; }
    [[nodiscard]] std::string toString() const;

    constexpr auto operator<=>(const Uuid&) const noexcept = default;

private:
    std::uint64_t value_ = 0;
};

} // namespace lumora

template <>
struct std::hash<lumora::Uuid> {
    std::size_t operator()(const lumora::Uuid& id) const noexcept { return std::hash<std::uint64_t>{}(id.value()); }
};
