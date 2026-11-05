#include "collab/HostIdentity.hpp"

extern "C" {
#include <uECC.h>
}

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>

#include <cstring>

namespace lumora::collab {

namespace {

// ---- Minimal DER (ASN.1) encoding, just what an X.509 certificate needs ------------------------

QByteArray tlv(quint8 tag, const QByteArray& content) {
    QByteArray out(1, static_cast<char>(tag));
    const auto n = static_cast<quint32>(content.size());
    if (n < 0x80) {
        out.append(static_cast<char>(n));
    } else {
        QByteArray length;
        for (quint32 v = n; v != 0; v >>= 8) length.prepend(static_cast<char>(v & 0xff));
        out.append(static_cast<char>(0x80 | length.size()));
        out.append(length);
    }
    return out + content;
}

QByteArray sequence(const QByteArray& content) { return tlv(0x30, content); }

/// Positive INTEGER from big-endian bytes.
QByteArray integer(QByteArray bytes) {
    while (bytes.size() > 1 && bytes[0] == 0 && !(static_cast<quint8>(bytes[1]) & 0x80)) bytes.remove(0, 1);
    if (bytes.isEmpty()) bytes = QByteArray(1, '\0');
    if (static_cast<quint8>(bytes[0]) & 0x80) bytes.prepend('\0');
    return tlv(0x02, bytes);
}

QByteArray oid(std::initializer_list<quint8> encoded) {
    QByteArray body;
    for (quint8 b : encoded) body.append(static_cast<char>(b));
    return tlv(0x06, body);
}

const QByteArray& oidEcPublicKey() { static const QByteArray o = oid({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01}); return o; }
const QByteArray& oidPrime256v1() { static const QByteArray o = oid({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07}); return o; }
const QByteArray& oidEcdsaSha256() { static const QByteArray o = oid({0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02}); return o; }
const QByteArray& oidCommonName() { static const QByteArray o = oid({0x55, 0x04, 0x03}); return o; }

QByteArray name(const QString& commonName) {
    return sequence(tlv(0x31, sequence(oidCommonName() + tlv(0x0C, commonName.toUtf8()))));
}

QByteArray utcTime(const QDateTime& t) {
    return tlv(0x17, t.toUTC().toString(QStringLiteral("yyMMddHHmmss")).toLatin1() + "Z");
}

QByteArray bitString(const QByteArray& bytes) { return tlv(0x03, QByteArray(1, '\0') + bytes); }

QByteArray pem(const char* label, const QByteArray& der) {
    QByteArray out = QByteArray("-----BEGIN ") + label + "-----\n";
    const QByteArray b64 = der.toBase64();
    for (qsizetype i = 0; i < b64.size(); i += 64) out += b64.mid(i, 64) + "\n";
    return out + "-----END " + label + "-----\n";
}

} // namespace

Result<HostIdentity> createHostIdentity(const QString& commonName) {
    const uECC_Curve curve = uECC_secp256r1();
    uECC_set_rng([](uint8_t* dest, unsigned size) -> int {
        // fillRange needs whole quint32s: fill a temporary and copy.
        QByteArray buffer(static_cast<qsizetype>((size + 3) / 4 * 4), '\0');
        QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(buffer.data()), buffer.size() / 4);
        std::memcpy(dest, buffer.constData(), size);
        return 1;
    });
    uint8_t publicKey[64];
    uint8_t privateKey[32];
    if (!uECC_make_key(publicKey, privateKey, curve)) return makeError("could not generate the host key");
    const QByteArray point = QByteArray(1, '\x04') + QByteArray(reinterpret_cast<const char*>(publicKey), 64);

    QByteArray serial(16, '\0');
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(serial.data()), 4);
    serial[0] = static_cast<char>(serial[0] & 0x7f);  // positive
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QByteArray algorithm = sequence(oidEcdsaSha256());
    const QByteArray tbs = sequence(tlv(0xA0, integer(QByteArray(1, '\x02')))  // v3
                                    + integer(serial) + algorithm + name(commonName) +
                                    sequence(utcTime(now.addDays(-1)) + utcTime(now.addYears(10))) + name(commonName) +
                                    sequence(sequence(oidEcPublicKey() + oidPrime256v1()) + bitString(point)));

    const QByteArray digest = QCryptographicHash::hash(tbs, QCryptographicHash::Sha256);
    uint8_t signature[64];
    if (!uECC_sign(privateKey, reinterpret_cast<const uint8_t*>(digest.constData()), 32, signature, curve)) {
        return makeError("could not sign the host certificate");
    }
    const QByteArray ecdsa = sequence(integer(QByteArray(reinterpret_cast<const char*>(signature), 32)) +
                                      integer(QByteArray(reinterpret_cast<const char*>(signature + 32), 32)));
    const QByteArray certificateDer = sequence(tbs + algorithm + bitString(ecdsa));

    // SEC1 ECPrivateKey ("EC PRIVATE KEY").
    const QByteArray keyDer = sequence(integer(QByteArray(1, '\x01')) +
                                       tlv(0x04, QByteArray(reinterpret_cast<const char*>(privateKey), 32)) +
                                       tlv(0xA0, oidPrime256v1()) + tlv(0xA1, bitString(point)));
    std::memset(privateKey, 0, sizeof privateKey);

    HostIdentity identity;
    identity.certificate = QSslCertificate(certificateDer, QSsl::Der);
    identity.key = QSslKey(pem("EC PRIVATE KEY", keyDer), QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    if (identity.certificate.isNull() || identity.key.isNull()) {
        return makeError("the TLS backend rejected the generated host identity");
    }
    identity.fingerprint = certificateFingerprint(identity.certificate);
    return identity;
}

QString defaultIdentityFolder() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("collab"));
}

Result<HostIdentity> loadOrCreateHostIdentity(const QString& directory) {
    const QString certPath = QDir(directory).filePath(QStringLiteral("host.crt"));
    const QString keyPath = QDir(directory).filePath(QStringLiteral("host.key"));
    QFile certFile(certPath);
    QFile keyFile(keyPath);
    if (certFile.open(QIODevice::ReadOnly) && keyFile.open(QIODevice::ReadOnly)) {
        HostIdentity identity;
        identity.certificate = QSslCertificate(certFile.readAll(), QSsl::Pem);
        identity.key = QSslKey(keyFile.readAll(), QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
        if (identity.isValid() && identity.certificate.expiryDate() > QDateTime::currentDateTimeUtc()) {
            identity.fingerprint = certificateFingerprint(identity.certificate);
            return identity;
        }
    }
    certFile.close();
    keyFile.close();
    auto identity = createHostIdentity();
    if (!identity) return identity;
    if (!QDir().mkpath(directory)) return makeError("cannot create {}", directory.toStdString());
    QFile key(keyPath);
    if (!key.open(QIODevice::WriteOnly | QIODevice::Truncate)) return makeError("cannot write {}", keyPath.toStdString());
    key.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    key.write(identity->key.toPem());
    key.close();
    QFile cert(certPath);
    if (!cert.open(QIODevice::WriteOnly | QIODevice::Truncate)) return makeError("cannot write {}", certPath.toStdString());
    cert.write(identity->certificate.toPem());
    return identity;
}

QByteArray certificateFingerprint(const QSslCertificate& certificate) {
    return certificate.digest(QCryptographicHash::Sha256).toHex();
}

QString formatFingerprint(const QByteArray& hex, int groups) {
    QStringList parts;
    for (qsizetype i = 0; i < hex.size(); i += 4) {
        if (groups > 0 && parts.size() == groups) {
            parts << QStringLiteral("…");
            break;
        }
        parts << QString::fromLatin1(hex.mid(i, 4));
    }
    return parts.join(QLatin1Char(' '));
}

namespace {
QString knownHostKey(const QString& host, quint16 port) {
    return QStringLiteral("collab/knownHosts/") +
           QString::fromLatin1(QStringLiteral("%1:%2").arg(host.toLower()).arg(port).toUtf8().toBase64(QByteArray::Base64UrlEncoding));
}
} // namespace

QByteArray KnownHosts::fingerprintFor(const QString& host, quint16 port) {
    return QSettings().value(knownHostKey(host, port)).toByteArray();
}

void KnownHosts::remember(const QString& host, quint16 port, const QByteArray& fingerprint) {
    QSettings().setValue(knownHostKey(host, port), fingerprint);
}

void KnownHosts::forget(const QString& host, quint16 port) { QSettings().remove(knownHostKey(host, port)); }

} // namespace lumora::collab
