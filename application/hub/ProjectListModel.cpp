#include "hub/ProjectListModel.hpp"

#include "common/ProjectUi.hpp"
#include "project/ProjectRegistry.hpp"

#include <QFileInfo>

namespace lumora::app {

ProjectListModel::ProjectListModel(ProjectRegistry& registry, QObject* parent)
    : QAbstractListModel(parent), registry_(registry) {
    reload();
}

void ProjectListModel::reload() {
    beginResetModel();
    entries_.clear();
    for (const RecentProject& p : registry_.entries()) {
        Entry e;
        e.descriptor = toQString(p.descriptor);
        e.root = toQString(p.descriptor.parent_path());
        e.name = p.name.empty() ? QFileInfo(e.descriptor).completeBaseName() : QString::fromStdString(p.name);
        e.lastOpened = p.lastOpenedUnix;
        e.exists = QFileInfo::exists(e.descriptor);
        entries_.push_back(std::move(e));
    }
    endResetModel();
}

int ProjectListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

QVariant ProjectListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const Entry& e = entries_[static_cast<std::size_t>(index.row())];
    switch (role) {
        case Qt::DisplayRole:
        case NameRole: return e.name;
        case Qt::ToolTipRole: return e.exists ? e.descriptor : tr("Missing: %1").arg(e.descriptor);
        case DescriptorRole: return e.descriptor;
        case RootRole: return e.root;
        case LastOpenedRole: return e.lastOpened;
        case ExistsRole: return e.exists;
        case ThumbnailRole:
            if (e.thumbnail.isNull()) e.thumbnail = projectThumbnail(e.root, e.name, kThumbnailSize);
            return e.thumbnail;
        default: return {};
    }
}

void ProjectFilterModel::setSort(Sort sort) {
    sort_ = sort;
    invalidate();
    this->sort(0, sort == Sort::Recent ? Qt::DescendingOrder : Qt::AscendingOrder);
}

void ProjectFilterModel::setSearch(const QString& text) {
    needle_ = text.trimmed();
    invalidateFilter();
}

bool ProjectFilterModel::filterAcceptsRow(int row, const QModelIndex& parent) const {
    const QString& needle = needle_;
    if (needle.isEmpty()) return true;
    const QModelIndex i = sourceModel()->index(row, 0, parent);
    return i.data(ProjectListModel::NameRole).toString().contains(needle, Qt::CaseInsensitive) ||
           i.data(ProjectListModel::DescriptorRole).toString().contains(needle, Qt::CaseInsensitive);
}

bool ProjectFilterModel::lessThan(const QModelIndex& a, const QModelIndex& b) const {
    if (sort_ == Sort::Recent) {
        return a.data(ProjectListModel::LastOpenedRole).toLongLong() < b.data(ProjectListModel::LastOpenedRole).toLongLong();
    }
    return QString::localeAwareCompare(a.data(ProjectListModel::NameRole).toString(),
                                       b.data(ProjectListModel::NameRole).toString()) < 0;
}

} // namespace lumora::app
