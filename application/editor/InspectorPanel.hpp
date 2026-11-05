#pragma once

#include "editor/PropertyWidgets.hpp"

#include <QWidget>

#include <vector>

class QLabel;
class QLineEdit;
class QScrollArea;
class QTimer;
class QVBoxLayout;

namespace lumora::editor {

class EditorContext;

/// Reflection-driven property editor. Shows the selected entity's components, or the scene
/// environment when nothing is selected. Values refresh live (e.g. while the simulation runs).
class InspectorPanel final : public QWidget {
    Q_OBJECT
public:
    explicit InspectorPanel(EditorContext& ctx, AssetPicker pickAsset = {}, QWidget* parent = nullptr);

private:
    void rebuild();
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
};

} // namespace lumora::editor
