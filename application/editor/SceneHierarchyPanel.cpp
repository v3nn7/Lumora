#include "editor/SceneHierarchyPanel.hpp"

#include "common/Theme.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EntityFactory.hpp"
#include "scene/Scene.hpp"

#include <QDropEvent>
#include <QMimeData>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

namespace lumora::editor {

namespace {
constexpr int kUuidRole = Qt::UserRole + 1;

Uuid itemUuid(const QTreeWidgetItem* item) {
    return item ? Uuid(item->data(0, kUuidRole).toULongLong()) : Uuid{};
}
} // namespace

namespace {
bool externalFiles(const QDropEvent* event, const QWidget* self) {
    return event->source() != self && event->mimeData() != nullptr && event->mimeData()->hasUrls();
}
} // namespace

void HierarchyTree::dragEnterEvent(QDragEnterEvent* event) {
    if (externalFiles(event, this)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    QTreeWidget::dragEnterEvent(event);
}

void HierarchyTree::dragMoveEvent(QDragMoveEvent* event) {
    if (externalFiles(event, this)) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    QTreeWidget::dragMoveEvent(event);
}

void HierarchyTree::dropEvent(QDropEvent* event) {
    if (externalFiles(event, this)) {
        QStringList files;
        for (const QUrl& url : event->mimeData()->urls()) {
            if (url.isLocalFile()) files << url.toLocalFile();
        }
        event->setDropAction(Qt::CopyAction);
        event->accept();
        emit filesDropped(itemUuid(itemAt(event->position().toPoint())), files);
        return;
    }
    QTreeWidgetItem* dragged = currentItem();
    if (dragged == nullptr) {
        event->ignore();
        return;
    }
    QTreeWidgetItem* target = itemAt(event->position().toPoint());
    Uuid parent;
    int index = -1;
    switch (dropIndicatorPosition()) {
        case QAbstractItemView::OnItem:
            parent = itemUuid(target);
            break;
        case QAbstractItemView::AboveItem:
        case QAbstractItemView::BelowItem: {
            QTreeWidgetItem* p = target->parent();
            parent = itemUuid(p);
            index = p ? p->indexOfChild(target) : indexOfTopLevelItem(target);
            if (dropIndicatorPosition() == QAbstractItemView::BelowItem) ++index;
            break;
        }
        case QAbstractItemView::OnViewport:
            break;
    }
    // We apply the move to the scene ourselves; stop Qt from rearranging items.
    event->setDropAction(Qt::IgnoreAction);
    event->accept();
    emit reparentRequested(itemUuid(dragged), parent, index);
}

SceneHierarchyPanel::SceneHierarchyPanel(EditorContext& ctx, QWidget* parent) : QWidget(parent), ctx_(ctx) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* top = new QHBoxLayout;
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Search entities"));
    filter_->addAction(app::theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    filter_->setClearButtonEnabled(true);
    auto* add = new QToolButton;
    add->setIcon(app::theme::icon(QStringLiteral("plus")));
    add->setToolTip(tr("Create entity"));
    add->setPopupMode(QToolButton::InstantPopup);
    auto* addMenu = new QMenu(add);
    populateCreateMenu(*addMenu, ctx_, false);
    add->setMenu(addMenu);
    top->addWidget(filter_, 1);
    top->addWidget(add);
    layout->addLayout(top);

    tree_ = new HierarchyTree;
    tree_->setHeaderHidden(true);
    tree_->setColumnCount(1);
    tree_->setIndentation(14);
    tree_->setIconSize(QSize(15, 15));
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);  // Ctrl/Shift+click
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree_->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setUniformRowHeights(true);
    layout->addWidget(tree_, 1);

    connect(filter_, &QLineEdit::textChanged, this, &SceneHierarchyPanel::applyFilter);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        if (rebuilding_) return;
        // The clicked (current) item is the primary selection: it goes last.
        std::vector<Uuid> ids;
        QTreeWidgetItem* current = tree_->currentItem();
        for (QTreeWidgetItem* item : tree_->selectedItems()) {
            if (item != current) ids.push_back(itemUuid(item));
        }
        if (current != nullptr && current->isSelected()) ids.push_back(itemUuid(current));
        ctx_.setSelection(std::move(ids));
    });
    connect(tree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int) {
        if (!rebuilding_) ctx_.renameEntity(itemUuid(item), item->text(0));
    });
    connect(tree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (!rebuilding_) collapsed_.insert(itemUuid(item));
    });
    connect(tree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (!rebuilding_) collapsed_.erase(itemUuid(item));
    });
    connect(tree_, &HierarchyTree::reparentRequested, this,
            [this](Uuid child, Uuid parentId, int index) { ctx_.reparent(child, parentId, index); });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &SceneHierarchyPanel::showContextMenu);
    connect(tree_, &HierarchyTree::filesDropped, this, &SceneHierarchyPanel::filesDropped);

    connect(&ctx_, &EditorContext::sceneReplaced, this, &SceneHierarchyPanel::rebuild);
    connect(&ctx_, &EditorContext::structureChanged, this, &SceneHierarchyPanel::rebuild);
    connect(&ctx_, &EditorContext::selectionChanged, this, &SceneHierarchyPanel::syncSelection);
    rebuild();
}

void SceneHierarchyPanel::rebuild() {
    rebuilding_ = true;
    tree_->clear();
    if (Scene* scene = ctx_.scene()) {
        std::function<void(entt::entity, QTreeWidgetItem*)> add = [&](entt::entity e, QTreeWidgetItem* parentItem) {
            auto* item = parentItem ? new QTreeWidgetItem(parentItem) : new QTreeWidgetItem(tree_);
            item->setText(0, QString::fromStdString(scene->nameOf(e)));
            item->setIcon(0, entityIcon(*scene, e));
            item->setData(0, kUuidRole, QVariant::fromValue<qulonglong>(scene->uuidOf(e).value()));
            item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            for (entt::entity child : scene->childrenOf(e)) add(child, item);
            item->setExpanded(!collapsed_.contains(scene->uuidOf(e)));
        };
        for (entt::entity root : scene->roots()) add(root, nullptr);
    }
    rebuilding_ = false;
    applyFilter();
    syncSelection();
}

void SceneHierarchyPanel::syncSelection() {
    rebuilding_ = true;
    tree_->clearSelection();
    const Uuid primary = ctx_.selectedUuid();
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        const Uuid id = itemUuid(*it);
        if (!ctx_.isSelected(id)) continue;
        if (id == primary) {
            tree_->setCurrentItem(*it, 0, QItemSelectionModel::NoUpdate);
            tree_->scrollToItem(*it);
        }
        (*it)->setSelected(true);
    }
    rebuilding_ = false;
}

void SceneHierarchyPanel::applyFilter() {
    const QString needle = filter_->text().trimmed();
    // An item is visible if it or any descendant matches.
    std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
        bool anyChild = false;
        for (int i = 0; i < item->childCount(); ++i) anyChild |= visit(item->child(i));
        const bool match = needle.isEmpty() || item->text(0).contains(needle, Qt::CaseInsensitive);
        item->setHidden(!(match || anyChild));
        if (!needle.isEmpty() && anyChild) item->setExpanded(true);
        return match || anyChild;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) visit(tree_->topLevelItem(i));
}

void SceneHierarchyPanel::showContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = tree_->itemAt(pos);
    if (item != nullptr && !ctx_.isSelected(itemUuid(item))) ctx_.select(itemUuid(item));
    QMenu menu(this);
    QMenu* create = menu.addMenu(app::theme::icon(QStringLiteral("plus")), item ? tr("Create Child") : tr("Create"));
    populateCreateMenu(*create, ctx_, item != nullptr);
    if (item != nullptr) {
        menu.addSeparator();
        menu.addAction(tr("Rename"), [this, item] { tree_->editItem(item); });
        menu.addAction(tr("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), [this] { ctx_.duplicateSelected(); });
        menu.addAction(tr("Unparent"), [this, item] { ctx_.reparent(itemUuid(item), Uuid{}, -1); });
        menu.addSeparator();
        menu.addAction(app::theme::icon(QStringLiteral("trash")), tr("Delete"), QKeySequence::Delete,
                       [this] { ctx_.deleteSelected(); });
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}

} // namespace lumora::editor
