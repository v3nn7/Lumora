#pragma once

#include "common/ModuleExport.hpp"

#include "assets/Material.hpp"
#include "editor/PropertyWidgets.hpp"

#include <QImage>
#include <QWidget>

#include <functional>
#include <vector>

class QLabel;
class QTimer;

namespace lumora {
class AssetLibrary;
}

namespace lumora::editor {

/// Renders a material (absolute .lmat path) on a sphere, `size` pixels square; null if it cannot.
using MaterialPreview = std::function<QImage(const QString& file, int size)>;

/// Edits a material asset (.lmat) in place: every change is written to the file at once and
/// every entity using the material shows it (the asset library re-reads the file). With a
/// preview renderer, a sphere wearing the material sits above the fields.
class LUMORA_EDITOR_API MaterialEditor final : public QWidget {
    Q_OBJECT
public:
    /// `file` absolute; `key` the project-relative reference entities use.
    MaterialEditor(QString file, std::string key, AssetLibrary* assets, AssetPicker pickAsset, MaterialPreview preview = {},
                   QWidget* parent = nullptr);
    /// The preview image (null without a preview renderer or before the first render).
    [[nodiscard]] QImage previewImage() const;

    [[nodiscard]] bool isLoaded() const noexcept { return loaded_; }
    [[nodiscard]] const MaterialAsset& material() const noexcept { return material_; }
    /// Re-reads the file (changed elsewhere) and updates the fields.
    void reload();

signals:
    void changed();

private:
    void write();
    void renderPreview();

    QString file_;
    std::string key_;
    AssetLibrary* assets_ = nullptr;
    MaterialAsset material_;
    bool loaded_ = false;
    QLabel* error_ = nullptr;
    QLabel* preview_ = nullptr;
    QTimer* previewTimer_ = nullptr;
    MaterialPreview previewRenderer_;
    std::vector<Refresher> refreshers_;
};

} // namespace lumora::editor
