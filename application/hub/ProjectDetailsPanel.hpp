#pragma once

#include <QModelIndex>
#include <QWidget>

class QLabel;
class QPushButton;

namespace lumora::app {

/// Right-hand panel in the Hub describing the selected project: large thumbnail, description,
/// location, engine version, dates, scene count and size on disk, plus actions.
class ProjectDetailsPanel final : public QWidget {
    Q_OBJECT
public:
    explicit ProjectDetailsPanel(QWidget* parent = nullptr);
    void showProject(const QModelIndex& index);

signals:
    void openRequested(const QString& descriptor);
    void settingsRequested(const QString& descriptor);
    void revealRequested(const QString& root);
    void removeRequested(const QString& descriptor);

private:
    QLabel* thumbnail_ = nullptr;
    QLabel* name_ = nullptr;
    QLabel* description_ = nullptr;
    QLabel* location_ = nullptr;
    QLabel* engine_ = nullptr;
    QLabel* created_ = nullptr;
    QLabel* opened_ = nullptr;
    QLabel* contents_ = nullptr;
    QPushButton* open_ = nullptr;
    QPushButton* settings_ = nullptr;
    QString descriptor_;
    QString root_;
};

} // namespace lumora::app
