#pragma once

#include "common/ModuleExport.hpp"

#include "assets/Material.hpp"
#include "editor/PropertyWidgets.hpp"

#include <QWidget>

#include <vector>

class QLabel;

namespace lumora {
class AssetLibrary;
}

namespace lumora::editor {

/// Edits a material asset (.lmat) in place: every change is written to the file at once and
/// every entity using the material shows it (the asset library re-reads the file).
class LUMORA_EDITOR_API MaterialEditor final : public QWidget {
    Q_OBJECT
public:
    /// `file` absolute; `key` the project-relative reference entities use.
    MaterialEditor(QString file, std::string key, AssetLibrary* assets, AssetPicker pickAsset, QWidget* parent = nullptr);

    [[nodiscard]] bool isLoaded() const noexcept { return loaded_; }
    [[nodiscard]] const MaterialAsset& material() const noexcept { return material_; }
    /// Re-reads the file (changed elsewhere) and updates the fields.
    void reload();

signals:
    void changed();

private:
    void write();

    QString file_;
    std::string key_;
    AssetLibrary* assets_ = nullptr;
    MaterialAsset material_;
    bool loaded_ = false;
    QLabel* error_ = nullptr;
    std::vector<Refresher> refreshers_;
};

} // namespace lumora::editor
