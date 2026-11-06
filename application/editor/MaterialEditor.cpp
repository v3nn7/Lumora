#include "editor/MaterialEditor.hpp"

#include "assets/AssetLibrary.hpp"
#include "common/ProjectUi.hpp"

#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace lumora::editor {

namespace {

struct MaterialField {
    FieldDesc desc;
    std::function<FieldValue(const MaterialAsset&)> get;
    std::function<void(MaterialAsset&, const FieldValue&)> set;
};

FieldDesc describe(const char* id, const char* label, FieldKind kind, float min = 0.0f, float max = 1.0f,
                   float step = 0.01f, std::vector<std::string> labels = {}) {
    FieldDesc d;
    d.id = id;
    d.label = label;
    d.kind = kind;
    d.min = min;
    d.max = max;
    d.step = step;
    d.enumLabels = std::move(labels);
    return d;
}

/// The fields, built once: the asset pickers keep a reference to their FieldDesc.
const std::vector<MaterialField>& materialFields() {
    static const std::vector<MaterialField> fields = [] {
        std::vector<MaterialField> f;
        auto text = [](std::string MaterialAsset::*member) {
            return std::pair{[member](const MaterialAsset& m) { return FieldValue(m.*member); },
                             [member](MaterialAsset& m, const FieldValue& v) { m.*member = std::get<std::string>(v); }};
        };
        auto color = [](glm::vec3 MaterialAsset::*member) {
            return std::pair{[member](const MaterialAsset& m) { return FieldValue(m.*member); },
                             [member](MaterialAsset& m, const FieldValue& v) { m.*member = std::get<glm::vec3>(v); }};
        };
        auto number = [](float MaterialAsset::*member) {
            return std::pair{[member](const MaterialAsset& m) { return FieldValue(m.*member); },
                             [member](MaterialAsset& m, const FieldValue& v) { m.*member = std::get<float>(v); }};
        };
        auto add = [&f](FieldDesc d, auto accessors) { f.push_back({std::move(d), accessors.first, accessors.second}); };
        add(describe("baseColor", "Base Color", FieldKind::Color), color(&MaterialAsset::baseColor));
        add(describe("metallic", "Metallic", FieldKind::Float), number(&MaterialAsset::metallic));
        add(describe("roughness", "Roughness", FieldKind::Float, 0.02f), number(&MaterialAsset::roughness));
        add(describe("emissive", "Emissive", FieldKind::Color), color(&MaterialAsset::emissive));
        add(describe("emissiveStrength", "Emissive Strength", FieldKind::Float, 0.0f, 1000.0f, 0.1f),
            number(&MaterialAsset::emissiveStrength));
        f.push_back({describe("pattern", "Pattern", FieldKind::Enum, 0, 0, 0, {"None", "Grid", "Checker"}),
                     [](const MaterialAsset& m) { return FieldValue(static_cast<int>(m.pattern)); },
                     [](MaterialAsset& m, const FieldValue& v) { m.pattern = std::get<int>(v); }});
        add(describe("baseColorTexture", "Base Color Map", FieldKind::Asset), text(&MaterialAsset::baseColorTexture));
        add(describe("metallicRoughnessTexture", "Metal/Rough Map", FieldKind::Asset),
            text(&MaterialAsset::metallicRoughnessTexture));
        add(describe("normalTexture", "Normal Map", FieldKind::Asset), text(&MaterialAsset::normalTexture));
        add(describe("emissiveTexture", "Emissive Map", FieldKind::Asset), text(&MaterialAsset::emissiveTexture));
        return f;
    }();
    return fields;
}

} // namespace

MaterialEditor::MaterialEditor(QString file, std::string key, AssetLibrary* assets, AssetPicker pickAsset, QWidget* parent)
    : QWidget(parent), file_(std::move(file)), key_(std::move(key)), assets_(assets) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    error_ = new QLabel;
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: #e06c75;"));
    error_->hide();
    layout->addWidget(error_);
    auto* form = new QFormLayout;
    form->setContentsMargins(8, 8, 4, 4);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->addLayout(form);
    reload();
    for (const MaterialField& field : materialFields()) {
        FieldGetter get = [this, &field] { return field.get(material_); };
        FieldSetter set = [this, &field](const FieldValue& v) {
            field.set(material_, v);
            write();
        };
        auto* label = new QLabel(tr(field.desc.label.c_str()));
        label->setProperty("muted", true);
        label->setMinimumWidth(92);
        QWidget* editor = createFieldEditor(field.desc, std::move(get), std::move(set), refreshers_, pickAsset);
        editor->setEnabled(loaded_);
        form->addRow(label, editor);
    }
}

void MaterialEditor::reload() {
    auto loaded = loadMaterial(app::toPath(file_));
    loaded_ = loaded.has_value();
    if (loaded) {
        material_ = *loaded;
        error_->hide();
    } else {
        error_->setText(tr("Cannot read this material: %1").arg(QString::fromStdString(loaded.error().message)));
        error_->show();
    }
    for (const Refresher& r : refreshers_) r();
}

void MaterialEditor::write() {
    if (!loaded_) return;
    if (auto saved = saveMaterial(material_, app::toPath(file_)); !saved) {
        error_->setText(QString::fromStdString(saved.error().message));
        error_->show();
        return;
    }
    if (assets_ != nullptr) assets_->reloadMaterial(key_);
    emit changed();
}

} // namespace lumora::editor
