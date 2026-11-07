#if defined(LUMORA_WITH_D3D12)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "renderer/d3d12/D3D12Context.hpp"

#include "core/Log.hpp"

#if defined(LUMORA_WITH_D3D12)

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvrhi/d3d12.h>

#include <algorithm>
#include <string>
#include <vector>

namespace lumora::render::d3d12 {

using Microsoft::WRL::ComPtr;

namespace {

std::string narrow(const wchar_t* text) {
    std::string out;
    for (; text != nullptr && *text != 0; ++text) out.push_back(*text < 128 ? static_cast<char>(*text) : '?');
    return out;
}

} // namespace

bool compiledIn() noexcept { return true; }

struct Context::Impl {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter3> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> graphicsQueue;
    nvrhi::DeviceHandle nvrhiDevice;
    rhi::GpuCapabilities caps;
};

Context::Context(const Options& options, nvrhi::IMessageCallback* messages, std::uint32_t bindlessCapacity)
    : impl_(std::make_unique<Impl>()) {
    if (options.validation) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    }
    const UINT factoryFlags = options.validation ? DXGI_CREATE_FACTORY_DEBUG : 0;
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&impl_->factory)))) {
        throw rhi::UnsupportedHardwareError("DXGI is not available");
    }
    // The adapter: WARP, the one asked for, or the first hardware adapter that takes D3D12.
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    if (options.warp) {
        ComPtr<IDXGIAdapter1> warp;
        if (SUCCEEDED(impl_->factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) adapters.push_back(warp);
    } else {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; impl_->factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            a->GetDesc1(&desc);
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 || options.adapterIndex) adapters.push_back(a);
            a.Reset();
        }
        if (options.adapterIndex && *options.adapterIndex < adapters.size()) {
            adapters = {adapters[*options.adapterIndex]};
        }
    }
    for (const auto& a : adapters) {
        ComPtr<ID3D12Device> device;
        if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            impl_->device = device;
            a.As(&impl_->adapter);
            break;
        }
    }
    if (!impl_->device) throw rhi::UnsupportedHardwareError("no Direct3D 12 adapter (feature level 12_0)");

    // Shader model 6.5 and resource binding tier 2 (unbounded descriptor arrays) are required.
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_5};
    if (FAILED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
        model.HighestShaderModel < D3D_SHADER_MODEL_6_5) {
        throw rhi::UnsupportedHardwareError("the Direct3D 12 adapter has no shader model 6.5");
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    impl_->device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts));
    if (opts.ResourceBindingTier < D3D12_RESOURCE_BINDING_TIER_2) {
        throw rhi::UnsupportedHardwareError("the Direct3D 12 adapter has resource binding tier 1");
    }

    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(impl_->device->CreateCommandQueue(&queue, IID_PPV_ARGS(&impl_->graphicsQueue)))) {
        throw rhi::VulkanError("could not create the Direct3D 12 graphics queue", VK_ERROR_INITIALIZATION_FAILED);
    }

    nvrhi::d3d12::DeviceDesc desc;
    desc.errorCB = messages;
    desc.pDevice = impl_->device.Get();
    desc.pGraphicsCommandQueue = impl_->graphicsQueue.Get();
    desc.shaderResourceViewHeapSize = bindlessCapacity * 2 + 16384;  // the bindless table (twice: 2D and 3D views) + the rest
    desc.samplerHeapSize = 2048;
    impl_->nvrhiDevice = nvrhi::d3d12::createDevice(desc);
    if (!impl_->nvrhiDevice) throw rhi::VulkanError("NVRHI could not use the Direct3D 12 device", VK_ERROR_INITIALIZATION_FAILED);

    DXGI_ADAPTER_DESC1 adapterDesc{};
    impl_->adapter->GetDesc1(&adapterDesc);
    rhi::GpuCapabilities& c = impl_->caps;
    c.name = narrow(adapterDesc.Description);
    c.driverName = "Direct3D 12";
    c.vendorId = adapterDesc.VendorId;
    c.deviceId = adapterDesc.DeviceId;
    c.type = (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ? VK_PHYSICAL_DEVICE_TYPE_CPU
                                                                     : VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    c.deviceLocalBytes = adapterDesc.DedicatedVideoMemory;
    c.timestamps = true;
    c.samplerAnisotropy = true;
    c.maxAnisotropy = 16.0f;
    c.textureCompressionBC = true;  // every D3D12 device
    c.maxImageDimension2D = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    c.memoryBudget = true;
    c.swapchain = true;
    c.maxPushConstantsSize = 128;
    c.maxBindlessSampledImages = 1000000;
    c.maxBindlessSamplers = 2048;
    log::info("D3D12", "using {} ({}), {} MiB VRAM", c.name, options.warp ? "WARP" : "hardware", c.deviceLocalBytes >> 20);
}

Context::~Context() {
    if (impl_->nvrhiDevice) impl_->nvrhiDevice->waitForIdle();
    impl_->nvrhiDevice = nullptr;
}

nvrhi::IDevice* Context::device() const noexcept { return impl_->nvrhiDevice; }
const rhi::GpuCapabilities& Context::caps() const noexcept { return impl_->caps; }
void* Context::dxgiFactory() const noexcept { return impl_->factory.Get(); }
void* Context::graphicsQueue() const noexcept { return impl_->graphicsQueue.Get(); }

rhi::Device::MemoryUsage Context::memoryUsage() const {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    rhi::Device::MemoryUsage usage;
    if (SUCCEEDED(impl_->adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        usage.usage = info.CurrentUsage;
        usage.budget = info.Budget;
    }
    return usage;
}

// ---- Swapchain ---------------------------------------------------------------------------------

constexpr UINT kBuffers = 3;

struct Swapchain::Impl {
    ComPtr<IDXGISwapChain3> swapchain;
    std::vector<ComPtr<ID3D12Resource>> buffers;
    bool tearing = false;
    void fetch() {
        buffers.assign(kBuffers, nullptr);
        for (UINT i = 0; i < kBuffers; ++i) swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
    }
};

Swapchain::Swapchain(Context& context, void* window, VkExtent2D extent, rhi::PresentPolicy policy)
    : impl_(std::make_unique<Impl>()), extent_(extent), policy_(policy) {
    auto* factory = static_cast<IDXGIFactory4*>(context.dxgiFactory());
    ComPtr<IDXGIFactory5> factory5;
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
        factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    }
    impl_->tearing = tearing == TRUE;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = std::max(extent.width, 1u);
    desc.Height = std::max(extent.height, 1u);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBuffers;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = impl_->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> swapchain;
    if (FAILED(factory->CreateSwapChainForHwnd(static_cast<ID3D12CommandQueue*>(context.graphicsQueue()), static_cast<HWND>(window),
                                               &desc, nullptr, nullptr, &swapchain)) ||
        FAILED(swapchain.As(&impl_->swapchain))) {
        throw rhi::VulkanError("could not create the DXGI swapchain", VK_ERROR_INITIALIZATION_FAILED);
    }
    factory->MakeWindowAssociation(static_cast<HWND>(window), DXGI_MWA_NO_ALT_ENTER);
    extent_ = {desc.Width, desc.Height};
    impl_->fetch();
}

Swapchain::~Swapchain() = default;

bool Swapchain::resize(VkExtent2D extent) {
    if (extent.width == 0 || extent.height == 0) return false;
    impl_->buffers.clear();
    if (FAILED(impl_->swapchain->ResizeBuffers(kBuffers, extent.width, extent.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                                               impl_->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0))) {
        return false;
    }
    extent_ = extent;
    impl_->fetch();
    return true;
}

std::uint32_t Swapchain::imageCount() const noexcept { return kBuffers; }
std::uint32_t Swapchain::currentImage() const { return impl_->swapchain->GetCurrentBackBufferIndex(); }
void* Swapchain::image(std::uint32_t index) const { return impl_->buffers.at(index).Get(); }

bool Swapchain::present() {
    const bool vsync = policy_ != rhi::PresentPolicy::Uncapped;
    const HRESULT hr = impl_->swapchain->Present(vsync ? 1 : 0, !vsync && impl_->tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
}

} // namespace lumora::render::d3d12

#else  // without the D3D12 back end

namespace lumora::render::d3d12 {

bool compiledIn() noexcept { return false; }

struct Context::Impl {};

Context::Context(const Options&, nvrhi::IMessageCallback*, std::uint32_t) {
    throw rhi::UnsupportedHardwareError("this build has no Direct3D 12 back end (Windows, LUMORA_WITH_D3D12)");
}
Context::~Context() = default;
nvrhi::IDevice* Context::device() const noexcept { return nullptr; }
const rhi::GpuCapabilities& Context::caps() const noexcept {
    static const rhi::GpuCapabilities none;
    return none;
}
rhi::Device::MemoryUsage Context::memoryUsage() const { return {}; }
void* Context::dxgiFactory() const noexcept { return nullptr; }
void* Context::graphicsQueue() const noexcept { return nullptr; }

struct Swapchain::Impl {};
Swapchain::Swapchain(Context&, void*, VkExtent2D, rhi::PresentPolicy) {
    throw rhi::UnsupportedHardwareError("this build has no Direct3D 12 back end");
}
Swapchain::~Swapchain() = default;
bool Swapchain::resize(VkExtent2D) { return false; }
std::uint32_t Swapchain::imageCount() const noexcept { return 0; }
std::uint32_t Swapchain::currentImage() const { return 0; }
void* Swapchain::image(std::uint32_t) const { return nullptr; }
bool Swapchain::present() { return false; }

} // namespace lumora::render::d3d12

#endif
