// Editor installer: release feed parsing, Ed25519 signatures, hostile ZIP archives and a full
// install from a local (file://) feed, exactly as the Hub runs it.
#include "TestUtils.hpp"

#include "launcher/EditorInstaller.hpp"
#include "launcher/EditorRegistry.hpp"
#include "launcher/FileList.hpp"
#include "launcher/ReleaseManifest.hpp"
#include "launcher/Signature.hpp"
#include "launcher/ZipExtractor.hpp"

#include <miniz.h>
extern "C" {
#include <monocypher-ed25519.h>
}

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>
#include <map>

using namespace lumora;
using namespace lumora::launcher;

namespace {

bool waitUntil(const std::function<bool()>& done, int timeoutMs = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

/// Minimal ZIP writer (stored entries) that can record Unix modes and symlinks, and craft
/// hostile names - things a well-behaved writer would refuse to produce.
class ZipWriter {
public:
    void file(const std::string& name, const QByteArray& data, std::uint32_t mode = 0100644) { add(name, data, mode); }
    void symlink(const std::string& name, const std::string& target) {
        add(name, QByteArray::fromStdString(target), 0120777);
    }
    void directory(const std::string& name) { add(name, {}, 040755); }
    bool save(const QString& path) const {
        QByteArray out = local_;
        const auto cdOffset = static_cast<quint32>(out.size());
        out += central_;
        QByteArray end(22, '\0');
        put32(end, 0, 0x06054b50);
        put16(end, 8, count_);
        put16(end, 10, count_);
        put32(end, 12, static_cast<quint32>(central_.size()));
        put32(end, 16, cdOffset);
        out += end;
        QFile f(path);
        return f.open(QIODevice::WriteOnly) && f.write(out) == out.size();
    }

private:
    static void put16(QByteArray& b, int at, quint16 v) { qToLittleEndian<quint16>(v, b.data() + at); }
    static void put32(QByteArray& b, int at, quint32 v) { qToLittleEndian<quint32>(v, b.data() + at); }
    void add(const std::string& name, const QByteArray& data, std::uint32_t mode) {
        const auto crc = static_cast<quint32>(
            mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.constData()), static_cast<size_t>(data.size())));
        const auto offset = static_cast<quint32>(local_.size());
        QByteArray lh(30, '\0');
        put32(lh, 0, 0x04034b50);
        put16(lh, 4, 20);
        put32(lh, 14, crc);
        put32(lh, 18, static_cast<quint32>(data.size()));
        put32(lh, 22, static_cast<quint32>(data.size()));
        put16(lh, 26, static_cast<quint16>(name.size()));
        local_ += lh + QByteArray::fromStdString(name) + data;
        QByteArray ch(46, '\0');
        put32(ch, 0, 0x02014b50);
        put16(ch, 4, (3 << 8) | 20);  // made by Unix
        put16(ch, 6, 20);
        put32(ch, 16, crc);
        put32(ch, 20, static_cast<quint32>(data.size()));
        put32(ch, 24, static_cast<quint32>(data.size()));
        put16(ch, 28, static_cast<quint16>(name.size()));
        put32(ch, 38, mode << 16);
        put32(ch, 42, offset);
        central_ += ch + QByteArray::fromStdString(name);
        ++count_;
    }
    QByteArray local_;
    QByteArray central_;
    quint16 count_ = 0;
};

struct KeyPair {
    QByteArray secret{64, '\0'};
    QByteArray pub{32, '\0'};
    KeyPair() {
        uint8_t seed[32];
        for (int i = 0; i < 32; ++i) seed[i] = static_cast<uint8_t>(i * 7 + 3);  // deterministic test key
        crypto_ed25519_key_pair(reinterpret_cast<uint8_t*>(secret.data()), reinterpret_cast<uint8_t*>(pub.data()), seed);
    }
    [[nodiscard]] QByteArray sign(const QByteArray& message) const {
        QByteArray signature(64, '\0');
        crypto_ed25519_sign(reinterpret_cast<uint8_t*>(signature.data()), reinterpret_cast<const uint8_t*>(secret.constData()),
                            reinterpret_cast<const uint8_t*>(message.constData()), static_cast<size_t>(message.size()));
        return signature;
    }
};

QString qpath(const std::filesystem::path& p) { return QString::fromStdString(p.string()); }

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray readFileBytes(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QByteArray sha256Hex(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }

} // namespace

// ---- Versions and feeds ------------------------------------------------------------------------

TEST(LauncherVersions, ValidatesAndOrders) {
    for (const char* ok : {"0.1.0", "10.20.30", "1.0.0-beta.2", "2.0.0-rc1"}) EXPECT_TRUE(isValidVersion(QString::fromLatin1(ok))) << ok;
    for (const char* bad : {"", "1.0", "1.0.0.0", "../1.0.0", "1.0.0-", "1.0.0-beta..1", "1.0.0 ", "v1.0.0"}) {
        EXPECT_FALSE(isValidVersion(QString::fromLatin1(bad))) << bad;
    }
    EXPECT_LT(compareVersions(QStringLiteral("0.9.9"), QStringLiteral("0.10.0")), 0);
    EXPECT_LT(compareVersions(QStringLiteral("1.0.0-rc.1"), QStringLiteral("1.0.0")), 0);
    EXPECT_LT(compareVersions(QStringLiteral("1.0.0-beta.2"), QStringLiteral("1.0.0-beta.11")), 0);
    EXPECT_LT(compareVersions(QStringLiteral("1.0.0-alpha"), QStringLiteral("1.0.0-beta")), 0);
    EXPECT_EQ(compareVersions(QStringLiteral("2.1.3"), QStringLiteral("2.1.3")), 0);
}

TEST(LauncherVersions, ReadsTheEditorBanner) {
    EXPECT_EQ(versionFromBanner(QStringLiteral("Lumora Engine 0.4.0 (Release)\n")), QStringLiteral("0.4.0"));
    EXPECT_EQ(versionFromBanner(QStringLiteral("Lumora 1.2.3-beta.1 (Debug)")), QStringLiteral("1.2.3-beta.1"));
    EXPECT_FALSE(versionFromBanner(QStringLiteral("SomethingElse 1.0.0")).has_value());
    EXPECT_FALSE(versionFromBanner(QStringLiteral("Lumora Engine (Debug)")).has_value());
}

TEST(LauncherManifest, ParsesResolvesAndSorts) {
    const QByteArray feed = R"({"format":"lumora.releases","version":1,"releases":[
        {"version":"0.4.0","channel":"stable","packages":{"linux-x64":{"url":"editor/0.4.0/a.zip","size":10,
          "sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}},
        {"version":"0.5.0-beta.1","channel":"beta","packages":{"linux-x64":{"url":"https://cdn.example.com/b.zip","size":20,
          "sha256":"BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"}},"notes":"try it"}]})";
    auto manifest = ReleaseManifest::parse(feed, QUrl(QStringLiteral("https://releases.example.com/editor/manifest.json")));
    ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
    ASSERT_EQ(manifest->releases.size(), 2u);
    EXPECT_EQ(manifest->releases[0].version, QStringLiteral("0.5.0-beta.1"));  // newest first
    const PackageInfo* stable = manifest->releases[1].packageFor(QStringLiteral("linux-x64"));
    ASSERT_NE(stable, nullptr);
    EXPECT_EQ(stable->url.toString(), QStringLiteral("https://releases.example.com/editor/editor/0.4.0/a.zip"));
    EXPECT_EQ(manifest->releases[0].packages[0].sha256, QByteArray(64, 'b'));  // normalised to lower case
    EXPECT_EQ(manifest->latest(QStringLiteral("linux-x64"), Channel::Stable)->version, QStringLiteral("0.4.0"));
    EXPECT_EQ(manifest->latest(QStringLiteral("linux-x64"))->version, QStringLiteral("0.5.0-beta.1"));
    EXPECT_EQ(manifest->latest(QStringLiteral("windows-x64")), nullptr);
}

TEST(LauncherChannels, FilterFromStableToAlpha) {
    EXPECT_EQ(channelOfVersion(QStringLiteral("0.4.0")), Channel::Stable);
    EXPECT_EQ(channelOfVersion(QStringLiteral("0.5.0-beta.2")), Channel::Beta);
    EXPECT_EQ(channelOfVersion(QStringLiteral("0.5.0-rc.1")), Channel::Beta);
    EXPECT_EQ(channelOfVersion(QStringLiteral("0.6.0-alpha.7")), Channel::Alpha);
    EXPECT_EQ(channelFromName(QStringLiteral("stable")), Channel::Stable);
    EXPECT_EQ(channelFromName(QStringLiteral("nightly")), Channel::Alpha) << "unknown channels count as least stable";
    EXPECT_EQ(channelName(Channel::Beta), QStringLiteral("beta"));

    const std::string sha(64, 'c');
    auto release = [&](const char* version, const char* channel) {
        return std::string(R"({"version":")") + version + R"(","channel":")" + channel +
               R"(","packages":{"linux-x64":{"url":"a.zip","size":1,"sha256":")" + sha + R"("}}})";
    };
    const QByteArray feed = QByteArray::fromStdString(R"({"format":"lumora.releases","version":1,"releases":[)" +
                                                      release("0.4.0", "stable") + "," + release("0.5.0-beta.1", "beta") +
                                                      "," + release("0.6.0-alpha.3", "alpha") + "," +
                                                      release("0.3.0", "stable") + "]}");
    auto manifest = ReleaseManifest::parse(feed, QUrl(QStringLiteral("https://x/editor/manifest.json")));
    ASSERT_TRUE(manifest.has_value()) << manifest.error().message;
    const QString linux = QStringLiteral("linux-x64");
    EXPECT_EQ(manifest->latest(linux, Channel::Stable)->version, QStringLiteral("0.4.0"));
    EXPECT_EQ(manifest->latest(linux, Channel::Beta)->version, QStringLiteral("0.5.0-beta.1"));
    EXPECT_EQ(manifest->latest(linux, Channel::Alpha)->version, QStringLiteral("0.6.0-alpha.3"));
    EXPECT_EQ(manifest->visible(linux, Channel::Stable).size(), 2u);
    EXPECT_EQ(manifest->visible(linux, Channel::Beta).size(), 3u);
    EXPECT_EQ(manifest->visible(linux, Channel::Alpha).size(), 4u);
    EXPECT_TRUE(manifest->visible(QStringLiteral("windows-x64"), Channel::Alpha).empty());
}

TEST(LauncherManifest, RejectsMalformedFeeds) {
    const QUrl base(QStringLiteral("https://x/manifest.json"));
    const std::string sha(64, 'a');
    auto feed = [&](const std::string& release) {
        return QByteArray::fromStdString(R"({"format":"lumora.releases","version":1,"releases":[)" + release + "]}");
    };
    EXPECT_FALSE(ReleaseManifest::parse("not json", base).has_value());
    EXPECT_FALSE(ReleaseManifest::parse(R"({"format":"other","version":1,"releases":[]})", base).has_value());
    EXPECT_FALSE(ReleaseManifest::parse(feed(R"({"version":"../../evil","packages":{"linux-x64":{"url":"a","size":1,"sha256":")" + sha + R"("}}})"), base).has_value());
    EXPECT_FALSE(ReleaseManifest::parse(feed(R"({"version":"1.0.0","packages":{"linux-x64":{"url":"a","size":1,"sha256":"abc"}}})"), base).has_value());
    EXPECT_FALSE(ReleaseManifest::parse(feed(R"({"version":"1.0.0","packages":{"linux-x64":{"url":"a","size":0,"sha256":")" + sha + R"("}}})"), base).has_value());
    EXPECT_FALSE(ReleaseManifest::parse(feed(R"({"version":"1.0.0"})"), base).has_value());
}

TEST(LauncherSignature, VerifiesEd25519) {
    const KeyPair key;
    const QByteArray message("release feed bytes");
    const QByteArray signature = key.sign(message);
    EXPECT_TRUE(verifyEd25519(message, signature, key.pub));
    EXPECT_FALSE(verifyEd25519(QByteArray("release feed bytez"), signature, key.pub));
    QByteArray otherKey = key.pub;
    otherKey[0] = static_cast<char>(otherKey[0] ^ 1);
    EXPECT_FALSE(verifyEd25519(message, signature, otherKey));
    EXPECT_FALSE(verifyEd25519(message, signature.left(63), key.pub));

    // Interoperates with the release tooling (scripts/release/manifest.py, Python 'cryptography'):
    // same seed -> same key, and Ed25519 signatures are deterministic.
    EXPECT_EQ(key.pub.toBase64(), QByteArray("dVxMuSVsp83ErP3Gz+7ahJAX5bn5UU6ZGRvWfgsNQnY="));
    const QByteArray fromPython = QByteArray::fromBase64(
        "xIw3fn6lHh1GvrfP1LsYUNWwUS/MTc0hynKDaBo5Ov2ejvdscZdgD8kQe4Jb9YBXNEgG+pWAJJrkAvr1kqfgCQ==");
    EXPECT_TRUE(verifyEd25519(QByteArray("lumora interop"), fromPython, key.pub));
    EXPECT_EQ(key.sign(QByteArray("lumora interop")), fromPython);

    EXPECT_EQ(decodeBinary(QString::fromLatin1(key.pub.toBase64()), 32), key.pub);
    EXPECT_EQ(decodeBinary(QString::fromLatin1(key.pub.toHex()), 32), key.pub);
    EXPECT_FALSE(decodeBinary(QStringLiteral("not a key"), 32).has_value());
    EXPECT_FALSE(decodeBinary(QString::fromLatin1(key.pub.left(31).toBase64()), 32).has_value());
}

// ---- ZIP extraction ----------------------------------------------------------------------------

TEST(LauncherZip, ExtractsFilesModesAndLinks) {
    test::TempDir dir;
    ZipWriter zip;
    zip.directory("bin/");
    zip.file("bin/lumora-editor", "#!/bin/sh\necho hi\n", 0100755);
    zip.file("lib/libLumora.so.1", QByteArray(5000, 'x'));
    zip.symlink("lib/libLumora.so", "libLumora.so.1");
    zip.file("shaders/mesh.frag.spv", "spirv");
    const QString archive = qpath(dir.path() / "pkg.zip");
    ASSERT_TRUE(zip.save(archive));
    const QString out = qpath(dir.path() / "out");
    auto stats = extractZip(archive, out);
    ASSERT_TRUE(stats.has_value()) << stats.error().message;
    EXPECT_EQ(stats->files, 3u);
    EXPECT_EQ(QFile(out + "/lib/libLumora.so.1").size(), 5000);
    EXPECT_EQ(QFile(out + "/shaders/mesh.frag.spv").size(), 5);
#if !defined(Q_OS_WIN)
    EXPECT_TRUE(QFileInfo(out + "/bin/lumora-editor").isExecutable());
    EXPECT_FALSE(QFileInfo(out + "/lib/libLumora.so.1").isExecutable());
    EXPECT_TRUE(QFileInfo(out + "/lib/libLumora.so").isSymLink());
    EXPECT_EQ(QFileInfo(out + "/lib/libLumora.so").symLinkTarget(), QFileInfo(out + "/lib/libLumora.so.1").absoluteFilePath());
#endif
}

TEST(LauncherZip, RefusesHostileArchives) {
    test::TempDir dir;
    const QString out = qpath(dir.path() / "out");
    auto refuses = [&](const std::function<void(ZipWriter&)>& build) {
        ZipWriter zip;
        zip.file("bin/ok", "ok");
        build(zip);
        const QString archive = qpath(dir.path() / "evil.zip");
        if (!zip.save(archive)) return false;
        return !extractZip(archive, out).has_value();
    };
    EXPECT_TRUE(refuses([](ZipWriter& z) { z.file("../escape.txt", "x"); }));
    EXPECT_TRUE(refuses([](ZipWriter& z) { z.file("a/../../escape.txt", "x"); }));
    EXPECT_TRUE(refuses([](ZipWriter& z) { z.file("/tmp/abs.txt", "x"); }));
    EXPECT_TRUE(refuses([](ZipWriter& z) { z.file("C:/windows.txt", "x"); }));
    EXPECT_TRUE(refuses([](ZipWriter& z) { z.file("a\\..\\b.txt", "x"); }));
    EXPECT_TRUE(refuses([](ZipWriter& z) { z.symlink("lib/out", "/etc/passwd"); }));
    EXPECT_TRUE(refuses([](ZipWriter& z) { z.symlink("lib/out", "../../outside"); }));
    EXPECT_TRUE(refuses([](ZipWriter& z) { z.symlink("lib/here", "."); }));
    // Nothing may have been written next to the destination.
    EXPECT_FALSE(std::filesystem::exists(dir.path() / "escape.txt"));
    EXPECT_FALSE(QFileInfo::exists(QStringLiteral("/tmp/abs.txt")));
    // Hostile names are detected before anything is extracted.
    EXPECT_FALSE(QFileInfo::exists(out + "/bin/ok"));

    ZipWriter big;
    big.file("a.bin", QByteArray(4096, 'a'));
    big.file("b.bin", QByteArray(4096, 'b'));
    ASSERT_TRUE(big.save(qpath(dir.path() / "big.zip")));
    ExtractLimits limits;
    limits.maxTotalBytes = 6000;
    EXPECT_FALSE(extractZip(qpath(dir.path() / "big.zip"), qpath(dir.path() / "big"), limits).has_value());
    EXPECT_FALSE(extractZip(qpath(dir.path() / "missing.zip"), out).has_value());
    ASSERT_TRUE(writeFile(qpath(dir.path() / "garbage.zip"), "this is not a zip"));
    EXPECT_FALSE(extractZip(qpath(dir.path() / "garbage.zip"), out).has_value());
}

// ---- Install from a feed -----------------------------------------------------------------------

namespace {

struct Feed {
    test::TempDir dir;
    KeyPair key;
    QString root() const { return qpath(dir.path() / "installs"); }
    QUrl url() const { return QUrl::fromLocalFile(qpath(dir.path() / "feed" / "manifest.json")); }

    /// Publishes a package and a (signed) manifest the way CI does.
    void publish(const QString& version, const ZipWriter& package, bool sign = true, bool corruptHash = false) {
        const QString zipPath = qpath(dir.path() / "feed" / "editor") + "/" + version + "/package.zip";
        QDir().mkpath(QFileInfo(zipPath).absolutePath());
        ASSERT_TRUE(package.save(zipPath));
        QFile zip(zipPath);
        ASSERT_TRUE(zip.open(QIODevice::ReadOnly));
        const QByteArray bytes = zip.readAll();
        const QByteArray sha = corruptHash ? QByteArray(64, '0') : sha256Hex(bytes);
        const QByteArray manifest = QStringLiteral(R"({"format":"lumora.releases","version":1,"releases":[
            {"version":"%1","channel":"stable","date":"2026-11-05","packages":{"%2":{"url":"editor/%1/package.zip",
             "size":%3,"sha256":"%4"}}}]})")
                                        .arg(version, currentPlatform())
                                        .arg(bytes.size())
                                        .arg(QString::fromLatin1(sha))
                                        .toUtf8();
        ASSERT_TRUE(writeFile(qpath(dir.path() / "feed" / "manifest.json"), manifest));
        ASSERT_TRUE(writeFile(qpath(dir.path() / "feed" / "manifest.json.sig"),
                              sign ? key.sign(manifest).toBase64() : QByteArray(88, 'A')));
    }
};

ZipWriter editorPackage(const char* prefix = "") {
    ZipWriter zip;
    const std::string p(prefix);
    zip.file(p + editorExecutable().toStdString(), "#!/bin/sh\nexit 0\n", 0100755);
    zip.file(p + "lib/libQt6Core.so.6", QByteArray(2048, 'q'));
    zip.symlink(p + "lib/libQt6Core.so", "libQt6Core.so.6");
    zip.file(p + "shaders/mesh.vert.spv", "spirv");
    return zip;
}

struct Outcome {
    std::optional<ReleaseManifest> manifest;
    QString error;
    std::optional<InstalledEditor> installed;
};

Outcome fetchAndInstall(EditorInstaller& installer, const QUrl& url, bool install = true) {
    Outcome o;
    bool done = false;
    QObject::connect(&installer, &EditorInstaller::manifestReady, [&](const ReleaseManifest& m) {
        o.manifest = m;
        if (!install) {
            done = true;
            return;
        }
        const ReleaseInfo& r = m.releases.front();
        installer.install(r, *r.packageFor(currentPlatform()));
    });
    QObject::connect(&installer, &EditorInstaller::manifestFailed, [&](const QString& e) {
        o.error = e;
        done = true;
    });
    QObject::connect(&installer, &EditorInstaller::installCompleted, [&](const InstalledEditor& e) {
        o.installed = e;
        done = true;
    });
    QObject::connect(&installer, &EditorInstaller::installFailed, [&](const QString& e) {
        o.error = e;
        done = true;
    });
    installer.fetchManifest(url);
    waitUntil([&] { return done; });
    installer.disconnect();
    return o;
}

} // namespace

TEST(LauncherInstall, InstallsASignedReleaseFromTheFeed) {
    Feed feed;
    feed.publish(QStringLiteral("0.4.0"), editorPackage());
    EditorInstaller installer(feed.root());
    installer.setPublicKey(feed.key.pub);
    const Outcome o = fetchAndInstall(installer, feed.url());
    ASSERT_TRUE(o.installed.has_value()) << o.error.toStdString();
    EXPECT_EQ(o.installed->version, QStringLiteral("0.4.0"));
    EXPECT_EQ(o.installed->directory, QDir(feed.root()).filePath(QStringLiteral("0.4.0")));
    EXPECT_TRUE(QFileInfo(o.installed->executable).isFile());
#if !defined(Q_OS_WIN)
    EXPECT_TRUE(QFileInfo(o.installed->executable).isExecutable());
    EXPECT_TRUE(QFileInfo(o.installed->directory + "/lib/libQt6Core.so").isSymLink());
#endif
    ASSERT_EQ(installer.installed().size(), 1u);
    EXPECT_TRUE(installer.find(QStringLiteral("0.4.0")).has_value());
    // No staging or download leftovers.
    EXPECT_TRUE(QDir(feed.root() + "/.downloads").isEmpty());
    EXPECT_FALSE(QFileInfo::exists(feed.root() + "/.staging-0.4.0"));

    // Installing it twice is refused; removing works.
    const Outcome again = fetchAndInstall(installer, feed.url());
    EXPECT_FALSE(again.installed.has_value());
    EXPECT_TRUE(again.error.contains(QStringLiteral("already installed")));
    ASSERT_TRUE(installer.uninstall(QStringLiteral("0.4.0")).has_value());
    EXPECT_TRUE(installer.installed().empty());
    EXPECT_FALSE(QFileInfo::exists(feed.root() + "/0.4.0"));
}

TEST(LauncherInstall, AcceptsAPackageWrappedInAFolder) {
    Feed feed;
    feed.publish(QStringLiteral("0.4.1"), editorPackage("lumora-editor-0.4.1-linux-x64/"));
    EditorInstaller installer(feed.root());
    installer.setPublicKey(feed.key.pub);
    const Outcome o = fetchAndInstall(installer, feed.url());
    ASSERT_TRUE(o.installed.has_value()) << o.error.toStdString();
    EXPECT_TRUE(QFileInfo(feed.root() + "/0.4.1/" + editorExecutable()).isFile());
}

TEST(LauncherInstall, RefusesForgedFeedsAndCorruptPackages) {
    {
        Feed feed;
        feed.publish(QStringLiteral("0.4.0"), editorPackage(), /*sign=*/false);
        EditorInstaller installer(feed.root());
        installer.setPublicKey(feed.key.pub);
        const Outcome o = fetchAndInstall(installer, feed.url());
        EXPECT_FALSE(o.manifest.has_value());
        EXPECT_TRUE(o.error.contains(QStringLiteral("signature")));
    }
    {
        Feed feed;
        feed.publish(QStringLiteral("0.4.0"), editorPackage(), true, /*corruptHash=*/true);
        EditorInstaller installer(feed.root());
        installer.setPublicKey(feed.key.pub);
        const Outcome o = fetchAndInstall(installer, feed.url());
        EXPECT_FALSE(o.installed.has_value());
        EXPECT_TRUE(o.error.contains(QStringLiteral("checksum")));
        EXPECT_TRUE(installer.installed().empty());
        EXPECT_FALSE(QFileInfo::exists(feed.root() + "/0.4.0"));
    }
    {
        Feed feed;  // a package without the editor binary
        ZipWriter junk;
        junk.file("readme.txt", "nothing here");
        feed.publish(QStringLiteral("0.4.0"), junk);
        EditorInstaller installer(feed.root());
        installer.setPublicKey(feed.key.pub);
        const Outcome o = fetchAndInstall(installer, feed.url());
        EXPECT_FALSE(o.installed.has_value());
        EXPECT_TRUE(installer.installed().empty());
        EXPECT_FALSE(QFileInfo::exists(feed.root() + "/.staging-0.4.0"));
    }
}

TEST(LauncherInstall, UnsignedFeedsWorkWithoutAKey) {
    Feed feed;
    feed.publish(QStringLiteral("0.3.0"), editorPackage(), /*sign=*/false);
    EditorInstaller installer(feed.root());  // development build: no release key compiled in
    const Outcome o = fetchAndInstall(installer, feed.url(), /*install=*/false);
    ASSERT_TRUE(o.manifest.has_value()) << o.error.toStdString();
    EXPECT_EQ(o.manifest->releases.front().version, QStringLiteral("0.3.0"));
}

TEST(LauncherInstall, InstallsFromAPublishedFeed) {
    // End-to-end check of real release artifacts (package.py + manifest.py output), used by CI
    // before uploading: LUMORA_TEST_FEED=<manifest url or path> LUMORA_TEST_FEED_KEY=<base64>.
    const QString feedText = qEnvironmentVariable("LUMORA_TEST_FEED");
    if (feedText.isEmpty()) GTEST_SKIP() << "set LUMORA_TEST_FEED to check a published feed";
    const QUrl url = QUrl::fromUserInput(feedText, QDir::currentPath(), QUrl::AssumeLocalFile);
    const auto key = decodeBinary(qEnvironmentVariable("LUMORA_TEST_FEED_KEY"), 32);
    ASSERT_TRUE(key.has_value()) << "LUMORA_TEST_FEED_KEY must be the base64 public key";
    test::TempDir dir;
    // LUMORA_TEST_INSTALL_ROOT keeps installs between runs, to check updates (delta) as well.
    const QString root = qEnvironmentVariableIsSet("LUMORA_TEST_INSTALL_ROOT") ? qEnvironmentVariable("LUMORA_TEST_INSTALL_ROOT")
                                                                              : qpath(dir.path());
    EditorInstaller installer(root);
    installer.setPublicKey(*key);
    const Outcome o = fetchAndInstall(installer, url);
    ASSERT_TRUE(o.installed.has_value()) << o.error.toStdString();
    EXPECT_TRUE(QFileInfo(o.installed->executable).isExecutable());
    std::printf("installed Lumora %s into %s (%s)\n", qPrintable(o.installed->version), qPrintable(o.installed->directory),
                installer.lastInstallWasDelta() ? "delta update" : "full package");
}

// ---- Delta updates -----------------------------------------------------------------------------

namespace {

/// Publishes a release the way package.py + manifest.py do: ZIP, file list and blobs/<sha256>.
struct DeltaFeed {
    test::TempDir dir;
    KeyPair key;
    QString root() const { return qpath(dir.path() / "installs"); }
    QString blobs() const { return qpath(dir.path() / "feed" / "editor" / "blobs"); }
    QUrl url() const { return QUrl::fromLocalFile(qpath(dir.path() / "feed" / "editor" / "manifest.json")); }

    void publish(const QString& version, const std::map<std::string, std::pair<QByteArray, int>>& files) {
        ZipWriter zip;
        std::vector<FileEntry> entries;
        QDir().mkpath(blobs());
        for (const auto& [path, content] : files) {
            zip.file(path, content.first, 0100000u | static_cast<std::uint32_t>(content.second));
            FileEntry e;
            e.path = QString::fromStdString(path);
            e.size = static_cast<quint64>(content.first.size());
            e.sha256 = sha256Hex(content.first);
            e.mode = content.second;
            entries.push_back(e);
            ASSERT_TRUE(writeFile(blobs() + "/" + QString::fromLatin1(e.sha256), content.first));
        }
        zip.symlink("lib/libLumora.so", "libLumora.so.1");
        FileEntry link;
        link.path = QStringLiteral("lib/libLumora.so");
        link.link = QStringLiteral("libLumora.so.1");
        entries.push_back(link);
        const QString folder = qpath(dir.path() / "feed" / "editor") + "/" + version;
        QDir().mkpath(folder);
        ASSERT_TRUE(zip.save(folder + "/package.zip"));
        QFile zipFile(folder + "/package.zip");
        ASSERT_TRUE(zipFile.open(QIODevice::ReadOnly));
        const QByteArray zipBytes = zipFile.readAll();
        const QByteArray listing = fileListJson(entries);
        ASSERT_TRUE(writeFile(folder + "/files.json", listing));
        const QByteArray manifest =
            QStringLiteral(R"({"format":"lumora.releases","version":1,"blobs":"blobs/","releases":[
                {"version":"%1","channel":"stable","packages":{"%2":{"url":"%1/package.zip","size":%3,"sha256":"%4",
                 "files":{"url":"%1/files.json","size":%5,"sha256":"%6"}}}}]})")
                .arg(version, currentPlatform())
                .arg(zipBytes.size())
                .arg(QString::fromLatin1(sha256Hex(zipBytes)))
                .arg(listing.size())
                .arg(QString::fromLatin1(sha256Hex(listing)))
                .toUtf8();
        ASSERT_TRUE(writeFile(qpath(dir.path() / "feed" / "editor" / "manifest.json"), manifest));
        ASSERT_TRUE(writeFile(qpath(dir.path() / "feed" / "editor" / "manifest.json.sig"), key.sign(manifest).toBase64()));
    }
};

std::map<std::string, std::pair<QByteArray, int>> editorFiles(const QByteArray& editorModule) {
    return {{editorExecutable().toStdString(), {"#!/bin/sh\nexit 0\n", 0755}},
            {"lib/libLumora.so.1", {QByteArray(4096, 'c'), 0644}},
            {"lib/liblumora_renderer.so", {QByteArray(8192, 'r'), 0644}},
            {"lib/liblumora_editor.so", {editorModule, 0644}}};
}

} // namespace

TEST(LauncherDelta, UpdatesDownloadOnlyChangedFiles) {
    DeltaFeed feed;
    feed.publish(QStringLiteral("0.4.0"), editorFiles(QByteArray(1000, '4')));
    EditorInstaller installer(feed.root());
    installer.setPublicKey(feed.key.pub);
    Outcome first = fetchAndInstall(installer, feed.url());
    ASSERT_TRUE(first.installed.has_value()) << first.error.toStdString();
    EXPECT_FALSE(installer.lastInstallWasDelta()) << "nothing installed yet: full package";
    EXPECT_TRUE(QFileInfo::exists(first.installed->directory + "/" + kInstalledFileList));

    // 0.5.0 changes only the editor module. Every other blob disappears from the server, so the
    // update can only succeed by reusing the installed copies.
    feed.publish(QStringLiteral("0.5.0"), editorFiles(QByteArray(1200, '5')));
    QDir blobs(feed.blobs());
    const QByteArray changed = sha256Hex(QByteArray(1200, '5'));
    for (const QString& name : blobs.entryList(QDir::Files)) {
        if (name.toLatin1() != changed) blobs.remove(name);
    }
    QFile::remove(qpath(feed.dir.path() / "feed" / "editor" / "0.5.0" / "package.zip"));  // no fallback either
    Outcome second = fetchAndInstall(installer, feed.url());
    ASSERT_TRUE(second.installed.has_value()) << second.error.toStdString();
    EXPECT_TRUE(installer.lastInstallWasDelta());
    const QString dir = second.installed->directory;
    EXPECT_EQ(readFileBytes(dir + "/lib/liblumora_editor.so"), QByteArray(1200, '5'));
    EXPECT_EQ(readFileBytes(dir + "/lib/liblumora_renderer.so"), QByteArray(8192, 'r'));
#if !defined(Q_OS_WIN)
    EXPECT_TRUE(QFileInfo(second.installed->executable).isExecutable());
    EXPECT_TRUE(QFileInfo(dir + "/lib/libLumora.so").isSymLink());
#endif
    EXPECT_EQ(installer.installed().size(), 2u);
}

TEST(LauncherDelta, FallsBackToTheFullPackage) {
    DeltaFeed feed;
    feed.publish(QStringLiteral("0.4.0"), editorFiles(QByteArray(1000, '4')));
    EditorInstaller installer(feed.root());
    installer.setPublicKey(feed.key.pub);
    ASSERT_TRUE(fetchAndInstall(installer, feed.url()).installed.has_value());

    feed.publish(QStringLiteral("0.5.0"), editorFiles(QByteArray(1200, '5')));
    ASSERT_TRUE(QFile::remove(feed.blobs() + "/" + QString::fromLatin1(sha256Hex(QByteArray(1200, '5')))));
    Outcome o = fetchAndInstall(installer, feed.url());
    ASSERT_TRUE(o.installed.has_value()) << o.error.toStdString();
    EXPECT_FALSE(installer.lastInstallWasDelta()) << "a missing blob means the full package";
    EXPECT_EQ(readFileBytes(o.installed->directory + "/lib/liblumora_editor.so"), QByteArray(1200, '5'));
}
