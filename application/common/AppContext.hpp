#pragma once

#include "main/CommandLine.hpp"
#include "core/Log.hpp"
#include "project/ProjectRegistry.hpp"

#include <QString>

#include <memory>

class QVulkanInstance;

namespace lumora::render {
class RenderContext;
class Renderer;
}

namespace lumora::app {

class LogBridge;

/// Application-wide services shared by Hub, Editor and Player. Vulkan is initialised lazily
/// on first use, so the Hub stays usable (with a clear message) on machines without
/// Vulkan 1.4 support.
class AppContext {
public:
    explicit AppContext(AppOptions options);
    ~AppContext();
    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;

    [[nodiscard]] const AppOptions& options() const noexcept { return options_; }

    /// Returns nullptr (and the reason in rendererError()) if Vulkan cannot be initialised.
    [[nodiscard]] render::Renderer* renderer();
    [[nodiscard]] render::RenderContext* renderContext();
    [[nodiscard]] QVulkanInstance* vulkanInstance();
    [[nodiscard]] const QString& rendererError() const noexcept { return rendererError_; }
    [[nodiscard]] bool validationEnabled() const noexcept;

    [[nodiscard]] ProjectRegistry& recentProjects() noexcept { return recent_; }
    void saveRecentProjects();

    [[nodiscard]] LogBridge& logBridge() noexcept { return *logBridge_; }

private:
    bool initRenderer();

    AppOptions options_;
    ProjectRegistry recent_;
    std::unique_ptr<LogBridge> logBridge_;
    std::shared_ptr<LogSink> logSink_;  // removed from the global logger before logBridge_ dies
    std::unique_ptr<render::RenderContext> renderContext_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<QVulkanInstance> qvk_;  // adopts renderContext_'s VkInstance; destroyed first
    bool initAttempted_ = false;
    QString rendererError_;
};

} // namespace lumora::app
