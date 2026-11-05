#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

namespace lumora::launcher {

/// Standard Ed25519 (RFC 8032) verification: true only for a valid 64-byte signature of
/// `message` by the 32-byte `publicKey`.
[[nodiscard]] bool verifyEd25519(QByteArrayView message, QByteArrayView signature, QByteArrayView publicKey);

/// Decodes a key or signature given as base64 or hex; nullopt if neither or the wrong length.
[[nodiscard]] std::optional<QByteArray> decodeBinary(const QString& text, qsizetype expectedSize);

/// Public key release feeds are signed with, baked in at build time
/// (-DLUMORA_RELEASE_PUBLIC_KEY=<base64>); empty in builds without one.
[[nodiscard]] QByteArray releasePublicKey();

} // namespace lumora::launcher
