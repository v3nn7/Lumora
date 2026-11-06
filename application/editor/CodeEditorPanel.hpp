#pragma once

#include "common/ModuleExport.hpp"

#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QWidget>

#include <vector>

class QLabel;
class QListWidget;
class QTabWidget;

namespace lumora::editor {

/// C++ colouring: keywords, types, Lumora's scripting API, strings, numbers, comments,
/// preprocessor lines (and block comments across lines).
class LUMORA_EDITOR_API CppHighlighter final : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit CppHighlighter(QTextDocument* document);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    std::vector<Rule> rules_;
    QTextCharFormat comment_;
};

/// One open file: line numbers, current-line highlight, 4-space tabs, auto-indent, Ctrl+S.
class LUMORA_EDITOR_API CodeEdit final : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEdit(QString path, QWidget* parent = nullptr);

    [[nodiscard]] const QString& path() const noexcept { return path_; }
    bool load();
    bool save();
    void goToLine(int line, int column = 1);
    [[nodiscard]] int lineNumberAreaWidth() const;
    void paintLineNumbers(class QPaintEvent* event);

signals:
    void saved(const QString& path);

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void highlightCurrentLine();
    QString path_;
    QWidget* lineNumbers_ = nullptr;
};

/// The editor's code panel: tabs of open files (Scripts/*.cpp and other text files), the script
/// build's diagnostics below (double-click jumps to the line), Save and Build buttons. Saving a
/// script makes the editor rebuild the game library and reload it, also while playing.
class LUMORA_EDITOR_API CodeEditorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit CodeEditorPanel(QWidget* parent = nullptr);

    /// Opens a file (or switches to it if it is open already).
    CodeEdit* openFile(const QString& path, int line = 0);
    /// Shows a script build's result; compiler messages become clickable diagnostics.
    void setBuildResult(bool ok, const QString& output);
    void setBuilding();
    [[nodiscard]] bool hasUnsavedChanges() const;
    bool saveAll();
    [[nodiscard]] int openFileCount() const;
    [[nodiscard]] int diagnosticCount() const;

signals:
    void buildRequested();

private:
    [[nodiscard]] CodeEdit* current() const;
    void updateTabTitle(CodeEdit* edit);
    bool closeTab(int index);

    QTabWidget* tabs_ = nullptr;
    QListWidget* diagnostics_ = nullptr;
    QLabel* status_ = nullptr;
};

} // namespace lumora::editor
