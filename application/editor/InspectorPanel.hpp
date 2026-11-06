#pragma once

#include "common/ModuleExport.hpp"

#include "editor/PropertyWidgets.hpp"

#include <QWidget>

#include <vector>

class QLabel;
class QLineEdit;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace lumora {
class AssetLibrary;
}

namespace lumora::editor {

class EditorContext;

/// Reflection-driven property editor. Shows the selected entity's components, or the scene
/// environment when nothing is selected, or an asset chosen in the Content Browser (materials).
/// Values refresh live (e.g. while the simulation runs).
class LUMORA_EDITOR_API InspectorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit InspectorPanel(EditorContext& ctx, AssetPicker pickAsset = {}, QWidget* parent = nullptr);

    /// Where material references resolve (and whose asset library re-reads edited materials).
    void setProject(AssetLibrary* assets, QString projectRoot);
    /// Shows an asset file instead of the selection (a .lmat is edited here); selecting an
    /// entity goes back to it. Other files clear the asset view.
    void showAsset(const QString& absolutePath);
    [[nodiscard]] const QString& shownAsset() const noexcept { return assetPath_; }

private:
    void rebuild();
    void buildMaterialAsset();
    [[nodiscard]] QWidget* materialSection(const std::string& key);
    void buildEntity();
    void buildEnvironment();
    void refreshValues();
    void clearContent();

    EditorContext& ctx_;
    QScrollArea* scroll_ = nullptr;
    QWidget* content_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    std::vector<Refresher> refreshers_;
    AssetPicker pickAsset_;
    AssetLibrary* assets_ = nullptr;
    QString projectRoot_;
    QString assetPath_;
};

} // namespace lumora::editor
