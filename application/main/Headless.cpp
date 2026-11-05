#include "main/Headless.hpp"

#include "assets/AssetImport.hpp"
#include "assets/AssetLibrary.hpp"
#include "scene/ModelInstance.hpp"
#include "core/Log.hpp"
#include "core/Timer.hpp"
#include "core/Version.hpp"
#include "project/Project.hpp"
#include "renderer/RenderContext.hpp"
#include "renderer/Renderer.hpp"
#include "runtime/Simulation.hpp"
#include "scene/SceneSerializer.hpp"

#include <QImage>
#include <QString>

#include <cmath>
#include <cstdio>
#include <format>
#include <filesystem>

namespace lumora::app {

namespace {

/// Locale-independent stdout output (QCoreApplication switches the C locale to the user's).
template <class... Args>
void out(std::format_string<Args...> fmt, Args&&... args) {
    const std::string text = std::format(fmt, std::forward<Args>(args)...);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

int printInfo() {
    out("{} {} ({})\n", version::kEngineName, version::kString, version::kBuildType);
    try {
        out("{}", render::RenderContext::describeDevices(false));
    } catch (const std::exception& e) {
        out("Vulkan unavailable: {}\n", e.what());
        return 2;
    }
    return 0;
}

int createProject(const AppOptions& o) {
    const ProjectTemplateInfo* templ = findProjectTemplate(o.templateKey);
    if (templ == nullptr) {
        log::error("Headless", "unknown template '{}' (empty, basic, showcase)", o.templateKey);
        return 2;
    }
    const std::filesystem::path dir(o.createDirectory);
    const std::string name = o.createName.empty() ? dir.filename().string() : o.createName;
    auto project = Project::create(dir, name, templ->id);
    if (!project) {
        log::error("Headless", "{}", project.error().message);
        return 1;
    }
    out("Created project '{}' at {}\n", project->name(), project->descriptorPath().string());
    return 0;
}

int renderProject(const AppOptions& o, const Project& project, Scene& scene) {
    render::RenderContextConfig config;
    config.requirePresentation = false;
    config.gpuIndex = o.gpuIndex;
#if defined(LUMORA_DEBUG)
    config.enableValidation = o.validation.value_or(true);
#else
    config.enableValidation = o.validation.value_or(false);
#endif
    try {
        render::RenderContext context(config);
        render::Renderer renderer(context);
        const float aspect = static_cast<float>(o.renderWidth) / static_cast<float>(o.renderHeight);
        Stopwatch timer;
        AssetLibrary assets(project.rootDir());
        render::RenderSettings settings;
        settings.assets = &assets;
        settings.blockOnAssets = true;
        auto image = renderer.capture(scene, sceneCameraView(scene, aspect), {o.renderWidth, o.renderHeight}, settings);
        if (!image) {
            log::error("Headless", "render failed: {}", image.error().message);
            return 1;
        }
        const QImage qimg(image->rgba.data(), static_cast<int>(image->width), static_cast<int>(image->height),
                          QImage::Format_RGBA8888);
        if (!qimg.save(QString::fromStdString(o.renderOutput))) {
            log::error("Headless", "could not write '{}'", o.renderOutput);
            return 1;
        }
        const auto& s = renderer.stats();
        out("Rendered '{}' ({}x{}) on {} in {:.1f} ms (GPU {:.3f} ms, {} draws, {} triangles) -> {}\n", project.name(),
            image->width, image->height, context.caps().name, timer.elapsedMs(), s.gpuFrameMs, s.scene.drawCalls,
            s.scene.triangles, o.renderOutput);
        return 0;
    } catch (const rhi::UnsupportedHardwareError& e) {
        log::error("Headless", "{}", e.what());
        return 3;
    } catch (const std::exception& e) {
        log::error("Headless", "renderer error: {}", e.what());
        return 1;
    }
}

} // namespace

int runHeadless(const AppOptions& o) {
    int status = 0;
    if (o.printInfo) status = printInfo();
    if (status == 0 && !o.createDirectory.empty()) status = createProject(o);
    if (status != 0 || o.projectPath.empty()) return status;

    auto project = Project::load(o.projectPath);
    if (!project) {
        log::error("Headless", "{}", project.error().message);
        return 1;
    }
    auto scene = SceneSerializer::load(project->defaultScenePath());
    if (!scene) {
        log::error("Headless", "{}", scene.error().message);
        return 1;
    }
    if (!o.importFiles.empty()) {
        AssetLibrary assets(project->rootDir());
        float offset = 0.0f;
        for (const std::string& file : o.importFiles) {
            auto copied = importModelFile(file, project->contentDir() / "Models");
            if (!copied) {
                log::error("Headless", "{}", copied.error().message);
                return 1;
            }
            auto model = assets.model(*copied);
            if (!model) return 1;
            const std::string relative = project->relativePath(*copied);
            const entt::entity root = instantiateModel(**scene, **model, relative, copied->stem().string());
            (*scene)->registry().get<TransformComponent>(root).position.x = offset;
            offset += 2.5f;
            out("Imported '{}' -> {} ({} meshes, {} nodes)\n", file, relative, (*model)->meshes.size(),
                (*model)->nodes.size());
        }
        if (auto saved = SceneSerializer::save(**scene, project->defaultScenePath()); !saved) {
            log::error("Headless", "{}", saved.error().message);
            return 1;
        }
    }
    if (o.validateProject) {
        out("Project '{}' OK: scene '{}', {} entities\n", project->name(), (*scene)->name(), (*scene)->entityCount());
    }
    if (o.simulateSeconds > 0.0) {
        Simulation sim(**scene);
        const auto steps = static_cast<std::uint64_t>(std::ceil(o.simulateSeconds / sim.fixedStep()));
        Stopwatch timer;
        for (std::uint64_t i = 0; i < steps; ++i) sim.step();
        out("Simulated {:.3f} s ({} fixed steps) in {:.2f} ms\n", sim.simulatedSeconds(), steps, timer.elapsedMs());
    }
    if (!o.renderOutput.empty()) {
        status = renderProject(o, *project, **scene);
    }
    return status;
}

} // namespace lumora::app
