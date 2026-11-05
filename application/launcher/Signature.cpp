#include "launcher/Signature.hpp"

extern "C" {
#include <monocypher-ed25519.h>
}

#include <QRegularExpression>

namespace lumora::launcher {

bool verifyEd25519(QByteArrayView message, QByteArrayView signature, QByteArrayView publicKey) {
    if (signature.size() != 64 || publicKey.size() != 32) return false;
    return crypto_ed25519_check(reinterpret_cast<const uint8_t*>(signature.data()),
                                reinterpret_cast<const uint8_t*>(publicKey.data()),
                                reinterpret_cast<const uint8_t*>(message.data()),
                                static_cast<size_t>(message.size())) == 0;
}

std::optional<QByteArray> decodeBinary(const QString& text, qsizetype expectedSize) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return std::nullopt;
    static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]+$"));
    if (hex.match(trimmed).hasMatch() && trimmed.size() == expectedSize * 2) {
        return QByteArray::fromHex(trimmed.toLatin1());
    }
    const auto decoded = QByteArray::fromBase64Encoding(trimmed.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() != expectedSize) return std::nullopt;
    return decoded.decoded;
}

QByteArray releasePublicKey() {
#if defined(LUMORA_RELEASE_PUBLIC_KEY)
    return decodeBinary(QStringLiteral(LUMORA_RELEASE_PUBLIC_KEY), 32).value_or(QByteArray());
#else
    return {};
#endif
}

} // namespace lumora::launcher
