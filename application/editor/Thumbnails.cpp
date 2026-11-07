#include "editor/Thumbnails.hpp"

#include "assets/Image.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QPointer>
#include <QThreadPool>
#include <QCoreApplication>

namespace lumora::editor {

namespace {

constexpr int kPreviewSize = 128;

bool isImage(const QString& s) {
    return s == QLatin1String("png") || s == QLatin1String("jpg") || s == QLatin1String("jpeg") ||
           s == QLatin1String("bmp") || s == QLatin1String("tga") || s == QLatin1String("gif") || s == QLatin1String("webp") ||
           s == QLatin1String("ktx2");
}

/// A preview of an image file: Qt's readers, or the engine's decoders (WebP without Qt's plugin, KTX2).
QImage readPreview(const QString& path) {
    QImageReader reader(path);
    const QSize size = reader.size();
    if (size.isValid()) reader.setScaledSize(size.scaled(kPreviewSize, kPreviewSize, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (!image.isNull()) return image;
    auto decoded = loadImageFile(std::filesystem::path(path.toStdU16String()));
    if (!decoded) return {};
    const QImage full(decoded->rgba.data(), static_cast<int>(decoded->width), static_cast<int>(decoded->height),
                      static_cast<qsizetype>(decoded->width) * 4, QImage::Format_RGBA8888);
    return full.scaled(kPreviewSize, kPreviewSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);  // a copy
}
bool isRendered(const QString& s) {
    return s == QLatin1String("glb") || s == QLatin1String("gltf") || s == QLatin1String("fbx") || s == QLatin1String("obj") ||
           s == QLatin1String("lprefab") ||
           s == QLatin1String("lmat") || s == QLatin1String("lmgraph");
}

QColor typeColor(const QString& s) {
    if (s == QLatin1String("cpp") || s == QLatin1String("cc") || s == QLatin1String("cxx")) return {0x4f, 0x86, 0xe8};
    if (s == QLatin1String("h") || s == QLatin1String("hpp") || s == QLatin1String("inl")) return {0x9b, 0x6b, 0xe3};
    if (s == QLatin1String("glb") || s == QLatin1String("gltf") || s == QLatin1String("fbx") || s == QLatin1String("obj")) {
        return {0xe8, 0x8a, 0x3c};
    }
    if (isImage(s)) return {0x3f, 0xb6, 0x75};
    if (s == QLatin1String("hdr") || s == QLatin1String("exr")) return {0xe0, 0xb3, 0x3a};
    if (s == QLatin1String("wav") || s == QLatin1String("mp3") || s == QLatin1String("flac") || s == QLatin1String("ogg")) {
        return {0xe0, 0x5e, 0x9a};
    }
    if (s == QLatin1String("lscene")) return {0x2f, 0xb3, 0xb3};
    if (s == QLatin1String("lprefab")) return {0x3a, 0x9f, 0xd8};
    if (s == QLatin1String("lmat") || s == QLatin1String("lmgraph")) return {0xb5, 0x5c, 0xd6};
    if (s == QLatin1String("ltimeline")) return {0xe2, 0x6d, 0x4a};
    if (s == QLatin1String("json") || s == QLatin1String("lumora")) return {0xc9, 0xa2, 0x27};
    if (s == QLatin1String("hlsl") || s == QLatin1String("glsl")) return {0xd6, 0x4d, 0x4d};
    return {0x6c, 0x74, 0x82};
}

} // namespace

QString ThumbnailCache::typeLabel(const QString& suffix) {
    const QString s = suffix.toLower();
    if (s == QLatin1String("cpp") || s == QLatin1String("cc") || s == QLatin1String("cxx")) return QStringLiteral("C++");
    if (s == QLatin1String("hpp") || s == QLatin1String("h")) return QStringLiteral("H");
    if (s == QLatin1String("lscene")) return QStringLiteral("SCENE");
    if (s == QLatin1String("lprefab")) return QStringLiteral("PREFAB");
    if (s == QLatin1String("lmat")) return QStringLiteral("MAT");
    if (s == QLatin1String("ltimeline")) return QStringLiteral("TIME");
    if (s == QLatin1String("lumora")) return QStringLiteral("PROJ");
    if (s.isEmpty()) return QStringLiteral("FILE");
    return s.left(5).toUpper();
}

QIcon ThumbnailCache::typeIcon(const QString& suffix) {
    static QHash<QString, QIcon> cache;
    const QString s = suffix.toLower();
    if (const auto it = cache.constFind(s); it != cache.constEnd()) return *it;
    QIcon icon;
    for (const int size : {32, 64, 128}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal u = size / 64.0;
        // A sheet of paper with a folded corner...
        const QRectF sheet(12 * u, 4 * u, 40 * u, 56 * u);
        const qreal fold = 12 * u;
        QPainterPath page;
        page.moveTo(sheet.left() + 3 * u, sheet.top());
        page.lineTo(sheet.right() - fold, sheet.top());
        page.lineTo(sheet.right(), sheet.top() + fold);
        page.lineTo(sheet.right(), sheet.bottom() - 3 * u);
        page.quadTo(sheet.right(), sheet.bottom(), sheet.right() - 3 * u, sheet.bottom());
        page.lineTo(sheet.left() + 3 * u, sheet.bottom());
        page.quadTo(sheet.left(), sheet.bottom(), sheet.left(), sheet.bottom() - 3 * u);
        page.lineTo(sheet.left(), sheet.top() + 3 * u);
        page.quadTo(sheet.left(), sheet.top(), sheet.left() + 3 * u, sheet.top());
        p.fillPath(page, QColor(0xe9, 0xec, 0xf1));
        p.setPen(QPen(QColor(0x9a, 0xa2, 0xb0), 1.2 * u));
        p.drawPath(page);
        QPainterPath corner;
        corner.moveTo(sheet.right() - fold, sheet.top());
        corner.lineTo(sheet.right() - fold, sheet.top() + fold);
        corner.lineTo(sheet.right(), sheet.top() + fold);
        p.fillPath(corner, QColor(0xc4, 0xca, 0xd4));
        // ... a few text lines ...
        p.setPen(QPen(QColor(0xc8, 0xcd, 0xd6), 2.0 * u));
        for (int i = 0; i < 3; ++i) {
            const qreal y = sheet.top() + (12 + i * 6) * u;
            p.drawLine(QPointF(sheet.left() + 6 * u, y), QPointF(sheet.right() - (i == 0 ? 16 : 6) * u, y));
        }
        // ... and a coloured band with the type.
        const QRectF band(sheet.left() - 4 * u, sheet.top() + 30 * u, sheet.width() + 8 * u, 16 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(typeColor(s));
        p.drawRoundedRect(band, 3 * u, 3 * u);
        QFont font = p.font();
        font.setBold(true);
        const QString label = typeLabel(s);
        font.setPixelSize(std::max(6, static_cast<int>((label.size() > 4 ? 8.5 : 10.5) * u)));
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(band, Qt::AlignCenter, label);
        p.end();
        icon.addPixmap(pixmap);
    }
    cache.insert(s, icon);
    return icon;
}

namespace {
/// The form icon() keys previews by (QFileInfo::absoluteFilePath: "/" separators).
QString normalized(const QString& path) { return QFileInfo(path).absoluteFilePath(); }
} // namespace

ThumbnailCache::ThumbnailCache(QString projectRoot, QObject* parent)
    : QObject(parent), root_(QDir(projectRoot).absolutePath()) {
    timer_ = new QTimer(this);
    timer_->setInterval(30);  // one preview per tick keeps the editor responsive
    connect(timer_, &QTimer::timeout, this, [this] { processOne(false); });
}

QString ThumbnailCache::cacheFile(const QString& path, qint64 stamp) const {
    const QByteArray key = QDir(root_).relativeFilePath(path).toUtf8() + '|' + QByteArray::number(stamp);
    const QString name = QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
    return QDir(root_).filePath(QStringLiteral("Saved/Thumbnails/") + name + QStringLiteral(".png"));
}

QIcon ThumbnailCache::icon(const QFileInfo& file) {
    const QString suffix = file.suffix().toLower();
    const QString path = file.absoluteFilePath();
    if (!isImage(suffix) && !isRendered(suffix)) return typeIcon(suffix);
    const qint64 stamp = file.lastModified().toMSecsSinceEpoch();
    if (const auto it = ready_.constFind(path); it != ready_.constEnd() && stamps_.value(path) == stamp) return *it;
    // From an earlier session?
    const QString cached = cacheFile(path, stamp);
    if (QFileInfo::exists(cached)) {
        const QIcon icon(cached);
        ready_.insert(path, icon);
        stamps_.insert(path, stamp);
        return icon;
    }
    if (!queue_.contains(path) && !decoding_.contains(path) && (isImage(suffix) || renderer_)) {
        queue_.append(path);
        if (!timer_->isActive()) timer_->start();
    }
    return ready_.contains(path) ? ready_.value(path) : typeIcon(suffix);  // the old preview until the new one
}

bool ThumbnailCache::hasPreview(const QString& path) const { return ready_.contains(normalized(path)); }

void ThumbnailCache::invalidate(const QString& file) {
    const QString path = normalized(file);
    stamps_.remove(path);
    if (!queue_.contains(path) && ready_.contains(path)) {
        queue_.append(path);
        if (!timer_->isActive()) timer_->start();
    }
}

QImage ThumbnailCache::makePreview(const QString& path) {
    const QFileInfo info(path);
    if (isImage(info.suffix().toLower())) return readPreview(path);
    return renderer_ ? renderer_(path) : QImage();
}

void ThumbnailCache::processOne(bool wait) {
    if (queue_.isEmpty()) {
        timer_->stop();
        return;
    }
    const QString path = queue_.takeFirst();
    const QFileInfo info(path);
    if (!info.exists()) return;
    const qint64 stamp = info.lastModified().toMSecsSinceEpoch();
    if (isImage(info.suffix().toLower()) && !wait) {
        // Images decode on the thread pool; the result comes back to this thread.
        decoding_.insert(path);
        QPointer<ThumbnailCache> self(this);
        QThreadPool::globalInstance()->start([self, path, stamp] {
            const QImage image = readPreview(path);
            if (self.isNull()) return;
            QMetaObject::invokeMethod(self.data(), [self, path, stamp, image] {
                if (self) self->finish(path, stamp, image);
            }, Qt::QueuedConnection);
        });
        return;
    }
    finish(path, stamp, makePreview(path));  // models and materials: the renderer lives on this thread
}

void ThumbnailCache::finish(const QString& path, qint64 stamp, const QImage& image) {
    decoding_.remove(path);
    const QFileInfo info(path);
    if (image.isNull()) {
        stamps_.insert(path, stamp);  // do not try again until it changes
        ready_.insert(path, typeIcon(info.suffix()));
        return;
    }
    const QString cached = cacheFile(path, stamp);
    QDir().mkpath(QFileInfo(cached).absolutePath());
    image.save(cached);
    ready_.insert(path, QIcon(QPixmap::fromImage(image)));
    stamps_.insert(path, stamp);
    emit previewReady(path);
}

void ThumbnailCache::processAll() {
    while (!queue_.isEmpty()) processOne(true);
    while (!decoding_.isEmpty()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

} // namespace lumora::editor
