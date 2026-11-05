#include "core/Uuid.hpp"

#include <charconv>
#include <format>
#include <mutex>
#include <random>

namespace lumora {

Uuid Uuid::generate() {
    static std::mutex mutex;
    static std::mt19937_64 engine = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    std::scoped_lock lock(mutex);
    std::uint64_t value = 0;
    while (value == 0) {
        value = engine();
    }
    return Uuid(value);
}

Result<Uuid> Uuid::parse(std::string_view text) {
    if (text.empty() || text.size() > 16) {
        return makeError("invalid UUID '{}': expected 1-16 hex digits", text);
    }
    std::uint64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return makeError("invalid UUID '{}': not a hex number", text);
    }
    return Uuid(value);
}

std::string Uuid::toString() const { return std::format("{:016x}", value_); }

} // namespace lumora
