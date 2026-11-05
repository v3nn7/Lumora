# Engine modules. Each module is a static library with an explicit dependency list;
# nothing in engine/ may depend on Qt or on application/.

function(lumora_engine_module name)
    cmake_parse_arguments(M "" "" "SOURCES;DEPS" ${ARGN})
    add_library(lumora_${name} STATIC ${M_SOURCES})
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
        platform/Paths.cpp
    DEPS Threads::Threads)

lumora_engine_module(input
    SOURCES input/InputState.cpp
    DEPS lumora_core glm::glm)

lumora_engine_module(assets
    SOURCES assets/Primitives.cpp
    DEPS lumora_core glm::glm)

lumora_engine_module(scene
    SOURCES
        scene/Components.cpp
        scene/Scene.cpp
        scene/ComponentRegistry.cpp
        scene/SceneSerializer.cpp
        scene/Camera.cpp
        scene/Picking.cpp
    DEPS lumora_core lumora_assets lumora_input EnTT::EnTT glm::glm nlohmann_json::nlohmann_json)

lumora_engine_module(project
    SOURCES
        project/Project.cpp
        project/ProjectTemplates.cpp
        project/ProjectRegistry.cpp
    DEPS lumora_scene)

