#include "common/Theme.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>

namespace lumora::app::theme {

namespace {

const char* kStyleSheet = R"(
* { outline: none; }
QWidget { color: #d7dae0; }
QMainWindow, QDialog { background: #121316; }
QMainWindow::separator { background: #121316; width: 4px; height: 4px; }
QMainWindow::separator:hover { background: #4f8cff; }

QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; }
QDockWidget::title {
    background: #1a1c21; padding: 6px 10px; border-bottom: 1px solid #2c3038;
    font-weight: 600; color: #aeb4bf; text-transform: uppercase;
}
QDockWidget > QWidget { background: #1a1c21; }

QTabBar::tab {
    background: #16181c; color: #8a909c; padding: 6px 14px; border: none;
    border-bottom: 2px solid transparent;
}
QTabBar::tab:selected { color: #d7dae0; background: #1a1c21; border-bottom: 2px solid #4f8cff; }
QTabBar::tab:hover:!selected { color: #c4c8d0; }

QMenuBar { background: #121316; border-bottom: 1px solid #23262d; }
QMenuBar::item { padding: 5px 10px; background: transparent; border-radius: 4px; }
QMenuBar::item:selected { background: #22252b; }
QMenu { background: #1d2025; border: 1px solid #2c3038; padding: 4px; border-radius: 6px; }
QMenu::item { padding: 6px 28px 6px 24px; border-radius: 4px; }
QMenu::item:selected { background: #2b4a80; }
QMenu::item:disabled { color: #5b616c; }
QMenu::separator { height: 1px; background: #2c3038; margin: 4px 8px; }
QMenu::icon { padding-left: 6px; }

QToolBar { background: #16181c; border: none; border-bottom: 1px solid #23262d; spacing: 4px; padding: 4px 8px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 5px; padding: 4px; }
QToolButton:hover { background: #262a31; border-color: #30353e; }
QToolButton:pressed, QToolButton:checked { background: #2b3340; border-color: #3a4ecc55; }
QToolButton:disabled { opacity: 0.4; }

QPushButton {
    background: #262a31; border: 1px solid #333842; border-radius: 6px; padding: 6px 14px;
}
QPushButton:hover { background: #2d323a; border-color: #3c424d; }
QPushButton:pressed { background: #22262c; }
QPushButton:disabled { color: #5b616c; background: #1e2126; border-color: #2a2e35; }
QPushButton[accent="true"] { background: #4f8cff; border-color: #5b95ff; color: #ffffff; font-weight: 600; }
QPushButton[accent="true"]:hover { background: #659bff; }
QPushButton[accent="true"]:pressed { background: #3f78e6; }
QPushButton[flat="true"] { background: transparent; border-color: transparent; }
QPushButton[flat="true"]:hover { background: #262a31; }

QLineEdit, QPlainTextEdit, QTextEdit, QAbstractSpinBox, QComboBox {
    background: #16181c; border: 1px solid #2c3038; border-radius: 5px; padding: 4px 6px;
    selection-background-color: #2b4a80;
}
QLineEdit:focus, QPlainTextEdit:focus, QAbstractSpinBox:focus, QComboBox:focus { border-color: #4f8cff; }
QLineEdit:disabled, QAbstractSpinBox:disabled { color: #6b717c; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 0; border: none; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: #1d2025; border: 1px solid #2c3038; selection-background-color: #2b4a80; }

QTreeView, QListView, QTableView {
    background: #16181c; border: none; alternate-background-color: #181a1f;
    selection-background-color: #2b4a80; selection-color: #ffffff;
}
QTreeView::item, QListView::item { padding: 3px 2px; border-radius: 3px; }
QTreeView::item:hover, QListView::item:hover { background: #20242b; }
QTreeView::item:selected, QListView::item:selected { background: #2b4a80; }
QHeaderView::section { background: #1a1c21; color: #8a909c; border: none; border-bottom: 1px solid #2c3038; padding: 4px 6px; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 0; }
QScrollBar::handle { background: #343943; border-radius: 4px; min-height: 24px; min-width: 24px; }
QScrollBar::handle:hover { background: #454b57; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #3a404a; border-radius: 3px; background: #16181c; }
QCheckBox::indicator:checked { background: #4f8cff; border-color: #4f8cff; }
QSlider::groove:horizontal { height: 4px; background: #2c3038; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #4f8cff; border-radius: 2px; }
QSlider::handle:horizontal { background: #d7dae0; width: 12px; margin: -5px 0; border-radius: 6px; }

QStatusBar { background: #121316; color: #8a909c; border-top: 1px solid #23262d; }
QStatusBar::item { border: none; }
QToolTip { background: #22252b; color: #d7dae0; border: 1px solid #343943; padding: 4px 6px; border-radius: 4px; }
QSplitter::handle { background: #121316; }
QProgressBar { background: #16181c; border: 1px solid #2c3038; border-radius: 4px; text-align: center; height: 12px; }
QProgressBar::chunk { background: #4f8cff; border-radius: 3px; }

QLabel[muted="true"] { color: #8a909c; }
QLabel[heading="true"] { font-size: 18pt; font-weight: 600; color: #f0f2f5; }
QLabel[section="true"] { color: #8a909c; font-weight: 600; text-transform: uppercase; letter-spacing: 1px; }
)";

} // namespace

void apply(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window, kBackground);
    p.setColor(QPalette::WindowText, kText);
    p.setColor(QPalette::Base, QColor(0x16, 0x18, 0x1c));
    p.setColor(QPalette::AlternateBase, kPanel);
    p.setColor(QPalette::ToolTipBase, kRaised);
    p.setColor(QPalette::ToolTipText, kText);
    p.setColor(QPalette::Text, kText);
    p.setColor(QPalette::Button, kRaised);
    p.setColor(QPalette::ButtonText, kText);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, QColor(0x2b, 0x4a, 0x80));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, kAccent);
    p.setColor(QPalette::PlaceholderText, QColor(0x6b, 0x71, 0x7c));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x5b, 0x61, 0x6c));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x5b, 0x61, 0x6c));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x5b, 0x61, 0x6c));
    app.setPalette(p);

    QFont font = app.font();
    const QStringList families = QFontDatabase::families();
    for (const char* preferred : {"Inter", "Inter Display", "Noto Sans", "Segoe UI", "Cantarell"}) {
        if (families.contains(QString::fromLatin1(preferred))) {
            font.setFamily(QString::fromLatin1(preferred));
            break;
        }
    }
    font.setPointSizeF(9.5);
    font.setHintingPreference(QFont::PreferNoHinting);
    app.setFont(font);
    app.setStyleSheet(QString::fromLatin1(kStyleSheet));
}

QIcon icon(const QString& name) { return QIcon(QStringLiteral(":/icons/%1.svg").arg(name)); }

} // namespace lumora::app::theme
