#pragma once

#include <QAbstractListModel>
#include <QPixmap>
#include <QSortFilterProxyModel>

#include <vector>

namespace lumora {
class ProjectRegistry;
}

namespace lumora::app {

/// Recent projects as a Qt model. Thumbnails are loaded lazily and cached.
class ProjectListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        NameRole = Qt::UserRole + 1, DescriptorRole, RootRole, LastOpenedRole, ExistsRole, ThumbnailRole,
        DescriptionRole, EngineVersionRole, CreatedRole
    };
    /// Index of the project with this descriptor, or an invalid index.
    [[nodiscard]] QModelIndex indexOf(const QString& descriptor) const;
    /// Drops the cached thumbnail (after the editor saved a new one).
    void invalidateThumbnails();

    explicit ProjectListModel(ProjectRegistry& registry, QObject* parent = nullptr);
    void reload();

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;

    static constexpr QSize kThumbnailSize{320, 180};

private:
    struct Entry {
        QString name;
        QString descriptor;
        QString root;
        qint64 lastOpened = 0;
        bool exists = false;
        QString description;
        QString engineVersion;
        qint64 created = 0;
        mutable QPixmap thumbnail;
    };
    ProjectRegistry& registry_;
    std::vector<Entry> entries_;
};

/// Case-insensitive search over name and path, sortable by recency or name.
class ProjectFilterModel final : public QSortFilterProxyModel {
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    enum class Sort { Recent, Name };
    void setSort(Sort sort);
    void setSearch(const QString& text);

protected:
    [[nodiscard]] bool filterAcceptsRow(int row, const QModelIndex& parent) const override;
    [[nodiscard]] bool lessThan(const QModelIndex& a, const QModelIndex& b) const override;

private:
    Sort sort_ = Sort::Recent;
    QString needle_;
};

} // namespace lumora::app
