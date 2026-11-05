#pragma once

#include <nlohmann/json.hpp>
#include <QByteArray>
#include <QString>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lumora::collab {

// Live collaboration protocol.
//
// TCP carries everything that must arrive: a stream of frames, each
//   u32 big-endian length | u8 kind | payload
// where kind 0 is a UTF-8 JSON object with a "t" (type) field and kind 1 is a chunk of an asset
// file. UDP (same port number) carries presence only - camera pose and selection - as datagrams
//   "LMRU" | JSON
// and is optional: when datagrams do not get through (e.g. a TCP-only tunnel), presence falls
// back to TCP.

inline constexpr int kProtocolVersion = 1;
inline constexpr quint16 kDefaultPort = 7777;
inline constexpr std::uint32_t kMaxJsonFrame = 64u << 20;    // scene snapshots
inline constexpr std::uint32_t kChunkSize = 256u << 10;      // asset transfer granularity
inline constexpr std::uint64_t kMaxAssetSize = 1ull << 30;   // per file
inline constexpr int kMaxNameLength = 32;
inline constexpr int kMaxChatLength = 500;
inline constexpr char kUdpMagic[4] = {'L', 'M', 'R', 'U'};

enum class FrameKind : std::uint8_t { Json = 0, AssetChunk = 1 };

struct AssetChunk {
    QString path;          // project-relative, validated with isSafeAssetPath
    std::uint64_t offset = 0;
    std::uint64_t total = 0;
    QByteArray data;
};

struct Frame {
    FrameKind kind = FrameKind::Json;
    nlohmann::json message;  // kind == Json
    AssetChunk chunk;        // kind == AssetChunk
};

[[nodiscard]] QByteArray encodeMessage(const nlohmann::json& message);
[[nodiscard]] QByteArray encodeChunk(const AssetChunk& chunk);
[[nodiscard]] QByteArray encodeDatagram(const nlohmann::json& message);
[[nodiscard]] std::optional<nlohmann::json> decodeDatagram(const QByteArray& datagram);

/// Splits a TCP byte stream into frames. Oversized or malformed frames put the reader into a
/// permanent error state (the peer is then disconnected).
class FrameReader {
public:
    /// Limit for JSON frames; servers keep it small until a peer has been admitted.
    void setJsonLimit(std::uint32_t bytes) noexcept { jsonLimit_ = bytes; }
    void append(const QByteArray& bytes);
    [[nodiscard]] std::optional<Frame> next();
    [[nodiscard]] bool failed() const noexcept { return !error_.isEmpty(); }
    [[nodiscard]] const QString& error() const noexcept { return error_; }

private:
    QByteArray buffer_;
    QString error_;
    std::uint32_t jsonLimit_ = kMaxJsonFrame;
};

/// Rejects JSON nested deeper than `limit` before it reaches the (recursive) parser.
[[nodiscard]] bool jsonNestingWithin(const QByteArray& text, int limit);

/// Asset paths that may cross the network: relative to the project root, forward slashes,
/// inside Content/, no "..", no empty or dot segments, no drive letters or backslashes.
[[nodiscard]] bool isSafeAssetPath(const QString& path);

/// Project-relative files an entity record (or every record of a scene document) refers to
/// through asset fields, e.g. "Content/Models/Crate.glb" for the key "Content/Models/Crate.glb#0".
[[nodiscard]] std::vector<std::string> referencedAssetFiles(const nlohmann::json& entityRecord);
[[nodiscard]] std::vector<std::string> referencedAssetFilesOfScene(const nlohmann::json& sceneDocument);
/// Files a .gltf document loads besides itself (relative to its folder), excluding data: URIs.
[[nodiscard]] std::vector<std::string> gltfDependencies(const QByteArray& gltfJson);

struct Address {
    QString host;
    quint16 port = kDefaultPort;
};
/// Accepts "host", "host:port", "[ipv6]:port" and "lumora://host:port" - e.g. a LAN address
/// ("192.168.1.20:7777") or a tunnel address from playit.gg ("name.at.ply.gg:12345").
[[nodiscard]] std::optional<Address> parseAddress(const QString& text);
[[nodiscard]] QString formatAddress(const Address& address);

/// Display name limited to kMaxNameLength printable characters; never empty.
[[nodiscard]] QString sanitizeName(const QString& name);

/// Stable digest of a scene document (servers and clients compare these to detect drift).
[[nodiscard]] QString sceneDigest(const nlohmann::json& sceneDocument);

} // namespace lumora::collab
