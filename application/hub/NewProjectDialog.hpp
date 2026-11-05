#pragma once

#include "project/Project.hpp"

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace lumora::app {

class AppContext;

/// Template picker + name/location form. Template previews are real offscreen renders of
/// each template scene when the GPU is available.
class NewProjectDialog final : public QDialog {
    Q_OBJECT
public:
    NewProjectDialog(AppContext& app, QWidget* parent = nullptr);

    /// Descriptor path of the created project (valid after exec() == Accepted).
    [[nodiscard]] const QString& createdDescriptor() const noexcept { return created_; }

private:
    void updateState();
    void create();
    void renderPreviews();
    [[nodiscard]] ProjectTemplate selectedTemplate() const;
    [[nodiscard]] QString targetDirectory() const;

    AppContext& app_;
    QListWidget* templates_ = nullptr;
    QLabel* templateDescription_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* location_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* error_ = nullptr;
    QPushButton* createButton_ = nullptr;
    QString created_;
};

} // namespace lumora::app
