#pragma once

#include "common/ModuleExport.hpp"

#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QRegularExpression>
#include <QWidget>

#include <vector>

class QCompleter;
class QLabel;
class QModelIndex;
class QStandardItemModel;
class QListWidget;
class QPushButton;
class QTabWidget;

namespace lumora::editor {

/// C++ (or, with lua = true, Lua) colouring: keywords, types, Lumora's scripting API, strings,
/// numbers, comments, preprocessor lines (and block comments across lines).
class LUMORA_EDITOR_API CppHighlighter final : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit CppHighlighter(QTextDocument* document, bool lua = false);

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    std::vector<Rule> rules_;
    QTextCharFormat comment_;
    QString blockOpen_ = QStringLiteral("/*");
    QString blockClose_ = QStringLiteral("*/");
};

/// One open file: line numbers, current-line highlight, 4-space tabs, auto-indent, Ctrl+S, and
/// completion of Lumora's scripting API (and words of the file) while typing or on Ctrl+Space:
/// after "Key::" the keys, after "." the fields of Vec3 and RayHit.
class LUMORA_EDITOR_API CodeEdit final : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEdit(QString path, QWidget* parent = nullptr);

    [[nodiscard]] const QString& path() const noexcept { return path_; }
    bool load();
    bool save();
    void goToLine(int line, int column = 1);
    [[nodiscard]] int lineNumberAreaWidth() const;
    [[nodiscard]] QCompleter* completer() const noexcept { return completer_; }
    /// Shows completions for the text before the cursor (forced: also for a short or empty word).
    void showCompletions(bool forced);
    void paintLineNumbers(class QPaintEvent* event);

signals:
    void saved(const QString& path);

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void highlightCurrentLine();
    void insertCompletion(const QModelIndex& index);
    QString path_;
    QWidget* lineNumbers_ = nullptr;
    QCompleter* completer_ = nullptr;
    QStandardItemModel* completions_ = nullptr;
    int completionStart_ = 0;  // where the word being completed starts (document position)
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

    /// Shows whether the panel sits in the editor's main area (next to the scene) or in a dock.
    void setInMainArea(bool inMainArea);

signals:
    void buildRequested();
    /// The "Main Area" / "Dock" button: move the panel to the other place.
    void placementToggled();

private:
    [[nodiscard]] CodeEdit* current() const;
    void updateTabTitle(CodeEdit* edit);
    bool closeTab(int index);

    QTabWidget* tabs_ = nullptr;
    QListWidget* diagnostics_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* placement_ = nullptr;
};

} // namespace lumora::editor
