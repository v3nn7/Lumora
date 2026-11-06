#include "editor/AddComponentMenu.hpp"

#include "common/Theme.hpp"
#include "editor/QuickOpen.hpp"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QShowEvent>
#include <QTimer>
#include <QWidgetAction>

#include <algorithm>

namespace lumora::editor {

const QStringList& AddComponentMenu::categories() {
    static const QStringList order{QStringLiteral("Core"),      QStringLiteral("Rendering"), QStringLiteral("Environment"),
                                   QStringLiteral("Physics"),   QStringLiteral("Animation"), QStringLiteral("Navigation"),
                                   QStringLiteral("Audio"),     QStringLiteral("Scripting"), QStringLiteral("UI")};
    return order;
}

QIcon AddComponentMenu::categoryIcon(const QString& category) {
    static const QHash<QString, QString> icons{
        {QStringLiteral("Core"), QStringLiteral("cube")},           {QStringLiteral("Rendering"), QStringLiteral("sparkle")},
        {QStringLiteral("Environment"), QStringLiteral("mountain")}, {QStringLiteral("Physics"), QStringLiteral("physics")},
        {QStringLiteral("Animation"), QStringLiteral("animation")}, {QStringLiteral("Navigation"), QStringLiteral("navigation")},
        {QStringLiteral("Audio"), QStringLiteral("audio")},         {QStringLiteral("Scripting"), QStringLiteral("code")},
        {QStringLiteral("UI"), QStringLiteral("ui")},
    };
    return app::theme::icon(icons.value(category, QStringLiteral("entity")));
}

namespace {

/// Enter in the search field adds the best match; Down moves into the list.
class SearchField final : public QLineEdit {
public:
    explicit SearchField(AddComponentMenu* menu) : menu_(menu) {}

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
            // Hand the arrow keys to the menu so the items can be walked.
            QKeyEvent copy(event->type(), event->key(), event->modifiers());
            QCoreApplication::sendEvent(menu_, &copy);
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

private:
    AddComponentMenu* menu_;
};

} // namespace

AddComponentMenu::AddComponentMenu(std::vector<Entry> entries, QWidget* parent)
    : QMenu(parent), entries_(std::move(entries)) {
    std::ranges::sort(entries_, [](const Entry& a, const Entry& b) { return a.label.localeAwareCompare(b.label) < 0; });
    search_ = new SearchField(this);
    search_->setPlaceholderText(tr("Search"));
    search_->setClearButtonEnabled(true);
    search_->addAction(app::theme::icon(QStringLiteral("search")), QLineEdit::LeadingPosition);
    search_->setMinimumWidth(220);
    searchAction_ = new QWidgetAction(this);
    searchAction_->setDefaultWidget(search_);
    addAction(searchAction_);
    addSeparator();
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) { filter(text); });
    connect(search_, &QLineEdit::returnPressed, this, [this] {
        if (firstMatch_ != nullptr) firstMatch_->trigger();
    });
    filter({});
}

void AddComponentMenu::clearItems() {
    firstMatch_ = nullptr;
    const QList<QAction*> all = actions();
    for (QAction* a : all) {
        if (a == searchAction_) continue;
        removeAction(a);
        if (QMenu* sub = a->menu()) sub->deleteLater();
        a->deleteLater();
    }
    addSeparator();
}

int AddComponentMenu::filter(const QString& text) {
    clearItems();
    const QString pattern = text.trimmed();
    auto add = [this](QMenu* into, const Entry& e, const QString& label) {
        QAction* action = into->addAction(categoryIcon(e.category), label);
        connect(action, &QAction::triggered, this, [this, id = e.id] {
            emit chosen(id);
            close();
        });
        return action;
    };
    if (pattern.isEmpty()) {
        int count = 0;
        for (const QString& category : categories()) {
            QMenu* sub = nullptr;
            for (const Entry& e : entries_) {
                if (e.category != category) continue;
                if (sub == nullptr) sub = addMenu(categoryIcon(category), category);
                add(sub, e, e.label);
                ++count;
            }
        }
        // Categories the list does not know about, at the end.
        for (const Entry& e : entries_) {
            if (categories().contains(e.category)) continue;
            add(this, e, e.label);
            ++count;
        }
        return count;
    }
    // Matches, best first: by the name, then by the category ("phys" lists all of Physics).
    std::vector<std::pair<int, const Entry*>> matches;
    for (const Entry& e : entries_) {
        const int byName = fuzzyScore(pattern, e.label);
        const int byCategory = e.category.startsWith(pattern, Qt::CaseInsensitive) ? 1 : -1;
        const int score = std::max(byName >= 0 ? byName + 1000 : -1, byCategory);
        if (score >= 0) matches.emplace_back(score, &e);
    }
    std::ranges::stable_sort(matches, [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [score, e] : matches) {
        QAction* action = add(this, *e, e->label + QLatin1Char('\t') + e->category);
        if (firstMatch_ == nullptr) firstMatch_ = action;
    }
    if (matches.empty()) addAction(tr("No component matches"))->setEnabled(false);
    return static_cast<int>(matches.size());
}

void AddComponentMenu::showEvent(QShowEvent* event) {
    QMenu::showEvent(event);
    search_->clear();
    QTimer::singleShot(0, search_, [field = search_] { field->setFocus(Qt::PopupFocusReason); });
}

} // namespace lumora::editor
