#pragma once

#include "common/ModuleExport.hpp"
#include "core/Error.hpp"

#include <QByteArray>
#include <QSslCertificate>
#include <QSslKey>
#include <QString>

namespace lumora::collab {

/// What a session host proves itself with: a self-signed X.509 certificate (ECDSA P-256) and its
/// private key. There is no certificate authority: guests pin the certificate's SHA-256
/// fingerprint, which travels inside the address the host shares (lumora://host:port#<sha256>)
/// or is confirmed once and remembered (trust on first use).
struct HostIdentity {
    QSslCertificate certificate{};  // {}: its default constructor is explicit (MSVC rejects aggregate {} otherwise)
    QSslKey key{};
    QByteArray fingerprint;  // SHA-256 of the DER certificate, lower-case hex

    [[nodiscard]] bool isValid() const { return !certificate.isNull() && !key.isNull(); }
};

/// Creates a new identity (ECDSA P-256 key, certificate valid for ten years).
[[nodiscard]] Result<HostIdentity> createHostIdentity(const QString& commonName = QStringLiteral("Lumora session host"));
/// The identity stored in `directory` (host.crt, host.key - owner-only), created on first use so
/// guests that pinned or remembered it keep trusting the host across sessions.
[[nodiscard]] Result<HostIdentity> loadOrCreateHostIdentity(const QString& directory);
/// Default storage folder: <AppData>/collab.
[[nodiscard]] QString defaultIdentityFolder();

[[nodiscard]] QByteArray certificateFingerprint(const QSslCertificate& certificate);
/// "ab12 cd34 ..." - groups of four for reading out loud; `groups` limits the length (0 = all).
[[nodiscard]] QString formatFingerprint(const QByteArray& hex, int groups = 0);

/// Fingerprints of hosts the user chose to trust, by "host:port" (QSettings).
class KnownHosts {
public:
    [[nodiscard]] static QByteArray fingerprintFor(const QString& host, quint16 port);
    static void remember(const QString& host, quint16 port, const QByteArray& fingerprint);
    static void forget(const QString& host, quint16 port);
};

} // namespace lumora::collab
