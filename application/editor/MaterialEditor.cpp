#include "editor/MaterialEditor.hpp"

#include "assets/AssetLibrary.hpp"
#include "common/ProjectUi.hpp"

#include <QFormLayout>
#include <QLabel>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>

namespace lumora::editor {

namespace {

constexpr int kPreviewSize = 160;

struct MaterialField {
    const char* group;  // heading shown above the first field of each group
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
        auto flag = [](bool MaterialAsset::*member) {
            return std::pair{[member](const MaterialAsset& m) { return FieldValue(m.*member); },
                             [member](MaterialAsset& m, const FieldValue& v) { m.*member = std::get<bool>(v); }};
        };
        // One component of a 2D value (tiling, offset).
        auto component = [](glm::vec2 MaterialAsset::*member, int i) {
            return std::pair{[member, i](const MaterialAsset& m) { return FieldValue((m.*member)[i]); },
                             [member, i](MaterialAsset& m, const FieldValue& v) { (m.*member)[i] = std::get<float>(v); }};
        };
        const char* group = "Surface";
        auto add = [&f, &group](FieldDesc d, auto accessors) { f.push_back({group, std::move(d), accessors.first, accessors.second}); };
        add(describe("baseColor", "Base Color", FieldKind::Color), color(&MaterialAsset::baseColor));
        add(describe("metallic", "Metallic", FieldKind::Float), number(&MaterialAsset::metallic));
        add(describe("roughness", "Roughness", FieldKind::Float, 0.02f), number(&MaterialAsset::roughness));
        add(describe("emissive", "Emissive", FieldKind::Color), color(&MaterialAsset::emissive));
        add(describe("emissiveStrength", "Emissive Strength", FieldKind::Float, 0.0f, 1000.0f, 0.1f),
            number(&MaterialAsset::emissiveStrength));
        f.push_back({group, describe("pattern", "Pattern", FieldKind::Enum, 0, 0, 0, {"None", "Grid", "Checker"}),
                     [](const MaterialAsset& m) { return FieldValue(static_cast<int>(m.pattern)); },
                     [](MaterialAsset& m, const FieldValue& v) { m.pattern = std::get<int>(v); }});
        f.push_back({group, describe("alphaMode", "Alpha", FieldKind::Enum, 0, 0, 0, {"Opaque", "Cut Out", "Transparent"}),
                     [](const MaterialAsset& m) { return FieldValue(static_cast<int>(m.alphaMode)); },
                     [](MaterialAsset& m, const FieldValue& v) { m.alphaMode = static_cast<MaterialAsset::AlphaMode>(std::get<int>(v)); }});
        add(describe("alphaCutoff", "Alpha Cutoff", FieldKind::Float, 0.01f, 1.0f), number(&MaterialAsset::alphaCutoff));
        add(describe("opacity", "Opacity", FieldKind::Float), number(&MaterialAsset::opacity));
        add(describe("doubleSided", "Two Sided", FieldKind::Bool), flag(&MaterialAsset::doubleSided));
        group = "Maps";
        add(describe("baseColorTexture", "Base Color Map", FieldKind::Asset), text(&MaterialAsset::baseColorTexture));
        add(describe("metallicRoughnessTexture", "Metal/Rough Map", FieldKind::Asset),
            text(&MaterialAsset::metallicRoughnessTexture));
        add(describe("normalTexture", "Normal Map", FieldKind::Asset), text(&MaterialAsset::normalTexture));
        add(describe("normalStrength", "Normal Strength", FieldKind::Float, 0.0f, 4.0f), number(&MaterialAsset::normalStrength));
        add(describe("emissiveTexture", "Emissive Map", FieldKind::Asset), text(&MaterialAsset::emissiveTexture));
        add(describe("occlusionTexture", "Occlusion Map", FieldKind::Asset), text(&MaterialAsset::occlusionTexture));
        add(describe("uvScaleU", "Tiling U", FieldKind::Float, -100.0f, 100.0f, 0.05f), component(&MaterialAsset::uvScale, 0));
        add(describe("uvScaleV", "Tiling V", FieldKind::Float, -100.0f, 100.0f, 0.05f), component(&MaterialAsset::uvScale, 1));
        add(describe("uvOffsetU", "Offset U", FieldKind::Float, -100.0f, 100.0f, 0.01f), component(&MaterialAsset::uvOffset, 0));
        add(describe("uvOffsetV", "Offset V", FieldKind::Float, -100.0f, 100.0f, 0.01f), component(&MaterialAsset::uvOffset, 1));
        add(describe("uvRotation", "UV Rotation", FieldKind::Float, -360.0f, 360.0f, 1.0f), number(&MaterialAsset::uvRotation));
        group = "Layers";
        add(describe("clearcoat", "Clear Coat", FieldKind::Float), number(&MaterialAsset::clearcoat));
        add(describe("clearcoatRoughness", "Coat Roughness", FieldKind::Float), number(&MaterialAsset::clearcoatRoughness));
        add(describe("sheenColor", "Sheen", FieldKind::Color), color(&MaterialAsset::sheenColor));
        add(describe("sheenRoughness", "Sheen Roughness", FieldKind::Float), number(&MaterialAsset::sheenRoughness));
        add(describe("specularFactor", "Specular", FieldKind::Float), number(&MaterialAsset::specularFactor));
        add(describe("specularColor", "Specular Tint", FieldKind::Color), color(&MaterialAsset::specularColor));
        add(describe("ior", "IOR", FieldKind::Float, 1.0f, 3.0f), number(&MaterialAsset::ior));
        group = "Glass";
        add(describe("transmission", "Transmission", FieldKind::Float), number(&MaterialAsset::transmission));
        add(describe("thickness", "Thickness", FieldKind::Float, 0.0f, 100.0f), number(&MaterialAsset::thickness));
        add(describe("attenuationColor", "Absorption Color", FieldKind::Color), color(&MaterialAsset::attenuationColor));
        add(describe("attenuationDistance", "Absorption Distance", FieldKind::Float, 0.0f, 1000.0f, 0.05f),
            number(&MaterialAsset::attenuationDistance));
        return f;
    }();
    return fields;
}

} // namespace

MaterialEditor::MaterialEditor(QString file, std::string key, AssetLibrary* assets, AssetPicker pickAsset, MaterialPreview preview,
                               QWidget* parent)
    : QWidget(parent), file_(std::move(file)), key_(std::move(key)), assets_(assets), previewRenderer_(std::move(preview)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    if (previewRenderer_) {
        preview_ = new QLabel;
        preview_->setObjectName(QStringLiteral("materialPreview"));
        preview_->setAlignment(Qt::AlignCenter);
        preview_->setMinimumHeight(kPreviewSize);
        layout->addWidget(preview_);
        // Edits come in bursts (dragging a slider): render once they pause.
        previewTimer_ = new QTimer(this);
        previewTimer_->setSingleShot(true);
        previewTimer_->setInterval(120);
        connect(previewTimer_, &QTimer::timeout, this, &MaterialEditor::renderPreview);
    }
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
    const char* group = nullptr;
    for (const MaterialField& field : materialFields()) {
        if (group == nullptr || std::string_view(group) != field.group) {
            group = field.group;
            auto* heading = new QLabel(tr(group));
            heading->setStyleSheet(QStringLiteral("font-weight: 600; padding-top: 6px;"));
            form->addRow(heading);
        }
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
    renderPreview();
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
    if (previewTimer_ != nullptr) previewTimer_->start();
    emit changed();
}

void MaterialEditor::renderPreview() {
    if (!previewRenderer_ || preview_ == nullptr || !loaded_) return;
    const QImage image = previewRenderer_(file_, kPreviewSize);
    if (!image.isNull()) preview_->setPixmap(QPixmap::fromImage(image));
}

QImage MaterialEditor::previewImage() const {
    return preview_ != nullptr ? preview_->pixmap().toImage() : QImage();
}

} // namespace lumora::editor
