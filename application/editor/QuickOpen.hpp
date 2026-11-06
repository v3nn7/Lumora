#pragma once

#include "common/ModuleExport.hpp"

#include <QDialog>
#include <QIcon>
#include <QString>

#include <functional>
#include <vector>

class QLineEdit;
class QListWidget;

namespace lumora::editor {

/// How well `pattern` matches `text` as a subsequence (case-insensitive): -1 for no match,
/// higher is better. Consecutive letters, word starts and matches in the file name score more.
[[nodiscard]] LUMORA_EDITOR_API int fuzzyScore(const QString& pattern, const QString& text);

/// Ctrl+P: type a few letters of a file or an entity, Enter opens it.
class LUMORA_EDITOR_API QuickOpenDialog final : public QDialog {
    Q_OBJECT
public:
    struct Item {
        QString label;   // shown and matched (a relative path or an entity name)
        QString detail;  // shown after it, dimmed ("Entity", "Scene", ...)
        QIcon icon;
        std::function<void()> open;
    };
    explicit QuickOpenDialog(std::vector<Item> items, QWidget* parent = nullptr);

    void setQuery(const QString& query);
    [[nodiscard]] QStringList shownLabels() const;
    /// Opens the highlighted item and closes the dialog.
    void openCurrent();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();

    std::vector<Item> items_;
    std::vector<std::size_t> shown_;
    QLineEdit* query_ = nullptr;
    QListWidget* list_ = nullptr;
};

} // namespace lumora::editor
