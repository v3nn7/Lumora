#include "common/AppContext.hpp"

#include "common/QtLogSink.hpp"
#include "core/Log.hpp"
#include "renderer/RenderContext.hpp"
#include "renderer/Renderer.hpp"

#include <QVulkanInstance>

#include <algorithm>

namespace lumora::app {

namespace {

/// Every WSI extension the loader offers; Qt picks the one matching the active platform plugin.
std::vector<std::string> surfaceExtensions() {
    const auto available = rhi::Instance::availableExtensions();
    std::vector<std::string> result;
    for (const char* name : {"VK_KHR_surface", "VK_KHR_wayland_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface",
                             "VK_KHR_win32_surface"}) {
        if (std::ranges::find(available, name) != available.end()) result.emplace_back(name);
    }
    return result;
}

} // namespace

AppContext::AppContext(AppOptions options)
    : options_(std::move(options)), recent_(ProjectRegistry::defaultStorageFile()), logBridge_(std::make_unique<LogBridge>()) {
    qRegisterMetaType<UiLogEntry>("lumora::app::UiLogEntry");
    logSink_ = std::make_shared<QtLogSink>(*logBridge_);
    Logger::instance().addSink(logSink_);
    if (auto loaded = recent_.load(); !loaded) {
        log::warn("Hub", "{}", loaded.error().message);
    }
}

AppContext::~AppContext() {
    qvk_.reset();
    renderer_.reset();
    renderContext_.reset();
    Logger::instance().removeSink(logSink_);
}

bool AppContext::validationEnabled() const noexcept {
#if defined(LUMORA_DEBUG)
    return options_.validation.value_or(true);
#else
    return options_.validation.value_or(false);
#endif
}

bool AppContext::initRenderer() {
    if (initAttempted_) return renderer_ != nullptr;
    initAttempted_ = true;
    try {
        render::RenderContextConfig config;
        config.enableValidation = validationEnabled();
        config.gpuIndex = options_.gpuIndex;
        config.instanceExtensions = surfaceExtensions();
        if (std::ranges::find(config.instanceExtensions, "VK_KHR_surface") == config.instanceExtensions.end()) {
            throw rhi::UnsupportedHardwareError("the Vulkan loader offers no VK_KHR_surface; cannot present to windows");
        }
        renderContext_ = std::make_unique<render::RenderContext>(config);
        renderer_ = std::make_unique<render::Renderer>(*renderContext_);

        qvk_ = std::make_unique<QVulkanInstance>();
        qvk_->setVkInstance(renderContext_->instance().handle());
        if (!qvk_->create()) {
            throw rhi::VulkanError("QVulkanInstance could not adopt the Vulkan instance", VK_ERROR_INITIALIZATION_FAILED);
        }
        return true;
    } catch (const std::exception& e) {
        rendererError_ = QString::fromUtf8(e.what());
        log::error("Renderer", "Vulkan initialisation failed: {}", e.what());
        qvk_.reset();
        renderer_.reset();
        renderContext_.reset();
        return false;
    }
}

render::Renderer* AppContext::renderer() { return initRenderer() ? renderer_.get() : nullptr; }

render::RenderContext* AppContext::renderContext() { return initRenderer() ? renderContext_.get() : nullptr; }

QVulkanInstance* AppContext::vulkanInstance() { return initRenderer() ? qvk_.get() : nullptr; }

void AppContext::saveRecentProjects() {
    if (auto saved = recent_.save(); !saved) {
        log::warn("Hub", "could not save recent projects: {}", saved.error().message);
    }
}

} // namespace lumora::app
