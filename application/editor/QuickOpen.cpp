#include "editor/QuickOpen.hpp"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <numeric>

namespace lumora::editor {

int fuzzyScore(const QString& pattern, const QString& text) {
    if (pattern.isEmpty()) return 0;
    const qsizetype nameStart = text.lastIndexOf(QLatin1Char('/')) + 1;
    int score = 0;
    qsizetype t = 0;
    qsizetype previous = -2;
    for (const QChar p : pattern) {
        if (p.isSpace()) continue;
        const QChar wanted = p.toLower();
        while (t < text.size() && text.at(t).toLower() != wanted) ++t;
        if (t >= text.size()) return -1;
        int points = 1;
        if (t == previous + 1) points += 5;  // runs of letters
        const QChar before = t > 0 ? text.at(t - 1) : QLatin1Char('/');
        if (!before.isLetterOrNumber() || (text.at(t).isUpper() && before.isLower())) points += 8;  // word starts
        if (t >= nameStart) points += 3;  // in the file name rather than the folders
        score += points;
        previous = t;
        ++t;
    }
    return score * 100 - static_cast<int>(text.size());  // shorter wins ties
}

QuickOpenDialog::QuickOpenDialog(std::vector<Item> items, QWidget* parent)
    : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint), items_(std::move(items)) {
    setMinimumSize(560, 380);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    query_ = new QLineEdit;
    query_->setPlaceholderText(tr("Go to file or entity…"));
    query_->setClearButtonEnabled(true);
    list_ = new QListWidget;
    list_->setUniformItemSizes(true);
    layout->addWidget(query_);
    layout->addWidget(list_, 1);
    query_->installEventFilter(this);
    connect(query_, &QLineEdit::textChanged, this, &QuickOpenDialog::refresh);
    connect(list_, &QListWidget::itemActivated, this, [this] { openCurrent(); });
    refresh();
    query_->setFocus();
}

void QuickOpenDialog::setQuery(const QString& query) { query_->setText(query); }

void QuickOpenDialog::refresh() {
    const QString q = query_->text().trimmed();
    std::vector<std::pair<int, std::size_t>> scored;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const int s = fuzzyScore(q, items_[i].label);
        if (s >= 0) scored.emplace_back(s, i);
    }
    std::ranges::stable_sort(scored, std::greater<>{}, &std::pair<int, std::size_t>::first);
    if (scored.size() > 200) scored.resize(200);
    shown_.clear();
    list_->clear();
    for (const auto& [score, i] : scored) {
        shown_.push_back(i);
        const Item& item = items_[i];
        auto* row = new QListWidgetItem(item.icon, item.detail.isEmpty() ? item.label
                                                                         : item.label + QStringLiteral("   ·  ") + item.detail);
        list_->addItem(row);
    }
    if (list_->count() > 0) list_->setCurrentRow(0);
}

QStringList QuickOpenDialog::shownLabels() const {
    QStringList labels;
    for (std::size_t i : shown_) labels << items_[i].label;
    return labels;
}

void QuickOpenDialog::openCurrent() {
    const int row = list_->currentRow();
    if (row < 0 || static_cast<std::size_t>(row) >= shown_.size()) return;
    const auto open = items_[shown_[static_cast<std::size_t>(row)]].open;
    accept();
    if (open) open();
}

bool QuickOpenDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == query_ && event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        switch (key->key()) {
            case Qt::Key_Down:
            case Qt::Key_Up: {
                const int step = key->key() == Qt::Key_Down ? 1 : -1;
                if (list_->count() > 0) list_->setCurrentRow(std::clamp(list_->currentRow() + step, 0, list_->count() - 1));
                return true;
            }
            case Qt::Key_Return:
            case Qt::Key_Enter:
                openCurrent();
                return true;
            case Qt::Key_Escape:
                reject();
                return true;
            default:
                break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace lumora::editor
