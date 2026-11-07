#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lumora::net {

/// Little-endian encoding of network messages.
struct ByteWriter {
    std::vector<std::byte>& out;
    void u8(std::uint8_t v) { out.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v) {
        u16(static_cast<std::uint16_t>(v));
        u16(static_cast<std::uint16_t>(v >> 16));
    }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
    void bytes(std::span<const std::byte> b) { out.insert(out.end(), b.begin(), b.end()); }
    void bytes(std::span<const std::uint8_t> b) { bytes(std::as_bytes(b)); }
    /// u16 length, then the bytes (at most 65535).
    void str(std::string_view s) {
        const std::size_t n = s.size() < 65535 ? s.size() : 65535;
        u16(static_cast<std::uint16_t>(n));
        bytes(std::as_bytes(std::span(s.data(), n)));
    }
};

/// Reads what ByteWriter wrote; `ok` turns false (and stays) when the data runs out.
struct ByteReader {
    std::span<const std::byte> data;
    std::size_t at = 0;
    bool ok = true;
    bool need(std::size_t n) {
        if (!ok || at + n > data.size()) ok = false;
        return ok;
    }
    [[nodiscard]] bool done() const { return at >= data.size(); }
    std::uint8_t u8() { return need(1) ? static_cast<std::uint8_t>(data[at++]) : 0; }
    std::uint16_t u16() {
        const std::uint16_t lo = u8();
        return static_cast<std::uint16_t>(lo | (u8() << 8));
    }
    std::uint32_t u32() {
        const std::uint32_t lo = u16();
        return lo | (static_cast<std::uint32_t>(u16()) << 16);
    }
    std::uint64_t u64() {
        const std::uint64_t lo = u32();
        return lo | (static_cast<std::uint64_t>(u32()) << 32);
    }
    float f32() { return std::bit_cast<float>(u32()); }
    double f64() { return std::bit_cast<double>(u64()); }
    std::span<const std::byte> bytes(std::size_t n) {
        if (!need(n)) return {};
        const auto s = data.subspan(at, n);
        at += n;
        return s;
    }
    std::string str() {
        const std::size_t n = u16();
        const auto b = bytes(n);
        return ok ? std::string(reinterpret_cast<const char*>(b.data()), b.size()) : std::string();
    }
};

} // namespace lumora::net
