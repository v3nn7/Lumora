#pragma once

#include "common/Preferences.hpp"

#include <QWidget>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;

namespace lumora::app {

class AppContext;

/// Hub > Settings: graphics device, validation layers, v-sync, default project location and
/// maintenance actions. Values persist immediately; graphics changes apply on the next start.
class HubSettingsPage final : public QWidget {
    Q_OBJECT
public:
    explicit HubSettingsPage(AppContext& app, QWidget* parent = nullptr);

signals:
    void recentProjectsChanged();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void populateGpus();
    void commit();

    AppContext& app_;
    Preferences prefs_;
    QComboBox* gpu_ = nullptr;
    QComboBox* validation_ = nullptr;
    QCheckBox* vsync_ = nullptr;
    QLineEdit* location_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* activeGpu_ = nullptr;
    bool populated_ = false;
    bool loading_ = false;
};

} // namespace lumora::app
