# Engine modules. Each module is a static library with an explicit dependency list;
# nothing in engine/ may depend on Qt or on application/.

function(lumora_engine_module name)
    cmake_parse_arguments(M "" "" "SOURCES;DEPS" ${ARGN})
    add_library(lumora_${name} ${LUMORA_MODULE_TYPE} ${M_SOURCES})
    lumora_module_setup(lumora_${name})
    add_library(Lumora::${name} ALIAS lumora_${name})
    target_include_directories(lumora_${name} PUBLIC ${CMAKE_SOURCE_DIR}/engine ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(lumora_${name} PUBLIC ${M_DEPS} PRIVATE lumora_compile_options)
    # Propagate shared definitions (GLM config, platform macros) to dependents.
    target_compile_definitions(lumora_${name} PUBLIC $<TARGET_PROPERTY:lumora_compile_options,INTERFACE_COMPILE_DEFINITIONS>)
endfunction()

configure_file(core/Version.hpp.in ${CMAKE_BINARY_DIR}/generated/core/Version.hpp @ONLY)

lumora_engine_module(core
    SOURCES
        core/Log.cpp
        core/Uuid.cpp
        core/FileSystem.cpp
        core/JobSystem.cpp
        platform/Paths.cpp
    DEPS Threads::Threads)

lumora_engine_module(input
    SOURCES input/InputState.cpp
    DEPS lumora_core glm::glm)

lumora_engine_module(assets
    SOURCES
        assets/Primitives.cpp
        assets/GltfImporter.cpp
        assets/CgltfImpl.cpp
        assets/AssetLibrary.cpp
        assets/AssetImport.cpp
        assets/Image.cpp
        assets/StbImpl.cpp
        assets/Material.cpp
    DEPS lumora_core glm::glm cgltf stb nlohmann_json::nlohmann_json)

lumora_engine_module(scene
    SOURCES
        scene/Components.cpp
        scene/Scene.cpp
        scene/ComponentRegistry.cpp
        scene/SceneSerializer.cpp
        scene/Prefab.cpp
        scene/ScenePatch.cpp
        scene/Camera.cpp
        scene/Picking.cpp
        scene/ModelInstance.cpp
        scene/PhysicsHelpers.cpp
        scene/Particles.cpp
    DEPS lumora_core lumora_assets lumora_input EnTT::EnTT glm::glm nlohmann_json::nlohmann_json)

lumora_engine_module(project
    SOURCES
        project/Project.cpp
        project/ProjectTemplates.cpp
        project/ProjectRegistry.cpp
    DEPS lumora_scene)

lumora_engine_module(renderer
    SOURCES
        renderer/vulkan/VulkanCommon.cpp
        renderer/vulkan/Instance.cpp
        renderer/vulkan/Device.cpp
        renderer/vulkan/Resources.cpp
        renderer/vulkan/Swapchain.cpp
        renderer/vulkan/Pipeline.cpp
        renderer/vulkan/BindlessTable.cpp
        renderer/vulkan/UploadQueue.cpp
        renderer/vulkan/VmaImpl.cpp
        renderer/RenderGraph.cpp
        renderer/GpuProfiler.cpp
        renderer/TransientImagePool.cpp
        renderer/MeshCache.cpp
        renderer/TextureCache.cpp
        renderer/EnvironmentMap.cpp
        renderer/EnvironmentCache.cpp
        renderer/LightGrid.cpp
        renderer/HudRenderer.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/generated/HudFont.cpp
        renderer/RenderContext.cpp
        renderer/Renderer.cpp
        renderer/SceneRenderer.cpp
        renderer/ParallelCommandPools.cpp
        renderer/Overlay.cpp
    DEPS lumora_scene Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)

# The HUD font (DejaVu Sans, Latin subset) compiled into the renderer as a byte array.
set(LUMORA_HUD_FONT ${CMAKE_CURRENT_SOURCE_DIR}/renderer/fonts/DejaVuSans-Subset.ttf)
file(READ ${LUMORA_HUD_FONT} lumora_font_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," lumora_font_bytes "${lumora_font_hex}")
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/HudFont.cpp CONTENT
"// Generated from renderer/fonts/DejaVuSans-Subset.ttf (see fonts/LICENSE-DejaVu.txt).
#include <cstddef>
namespace lumora::render {
extern const unsigned char kHudFont[];
extern const std::size_t kHudFontSize;
const unsigned char kHudFont[] = {${lumora_font_bytes}};
const std::size_t kHudFontSize = sizeof(kHudFont);
}
")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${LUMORA_HUD_FONT})
target_link_libraries(lumora_renderer PRIVATE stb)
