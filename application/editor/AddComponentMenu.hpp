#pragma once

#include "common/ModuleExport.hpp"

#include <QIcon>
#include <QMenu>
#include <QString>

#include <vector>

class QLineEdit;
class QWidgetAction;

namespace lumora::editor {

/// "Add Component": a search field on top, then the components grouped in category submenus
/// (Core, Rendering, Environment, Physics, ...). Typing lists the matching components directly;
/// Enter adds the best one.
class LUMORA_EDITOR_API AddComponentMenu final : public QMenu {
    Q_OBJECT
public:
    struct Entry {
        QString id;
        QString label;
        QString category;
    };
    explicit AddComponentMenu(std::vector<Entry> entries, QWidget* parent = nullptr);

    /// Shows the matches for `text` (empty: the categories). Returns how many components match.
    int filter(const QString& text);
    [[nodiscard]] QLineEdit* searchField() const noexcept { return search_; }
    [[nodiscard]] static QIcon categoryIcon(const QString& category);
    /// Category order in the menu.
    [[nodiscard]] static const QStringList& categories();

signals:
    void chosen(const QString& id);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void clearItems();
    std::vector<Entry> entries_;
    QLineEdit* search_ = nullptr;
    QWidgetAction* searchAction_ = nullptr;
    QAction* firstMatch_ = nullptr;
};

} // namespace lumora::editor
