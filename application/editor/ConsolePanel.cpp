#include "editor/ConsolePanel.hpp"

#include "common/Theme.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

namespace lumora::editor {

namespace {
QColor levelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return QColor(0x6b, 0x71, 0x7c);
        case LogLevel::Debug: return QColor(0x7f, 0xa7, 0xc9);
        case LogLevel::Info: return QColor(0xc9, 0xce, 0xd6);
        case LogLevel::Warn: return app::theme::kWarning;
        case LogLevel::Error:
        case LogLevel::Fatal: return app::theme::kError;
    }
    return app::theme::kText;
}
} // namespace

ConsolePanel::ConsolePanel(app::LogBridge& bridge, QWidget* parent) : QWidget(parent), bridge_(bridge) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto* bar = new QHBoxLayout;
    level_ = new QComboBox;
    level_->addItems({tr("All"), tr("Debug+"), tr("Info+"), tr("Warnings+"), tr("Errors")});
    level_->setCurrentIndex(2);
    search_ = new QLineEdit;
    search_->setPlaceholderText(tr("Filter messages"));
    search_->addAction(app::theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    search_->setClearButtonEnabled(true);
    counts_ = new QLabel;
    counts_->setProperty("muted", true);
    auto* clear = new QToolButton;
    clear->setIcon(app::theme::icon(QStringLiteral("trash")));
    clear->setToolTip(tr("Clear console"));
    bar->addWidget(level_);
    bar->addWidget(search_, 1);
    bar->addWidget(counts_);
    bar->addWidget(clear);
    layout->addLayout(bar);

    view_ = new QPlainTextEdit;
    view_->setReadOnly(true);
    view_->setMaximumBlockCount(app::LogBridge::kHistory);
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    view_->setStyleSheet(QStringLiteral("font-family: 'JetBrains Mono', 'DejaVu Sans Mono', monospace; font-size: 9pt;"));
    layout->addWidget(view_, 1);

    connect(level_, &QComboBox::currentIndexChanged, this, &ConsolePanel::rebuild);
    connect(search_, &QLineEdit::textChanged, this, &ConsolePanel::rebuild);
    connect(clear, &QToolButton::clicked, this, [this] {
        bridge_.clearHistory();
        warnings_ = errors_ = 0;
        rebuild();
    });
    connect(&bridge_, &app::LogBridge::messageLogged, this, [this](const app::UiLogEntry& e) {
        if (e.level == LogLevel::Warn) ++warnings_;
        if (e.level >= LogLevel::Error) ++errors_;
        updateCounts();
        if (accepts(e)) append(e);
    });
    for (const auto& e : bridge_.history()) {
        if (e.level == LogLevel::Warn) ++warnings_;
        if (e.level >= LogLevel::Error) ++errors_;
    }
    rebuild();
}

bool ConsolePanel::accepts(const app::UiLogEntry& e) const {
    static constexpr LogLevel kMin[] = {LogLevel::Trace, LogLevel::Debug, LogLevel::Info, LogLevel::Warn, LogLevel::Error};
    if (e.level < kMin[std::clamp(level_->currentIndex(), 0, 4)]) return false;
    const QString needle = search_->text();
    return needle.isEmpty() || e.text.contains(needle, Qt::CaseInsensitive) ||
           e.category.contains(needle, Qt::CaseInsensitive);
}

void ConsolePanel::append(const app::UiLogEntry& e) {
    QScrollBar* bar = view_->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;
    QTextCursor cursor(view_->document());
    cursor.movePosition(QTextCursor::End);
    if (!view_->document()->isEmpty()) cursor.insertBlock();
    QTextCharFormat dim;
    dim.setForeground(QColor(0x6b, 0x71, 0x7c));
    cursor.insertText(e.time.toString(QStringLiteral("HH:mm:ss.zzz ")), dim);
    QTextCharFormat cat;
    cat.setForeground(QColor(0x8a, 0x90, 0x9c));
    cursor.insertText(QStringLiteral("[%1] ").arg(e.category), cat);
    QTextCharFormat text;
    text.setForeground(levelColor(e.level));
    cursor.insertText(e.text, text);
    if (atBottom) bar->setValue(bar->maximum());
}

void ConsolePanel::rebuild() {
    view_->clear();
    for (const auto& e : bridge_.history()) {
        if (accepts(e)) append(e);
    }
    updateCounts();
}

void ConsolePanel::updateCounts() {
    counts_->setText(tr("%1 warnings · %2 errors").arg(warnings_).arg(errors_));
}

} // namespace lumora::editor
