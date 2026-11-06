#pragma once

#include "common/ModuleExport.hpp"

#include <QHash>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <QStringList>

#include <functional>

class QFileInfo;
class QTimer;

namespace lumora::editor {

/// Pictures for the Content Browser: previews of images, models, prefabs and materials (made
/// in the background, a few per frame, and kept in Saved/Thumbnails), and for every other file
/// a document icon labelled with its type ("C++", "GLB", "WAV"...).
class LUMORA_EDITOR_API ThumbnailCache final : public QObject {
    Q_OBJECT
public:
    /// Renders a preview of a model, prefab or material file (absolute path); null image if it
    /// cannot. Images are previewed without it.
    using Renderer = std::function<QImage(const QString& path)>;

    explicit ThumbnailCache(QString projectRoot, QObject* parent = nullptr);
    void setRenderer(Renderer renderer) { renderer_ = std::move(renderer); }

    /// The preview if it is ready, else the type icon (and the preview is queued).
    [[nodiscard]] QIcon icon(const QFileInfo& file);
    /// The file changed: make its preview again.
    void invalidate(const QString& path);
    [[nodiscard]] bool hasPreview(const QString& path) const { return ready_.contains(path); }
    [[nodiscard]] int pending() const noexcept { return static_cast<int>(queue_.size()); }
    /// Makes every queued preview now (tests).
    void processAll();

    /// A document icon with a coloured band naming the type; nullptr-free, cached per suffix.
    [[nodiscard]] static QIcon typeIcon(const QString& suffix);
    /// The label on it: "C++", "GLB", "SCENE", ...
    [[nodiscard]] static QString typeLabel(const QString& suffix);

signals:
    void previewReady(const QString& path);

private:
    void processOne();
    [[nodiscard]] QString cacheFile(const QString& path, qint64 stamp) const;
    [[nodiscard]] QImage makePreview(const QString& path);

    QString root_;
    Renderer renderer_;
    QHash<QString, QIcon> ready_;
    QHash<QString, qint64> stamps_;  // modification time the preview was made from
    QStringList queue_;
    QTimer* timer_ = nullptr;
};

} // namespace lumora::editor
