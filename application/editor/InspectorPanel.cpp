#include "editor/InspectorPanel.hpp"

#include "editor/AddComponentMenu.hpp"

#include "common/Theme.hpp"
#include "editor/EditorContext.hpp"
#include "editor/EntityFactory.hpp"
#include "editor/MaterialEditor.hpp"
#include "assets/AssetLibrary.hpp"
#include "common/ProjectUi.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Scene.hpp"

#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <span>

namespace lumora::editor {

namespace {

QFormLayout* makeForm(QWidget* body) {
    auto* form = new QFormLayout(body);
    form->setContentsMargins(8, 8, 4, 4);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    return form;
}

QIcon componentIcon(const std::string& id) {
    static const std::pair<const char*, const char*> kIcons[] = {
        {"Transform", "entity"}, {"MeshRenderer", "cube"}, {"Light", "light"}, {"Camera", "camera"}, {"Rotator", "rotate"},
        {"RigidBody", "sparkle"}, {"Collider", "grid"}, {"ParticleEmitter", "sparkle"}};
    for (const auto& [component, icon] : kIcons) {
        if (id == component) return app::theme::icon(QString::fromLatin1(icon));
    }
    return app::theme::icon(QStringLiteral("settings"));
}

bool usesMaterial(const Scene& scene, entt::entity e) {
    const auto* mesh = scene.registry().try_get<MeshRendererComponent>(e);
    return mesh != nullptr && !mesh->material.empty();
}

/// Mesh Renderer fields a material asset replaces.
bool overriddenByMaterial(const std::string& field) {
    static const char* kFields[] = {"baseColor", "metallic", "roughness", "emissive", "emissiveStrength", "pattern",
                                    "baseColorTexture", "metallicRoughnessTexture", "normalTexture", "emissiveTexture"};
    return std::ranges::any_of(kFields, [&](const char* f) { return field == f; });
}

QLabel* fieldLabel(const QString& text) {
    auto* label = new QLabel(text);
    label->setProperty("muted", true);
    label->setMinimumWidth(92);
    return label;
}

} // namespace

InspectorPanel::InspectorPanel(EditorContext& ctx, AssetPicker pickAsset, QWidget* parent)
    : QWidget(parent), ctx_(ctx), pickAsset_(std::move(pickAsset)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    scroll_ = new QScrollArea;
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll_);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(100);  // live values while simulating
    connect(refreshTimer_, &QTimer::timeout, this, &InspectorPanel::refreshValues);
    refreshTimer_->start();

    connect(&ctx_, &EditorContext::selectionChanged, this, [this] {
        assetPath_.clear();  // an entity (or nothing) was selected: show that again
        rebuild();
    });
    connect(&ctx_, &EditorContext::sceneReplaced, this, &InspectorPanel::rebuild);
    connect(&ctx_, &EditorContext::structureChanged, this, [this] {
        // Component add/remove or deletion: rebuild; renames only need a value refresh.
        rebuild();
    });
    connect(&ctx_, &EditorContext::valuesChanged, this, [this](QObject* source) {
        if (source != this) refreshValues();
    });
    rebuild();
}

void InspectorPanel::clearContent() {
    refreshers_.clear();
    nameEdit_ = nullptr;
    content_ = new QWidget;
    contentLayout_ = new QVBoxLayout(content_);
    contentLayout_->setContentsMargins(8, 8, 8, 8);
    contentLayout_->setSpacing(6);
    scroll_->setWidget(content_);  // deletes the previous content widget
}

void InspectorPanel::setProject(AssetLibrary* assets, QString projectRoot) {
    assets_ = assets;
    projectRoot_ = std::move(projectRoot);
}

void InspectorPanel::showAsset(const QString& absolutePath) {
    const bool material = absolutePath.endsWith(QString::fromLatin1(kMaterialExtension));
    if (!material && assetPath_.isEmpty()) return;  // nothing to show for this file
    assetPath_ = material ? absolutePath : QString();
    rebuild();
}

QWidget* InspectorPanel::materialSection(const std::string& key) {
    const QString file = QDir(projectRoot_).absoluteFilePath(QString::fromStdString(key));
    auto* section = new Section(tr("Material  ·  %1").arg(QFileInfo(file).completeBaseName()),
                                app::theme::icon(QStringLiteral("sphere")), false);
    auto* layout = new QVBoxLayout(section->body());
    layout->setContentsMargins(0, 0, 0, 0);
    auto* hint = new QLabel(tr("Shared: changes apply to every object using %1.").arg(QString::fromStdString(key)));
    hint->setProperty("muted", true);
    hint->setWordWrap(true);
    hint->setContentsMargins(8, 6, 8, 0);
    layout->addWidget(hint);
    layout->addWidget(new MaterialEditor(file, key, assets_, pickAsset_));
    return section;
}

void InspectorPanel::buildMaterialAsset() {
    const QString relative = QDir(projectRoot_).relativeFilePath(assetPath_);
    auto* title = new QLabel(QFileInfo(assetPath_).completeBaseName());
    title->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 11pt; padding: 2px 2px 2px 2px;"));
    contentLayout_->addWidget(title);
    auto* path = new QLabel(relative);
    path->setProperty("muted", true);
    path->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 8pt; padding-left: 2px;"));
    contentLayout_->addWidget(path);
    contentLayout_->addWidget(materialSection(relative.toStdString()));
    auto* hint = new QLabel(tr("Drag the material from the Content Browser onto an object in the viewport or the "
                               "Hierarchy, or choose it in a Mesh Renderer's Material field."));
    hint->setProperty("muted", true);
    hint->setWordWrap(true);
    contentLayout_->addWidget(hint);
}

void InspectorPanel::rebuild() {
    clearContent();
    if (!assetPath_.isEmpty()) {
        buildMaterialAsset();
    } else if (ctx_.scene() == nullptr) {
        auto* label = new QLabel(tr("No scene loaded"));
        label->setProperty("muted", true);
        contentLayout_->addWidget(label);
    } else if (ctx_.selectedEntity() == entt::null) {
        buildEnvironment();
    } else {
        buildEntity();
    }
    contentLayout_->addStretch();
}

void InspectorPanel::buildEntity() {
    Scene& scene = *ctx_.scene();
    const entt::entity e = ctx_.selectedEntity();
    const Uuid id = scene.uuidOf(e);

    // Header: icon, editable name, UUID.
    auto* header = new QWidget;
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(2, 2, 2, 6);
    auto* icon = new QLabel;
    icon->setPixmap(entityIcon(scene, e).pixmap(22, 22));
    nameEdit_ = new QLineEdit(QString::fromStdString(scene.nameOf(e)));
    nameEdit_->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 11pt;"));
    headerLayout->addWidget(icon);
    headerLayout->addWidget(nameEdit_, 1);
    contentLayout_->addWidget(header);
    auto* uuid = new QLabel(QStringLiteral("UUID  %1").arg(QString::fromStdString(id.toString())));
    uuid->setProperty("muted", true);
    uuid->setTextInteractionFlags(Qt::TextSelectableByMouse);
    uuid->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 8pt; padding-left: 4px;"));
    contentLayout_->addWidget(uuid);
    if (ctx_.selection().size() > 1) {
        auto* multi = new QLabel(tr("%n entities selected: changes apply to all of them that have the component.",
                                    nullptr, static_cast<int>(ctx_.selection().size())));
        multi->setWordWrap(true);
        multi->setStyleSheet(QStringLiteral("color: #e8b84a; padding: 4px 4px 2px 4px;"));
        contentLayout_->addWidget(multi);
    }
    connect(nameEdit_, &QLineEdit::editingFinished, this, [this, id] {
        if (nameEdit_ != nullptr) ctx_.renameEntity(id, nameEdit_->text());
    });

    const ComponentRegistry& registry = ComponentRegistry::builtin();
    for (const ComponentDesc& desc : registry.components()) {
        if (!desc.has(scene.registry(), e)) continue;
        auto* section = new Section(QString::fromStdString(desc.label), componentIcon(desc.id), desc.removable);
        auto* form = makeForm(section->body());
        for (const FieldDesc& field : desc.fields) {
            // Getter/setter resolve the entity by UUID each time: the panel may outlive the handle.
            FieldGetter get = [this, id, &field] {
                Scene* s = ctx_.scene();
                const entt::entity ent = s ? s->findByUuid(id) : entt::entity{entt::null};
                if (ent == entt::null) {
                    switch (field.kind) {
                        case FieldKind::Bool: return FieldValue(false);
                        case FieldKind::Int:
                        case FieldKind::Enum: return FieldValue(0);
                        case FieldKind::Float: return FieldValue(0.0f);
                        case FieldKind::Vec3:
                        case FieldKind::Color: return FieldValue(glm::vec3(0.0f));
                        case FieldKind::String:
                        case FieldKind::Asset: return FieldValue(std::string{});
                    }
                }
                return field.get(s->registry(), ent);
            };
            FieldSetter set = [this, id, &desc, &field](const FieldValue& v) {
                // With several entities selected, a value goes to all that have the component.
                if (ctx_.selection().size() > 1) {
                    ctx_.setFieldOnSelection(desc, field, v);
                } else {
                    ctx_.setField(id, desc, field, v);
                }
                // Choosing or clearing a material changes which fields apply.
                if (field.id == "material") QTimer::singleShot(0, this, &InspectorPanel::rebuild);
            };
            QWidget* editor = createFieldEditor(field, std::move(get), std::move(set), refreshers_, pickAsset_);
            form->addRow(fieldLabel(QString::fromStdString(field.label)), editor);
            if (desc.id == "MeshRenderer" && usesMaterial(scene, e) && overriddenByMaterial(field.id)) {
                editor->setEnabled(false);
                editor->setToolTip(tr("Set by the material"));
            }
        }
        connect(section, &Section::removeRequested, this, [this, id, &desc] { ctx_.removeComponent(id, desc); });
        contentLayout_->addWidget(section);
        if (desc.id == "MeshRenderer" && usesMaterial(scene, e) && assets_ != nullptr) {
            contentLayout_->addWidget(materialSection(scene.registry().get<MeshRendererComponent>(e).material));
        }
    }

    // Add Component
    auto* add = new QPushButton(app::theme::icon(QStringLiteral("plus")), tr("Add Component"));
    std::vector<AddComponentMenu::Entry> entries;
    for (const ComponentDesc& desc : registry.components()) {
        if (!desc.userAddable || desc.has(scene.registry(), e)) continue;
        entries.push_back({QString::fromStdString(desc.id), QString::fromStdString(desc.label), QString::fromStdString(desc.category)});
    }
    const bool any = !entries.empty();
    auto* menu = new AddComponentMenu(std::move(entries), add);
    connect(menu, &AddComponentMenu::chosen, this, [this, id, &registry](const QString& component) {
        if (const ComponentDesc* desc = registry.find(component.toStdString())) ctx_.addComponent(id, *desc);
    });
    add->setMenu(menu);
    add->setEnabled(any);
    contentLayout_->addSpacing(6);
    contentLayout_->addWidget(add);
}

void InspectorPanel::buildEnvironment() {
    auto* title = new QLabel(tr("Scene Environment"));
    title->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 11pt; padding: 2px 2px 6px 2px;"));
    contentLayout_->addWidget(title);
    auto* hint = new QLabel(tr("Nothing selected. These settings apply to the whole scene."));
    hint->setProperty("muted", true);
    hint->setWordWrap(true);
    contentLayout_->addWidget(hint);

    struct EnvField {
        const char* label;
        FieldKind kind;
        float min, max, step;
        std::function<FieldValue(const EnvironmentSettings&)> get;
        std::function<void(EnvironmentSettings&, const FieldValue&)> set;
        const char* id = "";  // asset pickers choose by id
    };
    auto addSection = [this](const QString& sectionTitle, const QString& iconName, std::span<const EnvField> fields) {
        auto* section = new Section(sectionTitle, app::theme::icon(iconName), false);
        auto* form = makeForm(section->body());
        for (const EnvField& f : fields) {
            FieldDesc desc;
            desc.id = f.id;
            desc.kind = f.kind;
            desc.min = f.min;
            desc.max = f.max;
            desc.step = f.step;
            FieldGetter get = [this, getter = f.get] {
                return ctx_.scene() ? getter(ctx_.scene()->environment()) : FieldValue(0.0f);
            };
            FieldSetter set = [this, setter = f.set, label = f.label](const FieldValue& v) {
                ctx_.setEnvironment(tr(label), [&setter, &v](EnvironmentSettings& env) { setter(env, v); });
            };
            form->addRow(fieldLabel(tr(f.label)), createFieldEditor(desc, std::move(get), std::move(set), refreshers_));
        }
        contentLayout_->addWidget(section);
    };

    const EnvField sky[] = {
        {"Zenith", FieldKind::Color, 0, 1, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.skyZenithColor); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.skyZenithColor = std::get<glm::vec3>(v); }},
        {"Horizon", FieldKind::Color, 0, 1, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.skyHorizonColor); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.skyHorizonColor = std::get<glm::vec3>(v); }},
        {"Ground", FieldKind::Color, 0, 1, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.groundColor); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.groundColor = std::get<glm::vec3>(v); }},
        {"Ambient", FieldKind::Float, 0, 10, 0.05f, [](const EnvironmentSettings& e) { return FieldValue(e.ambientIntensity); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.ambientIntensity = std::get<float>(v); }},
    };
    const EnvField atmosphere[] = {
        {"Fog Density", FieldKind::Float, 0, 0.2f, 0.001f, [](const EnvironmentSettings& e) { return FieldValue(e.fogDensity); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.fogDensity = std::get<float>(v); }},
        {"Fog Falloff", FieldKind::Float, 0.001f, 2, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.fogHeightFalloff); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.fogHeightFalloff = std::get<float>(v); }},
        {"Sun Shafts", FieldKind::Float, 0, 4, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.sunShafts); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.sunShafts = std::get<float>(v); }},
    };
    const EnvField post[] = {
        {"Exposure", FieldKind::Float, 0.01f, 16, 0.05f, [](const EnvironmentSettings& e) { return FieldValue(e.exposure); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.exposure = std::get<float>(v); }},
        {"Bloom", FieldKind::Float, 0, 1, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.bloomIntensity); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.bloomIntensity = std::get<float>(v); }},
        {"Vignette", FieldKind::Float, 0, 1, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.vignette); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.vignette = std::get<float>(v); }},
        {"Contrast", FieldKind::Float, 0.2f, 3, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.contrast); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.contrast = std::get<float>(v); }},
        {"Saturation", FieldKind::Float, 0, 3, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.saturation); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.saturation = std::get<float>(v); }},
        {"Temperature", FieldKind::Float, -1, 1, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.temperature); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.temperature = std::get<float>(v); }},
        {"Tint", FieldKind::Float, -1, 1, 0.01f, [](const EnvironmentSettings& e) { return FieldValue(e.tint); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.tint = std::get<float>(v); }},
        {"Depth of Field", FieldKind::Float, 0, 32, 0.5f, [](const EnvironmentSettings& e) { return FieldValue(e.dofBlur); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.dofBlur = std::get<float>(v); }},
        {"Focus Distance", FieldKind::Float, 0.1f, 1000, 0.1f, [](const EnvironmentSettings& e) { return FieldValue(e.focusDistance); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.focusDistance = std::get<float>(v); }},
        {"Focus Range", FieldKind::Float, 0.01f, 1000, 0.1f, [](const EnvironmentSettings& e) { return FieldValue(e.focusRange); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.focusRange = std::get<float>(v); }},
    };
    const EnvField environmentMap[] = {
        {"HDR Sky", FieldKind::Asset, 0, 0, 0, [](const EnvironmentSettings& e) { return FieldValue(e.skyTexture); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.skyTexture = std::get<std::string>(v); }, "skyHdr"},
        {"Intensity", FieldKind::Float, 0, 20, 0.05f, [](const EnvironmentSettings& e) { return FieldValue(e.skyIntensity); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.skyIntensity = std::get<float>(v); }},
        {"Rotation", FieldKind::Float, -180, 180, 1, [](const EnvironmentSettings& e) { return FieldValue(e.skyRotationDeg); },
         [](EnvironmentSettings& e, const FieldValue& v) { e.skyRotationDeg = std::get<float>(v); }},
    };
    addSection(tr("Sky and Ambient"), QStringLiteral("sun"), sky);
    addSection(tr("Environment Map"), QStringLiteral("sphere"), environmentMap);
    addSection(tr("Atmosphere"), QStringLiteral("sparkle"), atmosphere);
    addSection(tr("Post Processing"), QStringLiteral("chart"), post);
}

void InspectorPanel::refreshValues() {
    if (!isVisible()) return;
    if (nameEdit_ != nullptr && !nameEdit_->hasFocus() && ctx_.scene() != nullptr) {
        const entt::entity e = ctx_.selectedEntity();
        if (e != entt::null) {
            const QString name = QString::fromStdString(ctx_.scene()->nameOf(e));
            if (nameEdit_->text() != name) nameEdit_->setText(name);
        }
    }
    for (const Refresher& r : refreshers_) r();
}

} // namespace lumora::editor
