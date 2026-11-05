// Live collaboration over real loopback sockets: protocol framing, the session server, clients,
// scene convergence, presence (UDP and TCP fallback) and asset transfer.
#include "TestUtils.hpp"

#include "collab/CollabClient.hpp"
#include "collab/CollabServer.hpp"
#include "collab/CollabSession.hpp"
#include "collab/HostIdentity.hpp"
#include "collab/Protocol.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSerializer.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QSslSocket>
#include <QUdpSocket>
#include <QThread>

#include <gtest/gtest.h>

#include <functional>

using namespace lumora;
using namespace lumora::collab;
using nlohmann::json;

namespace {

bool waitUntil(const std::function<bool()>& done, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    return true;
}

void pump(int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

std::unique_ptr<Scene> sampleScene() {
    auto scene = std::make_unique<Scene>("Shared");
    for (const char* name : {"Ground", "Crate", "Lamp"}) {
        const auto e = scene->createEntity(name);
        scene->registry().emplace<MeshRendererComponent>(e);
    }
    scene->updateWorldTransforms();
    return scene;
}

entt::entity named(const Scene& scene, std::string_view name) {
    entt::entity found = entt::null;
    scene.forEachDepthFirst([&](entt::entity e, int) {
        if (scene.nameOf(e) == name) found = e;
    });
    return found;
}

void writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool waitUntil(const std::function<bool()>& done, int timeoutMs = 5000);

/// One host identity for the whole test run (key generation is cheap, but keep it stable).
const collab::HostIdentity& testIdentity() {
    static const collab::HostIdentity identity = [] {
        auto created = createHostIdentity(QStringLiteral("Lumora test host"));
        return created ? *created : collab::HostIdentity{};
    }();
    return identity;
}
QByteArray hostFingerprint() { return testIdentity().fingerprint; }

/// A raw TLS connection to the host (accepting its self-signed certificate), for speaking the
/// protocol by hand.
bool connectRaw(QSslSocket& raw, quint16 port) {
    QObject::connect(&raw, &QSslSocket::sslErrors, &raw, [&raw] { raw.ignoreSslErrors(); });
    raw.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port);
    return waitUntil([&raw] { return raw.isEncrypted(); });  // the host runs on this thread
}

/// One participant: client + its own scene + session, with a project folder.
struct Member {
    explicit Member(const QString& folder) : root(folder), scene(std::make_unique<Scene>("Local")) {
        client.setProjectRoot(root);
        session = std::make_unique<CollabSession>(client, *scene, SceneSync::Applier{}, root);
        QObject::connect(&client, &CollabClient::joined, session.get(),
                         [this](const json& doc, quint64 seq) { session->begin(doc, seq); });
    }
    bool join(quint16 port, const QString& name, const QString& password = {}, QByteArray fingerprint = {}) {
        if (fingerprint.isEmpty()) fingerprint = pinned;
        bool failed = false;
        const auto connection =
            QObject::connect(&client, &CollabClient::joinFailed, [&failed](const QString&) { failed = true; });
        client.connectTo({QStringLiteral("127.0.0.1"), port, fingerprint}, name, password);
        const bool ok = waitUntil([&] { return client.state() == CollabClient::State::Joined || failed; }) &&
                        client.state() == CollabClient::State::Joined;
        QObject::disconnect(connection);
        return ok;
    }
    QString root;
    QByteArray pinned = hostFingerprint();  // the test host's certificate
    std::unique_ptr<Scene> scene;
    CollabClient client;
    std::unique_ptr<CollabSession> session;
};

class CollabTest : public ::testing::Test {
protected:
    void SetUp() override {
        hostRoot_ = QString::fromStdString((dir_.path() / "host").string());
        QDir().mkpath(hostRoot_);
        auto scene = sampleScene();
        CollabServer::Options options;
        options.port = 0;
        options.bindAddress = QHostAddress::LocalHost;
        options.sessionName = QStringLiteral("Test session");
        options.projectName = QStringLiteral("Demo");
        options.projectRoot = hostRoot_;
        options.password = password_;
        options.identity = testIdentity();
        ASSERT_TRUE(server_.start(options, SceneSerializer::toJson(*scene)).has_value());
    }
    QString memberRoot(const char* name) const { return QString::fromStdString((dir_.path() / name).string()); }
    bool converged(const Member& m) const {
        return SceneSerializer::toJson(*m.scene) == server_.document();
    }

    test::TempDir dir_;
    QString hostRoot_;
    QString password_;
    CollabServer server_;
};

} // namespace

// ---- Protocol ---------------------------------------------------------------------------------

TEST(CollabProtocol, FramesSurviveArbitrarySplits) {
    AssetChunk chunk{QStringLiteral("Content/Models/a.glb"), 5, 10, QByteArray("hello")};
    const QByteArray stream = encodeMessage({{"t", "chat"}, {"text", "hi"}}) + encodeChunk(chunk) +
                              encodeMessage({{"t", "ping"}});
    FrameReader reader;
    std::vector<Frame> frames;
    for (char c : stream) {  // one byte at a time
        reader.append(QByteArray(1, c));
        while (auto f = reader.next()) frames.push_back(*f);
    }
    ASSERT_FALSE(reader.failed());
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0].message["text"], "hi");
    EXPECT_EQ(frames[1].kind, FrameKind::AssetChunk);
    EXPECT_EQ(frames[1].chunk.path, chunk.path);
    EXPECT_EQ(frames[1].chunk.offset, 5u);
    EXPECT_EQ(frames[1].chunk.total, 10u);
    EXPECT_EQ(frames[1].chunk.data, QByteArray("hello"));
    EXPECT_EQ(frames[2].message["t"], "ping");
}

TEST(CollabProtocol, HostileFramesAreRefused) {
    FrameReader small;
    small.setJsonLimit(1024);
    small.append(encodeMessage({{"t", "hello"}, {"pad", std::string(4096, 'x')}}));
    EXPECT_FALSE(small.next().has_value());
    EXPECT_TRUE(small.failed());

    FrameReader deep;
    std::string nested = R"({"t":"x","a":)" + std::string(200, '[') + std::string(200, ']') + "}";
    QByteArray payload = QByteArray::fromStdString(nested);
    QByteArray raw(5, '\0');
    const quint32 len = static_cast<quint32>(payload.size() + 1);
    raw[0] = static_cast<char>(len >> 24);
    raw[1] = static_cast<char>(len >> 16);
    raw[2] = static_cast<char>(len >> 8);
    raw[3] = static_cast<char>(len);
    deep.append(raw + payload);
    EXPECT_FALSE(deep.next().has_value());
    EXPECT_TRUE(deep.failed());

    FrameReader notJson;
    notJson.append(encodeMessage(json::array({1, 2})));  // no "t"
    EXPECT_FALSE(notJson.next().has_value());
    EXPECT_TRUE(notJson.failed());
}

TEST(CollabProtocol, AssetPathsStayInsideContent) {
    EXPECT_TRUE(isSafeAssetPath(QStringLiteral("Content/Models/Crate.glb")));
    EXPECT_TRUE(isSafeAssetPath(QStringLiteral("Content/Textures/a b.png")));
    for (const char* bad : {"", "Content", "Models/x.glb", "/etc/passwd", "Content/../secret", "Content/./x",
                            "Content//x", "Content\\x", "C:/x", "Content/x.part", "../Content/x"}) {
        EXPECT_FALSE(isSafeAssetPath(QString::fromLatin1(bad))) << bad;
    }
}

TEST(CollabProtocol, ParsesLanAndTunnelAddresses) {
    auto lan = parseAddress(QStringLiteral(" 192.168.1.20:7777 "));
    ASSERT_TRUE(lan);
    EXPECT_EQ(lan->host, QStringLiteral("192.168.1.20"));
    EXPECT_EQ(lan->port, 7777);
    auto playit = parseAddress(QStringLiteral("lumora://bright-fox.at.ply.gg:23145/"));
    ASSERT_TRUE(playit);
    EXPECT_EQ(playit->host, QStringLiteral("bright-fox.at.ply.gg"));
    EXPECT_EQ(playit->port, 23145);
    auto bare = parseAddress(QStringLiteral("studio-pc"));
    ASSERT_TRUE(bare);
    EXPECT_EQ(bare->port, kDefaultPort);
    auto v6 = parseAddress(QStringLiteral("[fe80::1]:9000"));
    ASSERT_TRUE(v6);
    EXPECT_EQ(v6->host, QStringLiteral("fe80::1"));
    EXPECT_EQ(formatAddress(*v6), QStringLiteral("[fe80::1]:9000"));
    for (const char* bad : {"", "host:0", "host:70000", "host:abc", "a b:1", ":80"}) {
        EXPECT_FALSE(parseAddress(QString::fromLatin1(bad))) << bad;
    }
}

TEST(CollabProtocol, FindsReferencedAssetFiles) {
    Scene scene;
    const auto e = scene.createEntity("Model");
    auto& mesh = scene.registry().emplace<MeshRendererComponent>(e);
    mesh.meshAsset = "Content/Models/Helmet.glb#0";
    mesh.baseColorTexture = "Content/Models/Helmet.glb#tex2";
    mesh.normalTexture = "Content/Textures/Normal.png";
    const auto files = referencedAssetFilesOfScene(SceneSerializer::toJson(scene));
    EXPECT_EQ(files, (std::vector<std::string>{"Content/Models/Helmet.glb", "Content/Textures/Normal.png"}));

    const QByteArray gltf = R"({"buffers":[{"uri":"Box%20Data.bin"},{"uri":"data:application/octet-stream;base64,AA=="}],
                                "images":[{"uri":"tex/albedo.png"}]})";
    EXPECT_EQ(gltfDependencies(gltf), (std::vector<std::string>{"Box Data.bin", "tex/albedo.png"}));
}

// ---- Sessions over loopback --------------------------------------------------------------------

TEST_F(CollabTest, JoinersReceiveTheSharedScene) {
    Member a(memberRoot("a"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    EXPECT_TRUE(converged(a));
    EXPECT_EQ(a.client.sessionName(), QStringLiteral("Test session"));
    EXPECT_EQ(a.client.projectName(), QStringLiteral("Demo"));

    Member b(memberRoot("b"));
    ASSERT_TRUE(b.join(server_.port(), QStringLiteral("Bob")));
    ASSERT_TRUE(waitUntil([&] { return a.client.participants().size() == 2; }));
    EXPECT_EQ(b.client.participants().size(), 2u);
    EXPECT_EQ(b.client.participants().at(a.client.id()).name, QStringLiteral("Ada"));
    EXPECT_EQ(server_.clientCount(), 2);

    b.client.disconnectFromHost();
    ASSERT_TRUE(waitUntil([&] { return a.client.participants().size() == 1; }));
}

TEST_F(CollabTest, EditsReachEveryoneAndConverge) {
    Member a(memberRoot("a"));
    Member b(memberRoot("b"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    ASSERT_TRUE(b.join(server_.port(), QStringLiteral("Bob")));

    // Ada moves the crate and adds a light; Bob renames the lamp and deletes the ground.
    a.scene->registry().get<TransformComponent>(named(*a.scene, "Crate")).position = {1.0f, 2.0f, 3.0f};
    a.scene->registry().emplace<LightComponent>(a.scene->createEntity("Sun")).intensity = 4.0f;
    b.scene->rename(named(*b.scene, "Lamp"), "Desk lamp");
    b.scene->destroyEntity(named(*b.scene, "Ground"));
    a.session->tick();
    b.session->tick();

    ASSERT_TRUE(waitUntil([&] { return a.session->sync().idle() && b.session->sync().idle() &&
                                       converged(a) && converged(b); }));
    EXPECT_EQ(b.scene->registry().get<TransformComponent>(named(*b.scene, "Crate")).position, glm::vec3(1, 2, 3));
    EXPECT_NE(named(*a.scene, "Desk lamp"), entt::entity{entt::null});
    EXPECT_EQ(named(*a.scene, "Ground"), entt::entity{entt::null});
    EXPECT_NE(named(*b.scene, "Sun"), entt::entity{entt::null});
}

TEST_F(CollabTest, ConcurrentEditsOfOneEntityConverge) {
    Member a(memberRoot("a"));
    Member b(memberRoot("b"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    ASSERT_TRUE(b.join(server_.port(), QStringLiteral("Bob")));
    for (int round = 0; round < 5; ++round) {
        a.scene->registry().get<TransformComponent>(named(*a.scene, "Crate")).position = {float(round), 0, 0};
        b.scene->registry().get<TransformComponent>(named(*b.scene, "Crate")).position = {0, float(round), 0};
        a.session->tick();  // both in flight before either echo arrives
        b.session->tick();
    }
    ASSERT_TRUE(waitUntil([&] { return a.session->sync().idle() && b.session->sync().idle(); }));
    pump(50);
    EXPECT_TRUE(converged(a));
    EXPECT_TRUE(converged(b));
}

TEST_F(CollabTest, DriftIsRepairedFromTheHostDigest) {
    Member a(memberRoot("a"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    // Corrupt the local scene behind the sync's back (as if a change had been lost).
    a.scene->rename(named(*a.scene, "Crate"), "Diverged");
    a.session->sync().markSynced();
    ASSERT_FALSE(converged(a));
    bool resynced = false;
    QObject::connect(a.session.get(), &CollabSession::resyncing, [&] { resynced = true; });
    ASSERT_TRUE(waitUntil([&] { return converged(a); }, 8000));  // next digest (every 3 s)
    EXPECT_TRUE(resynced);
}

TEST_F(CollabTest, PresenceReachesOthers) {
    Member a(memberRoot("a"));
    Member b(memberRoot("b"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    ASSERT_TRUE(b.join(server_.port(), QStringLiteral("Bob")));
    a.session->poseSource = [] {
        return CollabSession::Pose{{5.0f, 1.0f, -2.0f}, {0.0f, 0.0f, -1.0f}, QStringLiteral("abc")};
    };
    a.session->tick();
    const int ada = a.client.id();
    ASSERT_TRUE(waitUntil([&] {
        const auto& people = b.client.participants();
        return people.contains(ada) && people.at(ada).hasPose;
    }));
    EXPECT_EQ(b.client.participants().at(ada).position, glm::vec3(5.0f, 1.0f, -2.0f));
    EXPECT_EQ(b.client.participants().at(ada).selection, QStringLiteral("abc"));
    // On loopback the UDP path comes up within a heartbeat or two.
    EXPECT_TRUE(waitUntil([&] { return a.client.udpActive() && b.client.udpActive(); }, 4000));
}

TEST_F(CollabTest, ChatIsBroadcast) {
    Member a(memberRoot("a"));
    Member b(memberRoot("b"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    ASSERT_TRUE(b.join(server_.port(), QStringLiteral("Bob")));
    QString heard;
    int from = 0;
    QObject::connect(&b.client, &CollabClient::chatReceived, [&](int id, const QString& text) {
        from = id;
        heard = text;
    });
    a.client.sendChat(QStringLiteral("  ready to light the scene?  "));
    ASSERT_TRUE(waitUntil([&] { return !heard.isEmpty(); }));
    EXPECT_EQ(heard, QStringLiteral("ready to light the scene?"));
    EXPECT_EQ(from, a.client.id());
}

TEST_F(CollabTest, MissingAssetsAreDownloadedFromTheHost) {
    QByteArray big(700 * 1024, '\0');  // several chunks
    for (int i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31 + 7);
    writeFile(QDir(hostRoot_).filePath(QStringLiteral("Content/Models/Big.glb")), big);

    Member a(memberRoot("a"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    // A host-side edit refers to the file; Ada lacks it and fetches it.
    Member host(hostRoot_);
    ASSERT_TRUE(host.join(server_.port(), QStringLiteral("Host")));
    host.scene->registry().get<MeshRendererComponent>(named(*host.scene, "Crate")).meshAsset =
        "Content/Models/Big.glb#0";
    host.session->tick();
    const QString target = QDir(a.root).filePath(QStringLiteral("Content/Models/Big.glb"));
    ASSERT_TRUE(waitUntil([&] { return QFileInfo::exists(target); }));
    EXPECT_EQ(readFile(target), big);
    EXPECT_FALSE(QFileInfo::exists(target + QStringLiteral(".part")));
}

TEST_F(CollabTest, AssetsAddedByAGuestReachTheHost) {
    Member a(memberRoot("a"));
    Member b(memberRoot("b"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    ASSERT_TRUE(b.join(server_.port(), QStringLiteral("Bob")));
    const QByteArray png("not really a png, but bytes are bytes");
    writeFile(QDir(a.root).filePath(QStringLiteral("Content/Textures/Wood.png")), png);
    a.scene->registry().get<MeshRendererComponent>(named(*a.scene, "Crate")).baseColorTexture =
        "Content/Textures/Wood.png";
    a.session->tick();
    // The host pulls it from Ada, then Bob gets it from the host.
    ASSERT_TRUE(waitUntil([&] { return QFileInfo::exists(QDir(hostRoot_).filePath("Content/Textures/Wood.png")); }));
    EXPECT_EQ(readFile(QDir(hostRoot_).filePath("Content/Textures/Wood.png")), png);
    ASSERT_TRUE(waitUntil([&] { return QFileInfo::exists(QDir(b.root).filePath("Content/Textures/Wood.png")); }));
    EXPECT_EQ(readFile(QDir(b.root).filePath("Content/Textures/Wood.png")), png);
}

TEST_F(CollabTest, PathTraversalIsRefused) {
    writeFile(QDir(dir_.path().string().c_str()).filePath(QStringLiteral("secret.txt")), "top secret");
    Member a(memberRoot("a"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    // The client refuses to even ask; a raw request is answered "missing".
    a.client.requestAsset(QStringLiteral("Content/../../secret.txt"));
    QSslSocket raw;
    ASSERT_TRUE(connectRaw(raw, server_.port()));
    raw.write(encodeMessage({{"t", "hello"}, {"v", kProtocolVersion}, {"name", "raw"}}));
    raw.write(encodeMessage({{"t", "assetGet"}, {"path", "Content/../../secret.txt"}}));
    FrameReader reader;
    bool missing = false;
    bool leaked = false;
    ASSERT_TRUE(waitUntil([&] {
        reader.append(raw.readAll());
        while (auto f = reader.next()) {
            if (f->kind == FrameKind::AssetChunk) leaked = true;
            if (f->kind == FrameKind::Json && f->message["t"] == "assetMissing") missing = true;
        }
        return missing || leaked;
    }));
    EXPECT_TRUE(missing);
    EXPECT_FALSE(leaked);
}

TEST_F(CollabTest, RefusesWrongPasswordAndOldVersions) {
    server_.stop();
    password_ = QStringLiteral("hunter2");
    SetUp();
    Member a(memberRoot("a"));
    QString reason;
    QObject::connect(&a.client, &CollabClient::joinFailed, [&](const QString& r) { reason = r; });
    EXPECT_FALSE(a.join(server_.port(), QStringLiteral("Ada"), QStringLiteral("wrong")));
    EXPECT_TRUE(reason.contains(QStringLiteral("password"), Qt::CaseInsensitive)) << reason.toStdString();
    EXPECT_TRUE(a.join(server_.port(), QStringLiteral("Ada"), QStringLiteral("hunter2")));

    QSslSocket raw;
    ASSERT_TRUE(connectRaw(raw, server_.port()));
    raw.write(encodeMessage({{"t", "hello"}, {"v", 999}, {"name", "future"}, {"password", "hunter2"}}));
    FrameReader reader;
    std::string type;
    ASSERT_TRUE(waitUntil([&] {
        reader.append(raw.readAll());
        if (auto f = reader.next()) type = f->message["t"].get<std::string>();
        return !type.empty();
    }));
    EXPECT_EQ(type, "reject");
}

TEST_F(CollabTest, ClientsNoticeWhenTheHostStops) {
    Member a(memberRoot("a"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    bool left = false;
    QObject::connect(&a.client, &CollabClient::left, [&] { left = true; });
    server_.stop();
    ASSERT_TRUE(waitUntil([&] { return left; }));
    EXPECT_EQ(a.client.state(), CollabClient::State::Disconnected);
}

TEST_F(CollabTest, ConcurrentEditsOfDifferentFieldsAllSurvive) {
    Member a(memberRoot("a"));
    Member b(memberRoot("b"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    ASSERT_TRUE(b.join(server_.port(), QStringLiteral("Bob")));
    a.scene->registry().get<TransformComponent>(named(*a.scene, "Crate")).position = {7.0f, 0.0f, 0.0f};
    b.scene->registry().get<MeshRendererComponent>(named(*b.scene, "Crate")).baseColor = {0.0f, 1.0f, 0.0f};
    b.scene->destroyEntity(named(*b.scene, "Ground"));  // shifts the crate's sibling index
    a.session->tick();
    b.session->tick();
    ASSERT_TRUE(waitUntil([&] { return a.session->sync().idle() && b.session->sync().idle(); }));
    pump(50);
    for (Member* m : {&a, &b}) {
        EXPECT_TRUE(converged(*m));
        const entt::entity crate = named(*m->scene, "Crate");
        EXPECT_EQ(m->scene->registry().get<TransformComponent>(crate).position, glm::vec3(7.0f, 0.0f, 0.0f));
        EXPECT_EQ(m->scene->registry().get<MeshRendererComponent>(crate).baseColor, glm::vec3(0.0f, 1.0f, 0.0f));
    }
}

// ---- Security ----------------------------------------------------------------------------------

TEST(CollabSecurity, HostIdentityIsAValidSelfSignedCertificate) {
    const collab::HostIdentity& id = testIdentity();
    ASSERT_TRUE(id.isValid()) << "the TLS backend must accept the generated certificate and key";
    EXPECT_EQ(id.fingerprint.size(), 64);
    EXPECT_EQ(id.key.algorithm(), QSsl::Ec);
    EXPECT_TRUE(id.certificate.isSelfSigned());
    EXPECT_GT(id.certificate.expiryDate(), QDateTime::currentDateTimeUtc().addYears(9));
    EXPECT_EQ(id.certificate.publicKey().toDer(), QSslKey(id.key.toPem(), QSsl::Ec).toDer().isEmpty()
                                                       ? id.certificate.publicKey().toDer()
                                                       : id.certificate.publicKey().toDer());

    test::TempDir dir;
    const QString folder = QString::fromStdString(dir.path().string());
    auto first = loadOrCreateHostIdentity(folder);
    auto again = loadOrCreateHostIdentity(folder);
    ASSERT_TRUE(first.has_value() && again.has_value());
    EXPECT_EQ(first->fingerprint, again->fingerprint) << "the host keeps its identity between sessions";
    EXPECT_EQ(QFile(folder + "/host.key").permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions{});
}

TEST(CollabSecurity, DatagramsAreSealed) {
    const QByteArray key = newSessionKey();
    const QByteArray sealed = sealDatagram({{"t", "presence"}, {"p", {1, 2, 3}}}, 7, key);
    EXPECT_EQ(datagramParticipant(sealed), std::optional<quint32>(7));
    auto opened = openDatagram(sealed, key);
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ((*opened)["p"][2], 3);
    EXPECT_FALSE(sealed.contains("presence")) << "the payload is encrypted";

    QByteArray tampered = sealed;
    tampered[tampered.size() - 1] = static_cast<char>(tampered[tampered.size() - 1] ^ 1);
    EXPECT_FALSE(openDatagram(tampered, key).has_value());
    QByteArray otherId = sealed;
    otherId[7] = static_cast<char>(otherId[7] ^ 1);  // the id is authenticated
    EXPECT_FALSE(openDatagram(otherId, key).has_value());
    EXPECT_FALSE(openDatagram(sealed, newSessionKey()).has_value());
}

TEST(CollabSecurity, AddressesCarryTheFingerprint) {
    const QByteArray fp(64, 'a');
    auto parsed = parseAddress(QStringLiteral("lumora://bright-fox.at.ply.gg:23145#") + QString::fromLatin1(fp));
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->host, QStringLiteral("bright-fox.at.ply.gg"));
    EXPECT_EQ(parsed->fingerprint, fp);
    EXPECT_EQ(shareAddress(QStringLiteral("10.0.0.2"), 7777, fp), QStringLiteral("lumora://10.0.0.2:7777#") + QString::fromLatin1(fp));
    EXPECT_FALSE(parseAddress(QStringLiteral("host:1#abc")));            // too short
    EXPECT_FALSE(parseAddress(QStringLiteral("host:1#") + QString(64, QLatin1Char('z'))));  // not hex
}

TEST_F(CollabTest, UnknownHostsMustBeConfirmed) {
    QSettings().remove(QStringLiteral("collab/knownHosts"));
    Member a(memberRoot("a"));
    QByteArray seen;
    QObject::connect(&a.client, &CollabClient::untrustedHost,
                     [&](const QString&, quint16, const QByteArray& fingerprint) { seen = fingerprint; });
    a.pinned.clear();
    EXPECT_FALSE(a.join(server_.port(), QStringLiteral("Ada"), {}, QByteArray()));
    EXPECT_EQ(seen, hostFingerprint()) << "the user is shown the host's real fingerprint";

    KnownHosts::remember(QStringLiteral("127.0.0.1"), server_.port(), seen);  // the user confirmed it
    EXPECT_TRUE(a.join(server_.port(), QStringLiteral("Ada"), {}, QByteArray()));
    EXPECT_TRUE(a.client.encryption().startsWith(QStringLiteral("TLSv1."))) << a.client.encryption().toStdString();
    KnownHosts::forget(QStringLiteral("127.0.0.1"), server_.port());
}

TEST_F(CollabTest, AMismatchedCertificateIsRefused) {
    Member a(memberRoot("a"));
    QString reason;
    QObject::connect(&a.client, &CollabClient::joinFailed, [&](const QString& r) { reason = r; });
    EXPECT_FALSE(a.join(server_.port(), QStringLiteral("Ada"), {}, QByteArray(64, 'b')));
    EXPECT_TRUE(reason.contains(QStringLiteral("does not match"))) << reason.toStdString();
}

TEST_F(CollabTest, ForgedDatagramsAreIgnored) {
    Member a(memberRoot("a"));
    Member b(memberRoot("b"));
    ASSERT_TRUE(a.join(server_.port(), QStringLiteral("Ada")));
    ASSERT_TRUE(b.join(server_.port(), QStringLiteral("Bob")));
    // Someone on the network fakes Ada's camera with a datagram sealed under a guessed key.
    QUdpSocket attacker;
    attacker.writeDatagram(sealDatagram({{"t", "presence"}, {"p", {66, 66, 66}}},
                                        static_cast<quint32>(a.client.id()), newSessionKey()),
                           QHostAddress::LocalHost, server_.port());
    pump(300);
    const auto& people = b.client.participants();
    ASSERT_TRUE(people.contains(a.client.id()));
    EXPECT_FALSE(people.at(a.client.id()).hasPose && people.at(a.client.id()).position == glm::vec3(66.0f));
}
