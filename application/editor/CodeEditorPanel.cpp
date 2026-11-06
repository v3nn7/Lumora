#include "editor/CodeEditorPanel.hpp"

#include "common/Theme.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QTabWidget>
#include <QTextBlock>
#include <QVBoxLayout>

namespace lumora::editor {

namespace {

QTextCharFormat textFormat(const QColor& color, bool bold = false, bool italic = false) {
    QTextCharFormat f;
    f.setForeground(color);
    if (bold) f.setFontWeight(QFont::Bold);
    f.setFontItalic(italic);
    return f;
}

class LineNumberArea final : public QWidget {
public:
    explicit LineNumberArea(CodeEdit* editor) : QWidget(editor), editor_(editor) {}
    [[nodiscard]] QSize sizeHint() const override { return {editor_->lineNumberAreaWidth(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { editor_->paintLineNumbers(event); }

private:
    CodeEdit* editor_;
};

} // namespace

// ---- CppHighlighter --------------------------------------------------------------------------

CppHighlighter::CppHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {
    const QStringList keywords = {
        "alignas", "auto", "break", "case", "catch", "class", "const", "constexpr", "continue", "default", "delete",
        "do", "else", "enum", "explicit", "extern", "false", "final", "for", "friend", "if", "inline", "mutable",
        "namespace", "new", "noexcept", "nullptr", "operator", "override", "private", "protected", "public",
        "return", "sizeof", "static", "static_cast", "struct", "switch", "template", "this", "throw", "true", "try",
        "typename", "using", "virtual", "void", "volatile", "while", "co_await", "co_return", "concept", "requires"};
    const QStringList types = {"bool", "char", "double", "float", "int", "long", "short", "unsigned", "signed",
                               "size_t", "std", "string", "vector", "uint32_t", "int32_t", "uint64_t"};
    const QStringList api = {"Behaviour", "Vec3", "Entity", "Key", "MouseButton", "RayHit", "LUMORA_BEHAVIOUR",
                             "kNoEntity", "start", "update", "onCollision"};
    auto words = [](const QStringList& list) {
        return QRegularExpression(QStringLiteral("\\b(") + list.join(QLatin1Char('|')) + QStringLiteral(")\\b"));
    };
    rules_.push_back({words(keywords), textFormat(QColor(0xc6, 0x78, 0xdd), true)});
    rules_.push_back({words(types), textFormat(QColor(0x56, 0xb6, 0xc2))});
    rules_.push_back({words(api), textFormat(QColor(0xe5, 0xc0, 0x7b))});
    rules_.push_back({QRegularExpression(QStringLiteral("\\b[0-9]+(\\.[0-9]*)?[fFuUlL]*\\b")), textFormat(QColor(0xd1, 0x9a, 0x66))});
    rules_.push_back({QRegularExpression(QStringLiteral("\\b[A-Za-z_][A-Za-z0-9_]*(?=\\s*\\()")), textFormat(QColor(0x61, 0xaf, 0xef))});
    rules_.push_back({QRegularExpression(QStringLiteral("^\\s*#\\s*\\w+.*$")), textFormat(QColor(0xe0, 0x6c, 0x75))});
    rules_.push_back({QRegularExpression(QStringLiteral("\"([^\"\\\\]|\\\\.)*\"|'([^'\\\\]|\\\\.)*'")), textFormat(QColor(0x98, 0xc3, 0x79))});
    rules_.push_back({QRegularExpression(QStringLiteral("//[^\n]*")), textFormat(QColor(0x7f, 0x84, 0x8e), false, true)});
    comment_ = textFormat(QColor(0x7f, 0x84, 0x8e), false, true);
}

void CppHighlighter::highlightBlock(const QString& text) {
    for (const Rule& rule : rules_) {
        for (auto it = rule.pattern.globalMatch(text); it.hasNext();) {
            const auto m = it.next();
            setFormat(static_cast<int>(m.capturedStart()), static_cast<int>(m.capturedLength()), rule.format);
        }
    }
    // Block comments may span lines: state 1 = inside one.
    setCurrentBlockState(0);
    qsizetype start = previousBlockState() == 1 ? 0 : text.indexOf(QStringLiteral("/*"));
    while (start >= 0) {
        const qsizetype end = text.indexOf(QStringLiteral("*/"), start + (previousBlockState() == 1 && start == 0 ? 0 : 2));
        qsizetype length;
        if (end < 0) {
            setCurrentBlockState(1);
            length = text.length() - start;
        } else {
            length = end - start + 2;
        }
        setFormat(static_cast<int>(start), static_cast<int>(length), comment_);
        start = end < 0 ? -1 : text.indexOf(QStringLiteral("/*"), start + length);
    }
}

// ---- CodeEdit --------------------------------------------------------------------------------

CodeEdit::CodeEdit(QString path, QWidget* parent) : QPlainTextEdit(parent), path_(std::move(path)) {
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(std::max(mono.pointSizeF(), 10.0));
    setFont(mono);
    setTabStopDistance(QFontMetricsF(mono).horizontalAdvance(QLatin1Char(' ')) * 4);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    lineNumbers_ = new LineNumberArea(this);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(lineNumberAreaWidth(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy != 0) {
            lineNumbers_->scroll(0, dy);
        } else {
            lineNumbers_->update(0, rect.y(), lineNumbers_->width(), rect.height());
        }
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEdit::highlightCurrentLine);
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    new CppHighlighter(document());
    load();
    highlightCurrentLine();
}

bool CodeEdit::load() {
    QFile file(path_);
    if (!file.open(QIODevice::ReadOnly)) return false;
    setPlainText(QString::fromUtf8(file.readAll()));
    document()->setModified(false);
    return true;
}

bool CodeEdit::save() {
    QSaveFile file(path_);  // atomic: the file watcher never sees half a file
    if (!file.open(QIODevice::WriteOnly) || file.write(toPlainText().toUtf8()) < 0 || !file.commit()) return false;
    document()->setModified(false);
    emit saved(path_);
    return true;
}

void CodeEdit::goToLine(int line, int column) {
    QTextCursor cursor(document()->findBlockByNumber(std::max(line - 1, 0)));
    cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, std::max(column - 1, 0));
    setTextCursor(cursor);
    centerCursor();
    setFocus();
}

int CodeEdit::lineNumberAreaWidth() const {
    int digits = 1;
    for (int max = std::max(1, blockCount()); max >= 10; max /= 10) ++digits;
    return 14 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(digits, 3);
}

void CodeEdit::paintLineNumbers(QPaintEvent* event) {
    QPainter painter(lineNumbers_);
    painter.fillRect(event->rect(), palette().color(QPalette::AlternateBase));
    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int current = textCursor().blockNumber();
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            painter.setPen(number == current ? palette().color(QPalette::Text) : palette().color(QPalette::PlaceholderText));
            painter.drawText(0, top, lineNumbers_->width() - 6, fontMetrics().height(), Qt::AlignRight,
                             QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

void CodeEdit::resizeEvent(QResizeEvent* e) {
    QPlainTextEdit::resizeEvent(e);
    const QRect r = contentsRect();
    lineNumbers_->setGeometry(QRect(r.left(), r.top(), lineNumberAreaWidth(), r.height()));
}

void CodeEdit::highlightCurrentLine() {
    QTextEdit::ExtraSelection line;
    QColor color = palette().color(QPalette::Highlight);
    color.setAlpha(28);
    line.format.setBackground(color);
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    setExtraSelections({line});
    lineNumbers_->update();
}

bool CodeEdit::event(QEvent* e) {
    // Ctrl+S saves this file even though the editor window has a Save Scene shortcut.
    if (e->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(e);
        if (key->matches(QKeySequence::Save)) {
            e->accept();
            return true;
        }
    }
    return QPlainTextEdit::event(e);
}

void CodeEdit::keyPressEvent(QKeyEvent* e) {
    if (e->matches(QKeySequence::Save)) {
        if (!save()) QMessageBox::warning(this, tr("Save"), tr("Could not save %1.").arg(QFileInfo(path_).fileName()));
        return;
    }
    if (e->key() == Qt::Key_Tab && e->modifiers() == Qt::NoModifier) {
        insertPlainText(QStringLiteral("    "));
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        // Keep the indentation, one level deeper after an opening brace.
        const QString line = textCursor().block().text();
        QString indent;
        for (QChar c : line) {
            if (c != QLatin1Char(' ') && c != QLatin1Char('\t')) break;
            indent += c;
        }
        if (line.trimmed().endsWith(QLatin1Char('{'))) indent += QStringLiteral("    ");
        QPlainTextEdit::keyPressEvent(e);
        insertPlainText(indent);
        return;
    }
    if (e->key() == Qt::Key_BraceRight) {
        // Outdent a lone closing brace.
        QTextCursor cursor = textCursor();
        const QString before = cursor.block().text().left(cursor.positionInBlock());
        if (before.trimmed().isEmpty() && before.endsWith(QStringLiteral("    "))) {
            for (int i = 0; i < 4; ++i) cursor.deletePreviousChar();
        }
    }
    QPlainTextEdit::keyPressEvent(e);
}

// ---- CodeEditorPanel -------------------------------------------------------------------------

CodeEditorPanel::CodeEditorPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    auto* bar = new QHBoxLayout;
    auto* save = new QPushButton(app::theme::icon(QStringLiteral("save")), tr("Save"));
    save->setToolTip(tr("Save the open file (Ctrl+S); saving a script rebuilds the game"));
    auto* build = new QPushButton(tr("Build Scripts"));
    status_ = new QLabel(tr("Open a script from the Content Browser (Scripts folder) or Scripts > New Behaviour."));
    status_->setProperty("muted", true);
    bar->addWidget(save);
    bar->addWidget(build);
    bar->addWidget(status_, 1);
    layout->addLayout(bar);

    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    diagnostics_ = new QListWidget;
    diagnostics_->setToolTip(tr("Double-click a message to go to its line"));
    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(tabs_);
    split->addWidget(diagnostics_);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);

    connect(save, &QPushButton::clicked, this, [this] {
        if (CodeEdit* edit = current()) edit->save();
    });
    connect(build, &QPushButton::clicked, this, [this] {
        saveAll();
        emit buildRequested();
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &CodeEditorPanel::closeTab);
    connect(diagnostics_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        const QString file = item->data(Qt::UserRole).toString();
        if (file.isEmpty()) return;
        openFile(file, item->data(Qt::UserRole + 1).toInt());
    });
}

CodeEdit* CodeEditorPanel::current() const { return qobject_cast<CodeEdit*>(tabs_->currentWidget()); }

int CodeEditorPanel::openFileCount() const { return tabs_->count(); }
int CodeEditorPanel::diagnosticCount() const { return diagnostics_->count(); }

CodeEdit* CodeEditorPanel::openFile(const QString& path, int line) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* edit = qobject_cast<CodeEdit*>(tabs_->widget(i));
        if (edit != nullptr && QFileInfo(edit->path()).absoluteFilePath() == absolute) {
            tabs_->setCurrentIndex(i);
            if (line > 0) edit->goToLine(line);
            return edit;
        }
    }
    auto* edit = new CodeEdit(absolute);
    const int index = tabs_->addTab(edit, QFileInfo(absolute).fileName());
    tabs_->setTabToolTip(index, absolute);
    tabs_->setCurrentIndex(index);
    connect(edit->document(), &QTextDocument::modificationChanged, this, [this, edit] { updateTabTitle(edit); });
    connect(edit, &CodeEdit::saved, this, [this](const QString& file) {
        status_->setText(tr("Saved %1").arg(QFileInfo(file).fileName()));
    });
    if (line > 0) edit->goToLine(line);
    return edit;
}

void CodeEditorPanel::updateTabTitle(CodeEdit* edit) {
    const int index = tabs_->indexOf(edit);
    if (index < 0) return;
    tabs_->setTabText(index, QFileInfo(edit->path()).fileName() + (edit->document()->isModified() ? QStringLiteral(" *") : QString()));
}

bool CodeEditorPanel::closeTab(int index) {
    auto* edit = qobject_cast<CodeEdit*>(tabs_->widget(index));
    if (edit != nullptr && edit->document()->isModified()) {
        const auto answer = QMessageBox::question(this, tr("Close"), tr("Save changes to %1?").arg(QFileInfo(edit->path()).fileName()),
                                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel) return false;
        if (answer == QMessageBox::Save && !edit->save()) return false;
    }
    tabs_->removeTab(index);
    delete edit;
    return true;
}

bool CodeEditorPanel::hasUnsavedChanges() const {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* edit = qobject_cast<CodeEdit*>(tabs_->widget(i)); edit != nullptr && edit->document()->isModified()) return true;
    }
    return false;
}

bool CodeEditorPanel::saveAll() {
    bool ok = true;
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto* edit = qobject_cast<CodeEdit*>(tabs_->widget(i)); edit != nullptr && edit->document()->isModified()) {
            ok = edit->save() && ok;
        }
    }
    return ok;
}

void CodeEditorPanel::setBuilding() {
    status_->setText(tr("Building scripts…"));
}

void CodeEditorPanel::setBuildResult(bool ok, const QString& output) {
    diagnostics_->clear();
    // GCC/Clang: file:line:col: error: text    MSVC: file(line): error C1234: text
    static const QRegularExpression gnu(QStringLiteral(R"(^(.+?):(\d+):(?:(\d+):)?\s*(fatal error|error|warning):\s*(.*)$)"));
    static const QRegularExpression msvc(QStringLiteral(R"(^(.+?)\((\d+)(?:,(\d+))?\)\s*:\s*(fatal error|error|warning)\s*\w*:\s*(.*)$)"));
    int errors = 0;
    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        QRegularExpressionMatch m = gnu.match(line);
        if (!m.hasMatch()) m = msvc.match(line);
        if (!m.hasMatch()) continue;
        const QString file = m.captured(1);
        const bool error = m.captured(4).contains(QLatin1String("error"));
        errors += error ? 1 : 0;
        auto* item = new QListWidgetItem(app::theme::icon(error ? QStringLiteral("error") : QStringLiteral("warning")),
                                         QStringLiteral("%1:%2  %3").arg(QFileInfo(file).fileName(), m.captured(2), m.captured(5)));
        item->setData(Qt::UserRole, QFileInfo::exists(file) ? file : QString());
        item->setData(Qt::UserRole + 1, m.captured(2).toInt());
        diagnostics_->addItem(item);
    }
    if (ok) {
        status_->setText(diagnostics_->count() > 0 ? tr("Scripts built (%n warning(s))", nullptr, diagnostics_->count())
                                                   : tr("Scripts built and loaded"));
    } else if (errors > 0) {
        status_->setText(tr("Build failed: %n error(s)", nullptr, errors));
    } else {
        status_->setText(tr("Build failed - see the Console"));
        auto* item = new QListWidgetItem(app::theme::icon(QStringLiteral("error")), output.trimmed().right(300));
        diagnostics_->addItem(item);
    }
}

} // namespace lumora::editor
