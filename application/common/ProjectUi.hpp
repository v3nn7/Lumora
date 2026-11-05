#pragma once

#include "common/ModuleExport.hpp"

#include "project/Project.hpp"

#include <QDialog>
#include <QPixmap>
#include <QString>

class QLineEdit;
class QPlainTextEdit;
class QComboBox;

namespace lumora::app {

/// Saved/Thumbnail.png if present, otherwise a generated placeholder with the project initials.
[[nodiscard]] QPixmap projectThumbnail(const QString& projectRoot, const QString& name, const QSize& size);
[[nodiscard]] QPixmap placeholderThumbnail(const QString& name, const QSize& size);
[[nodiscard]] QString relativeTimeString(qint64 unixSeconds);
[[nodiscard]] QString toQString(const std::filesystem::path& p);
[[nodiscard]] std::filesystem::path toPath(const QString& s);

/// Edits project metadata (name, description, default scene) and saves the descriptor.
class LUMORA_APPCOMMON_API ProjectSettingsDialog final : public QDialog {
    Q_OBJECT
public:
    ProjectSettingsDialog(Project& project, QWidget* parent = nullptr);

private:
    void accept() override;

    Project& project_;
    QLineEdit* name_ = nullptr;
    QPlainTextEdit* description_ = nullptr;
    QComboBox* defaultScene_ = nullptr;
};

} // namespace lumora::app
