# Application layer: one executable (Lumora) hosting Hub, Editor, Player and headless mode.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Command-line parsing has no Qt dependency so it can be unit tested.
add_library(lumora_cli ${LUMORA_MODULE_TYPE} main/CommandLine.cpp)
lumora_module_setup(lumora_cli)
target_include_directories(lumora_cli PUBLIC ${CMAKE_SOURCE_DIR}/application)
target_link_libraries(lumora_cli PUBLIC lumora_core PRIVATE lumora_compile_options)

function(lumora_qt_module name)
    cmake_parse_arguments(M "" "" "SOURCES;DEPS" ${ARGN})
    add_library(lumora_${name} ${LUMORA_MODULE_TYPE} ${M_SOURCES})
    lumora_module_setup(lumora_${name})
    target_include_directories(lumora_${name} PUBLIC ${CMAKE_SOURCE_DIR}/application)
    target_link_libraries(lumora_${name} PUBLIC ${M_DEPS} PRIVATE lumora_compile_options)
endfunction()

lumora_qt_module(appcommon
    SOURCES
        common/AppContext.cpp
        common/QtLogSink.cpp
        common/Theme.cpp
        common/VulkanViewport.cpp
        common/ProjectUi.cpp
        common/Snapshot.cpp
        common/Preferences.cpp
        common/WaylandPointerLock.cpp
        common/resources.qrc
    DEPS lumora_project lumora_renderer lumora_runtime lumora_cli Qt6::Widgets Qt6::Svg)
if(LUMORA_WAYLAND_POINTER_LOCK)
    target_link_libraries(lumora_appcommon PRIVATE lumora_wayland_protocols)
    target_compile_definitions(lumora_appcommon PRIVATE LUMORA_HAS_WAYLAND_POINTER_LOCK=1)
endif()

# Live collaboration: session server, client, scene sync, and the host/join dialogs and panel.
lumora_qt_module(collab
    SOURCES
        collab/Protocol.cpp
        collab/AssetTransfer.cpp
        collab/CollabServer.cpp
        collab/CollabClient.cpp
        collab/SceneSync.cpp
        collab/CollabSession.cpp
        collab/CollabUi.cpp
    DEPS lumora_scene lumora_core Qt6::Network Qt6::Widgets)

add_subdirectory(launcher)

lumora_qt_module(hub
    SOURCES
        hub/HubWindow.cpp
        hub/EditorsPage.cpp
        hub/HeroBanner.cpp
        hub/HubSettingsPage.cpp
        hub/ProjectDetailsPanel.cpp
        hub/NewProjectDialog.cpp
        hub/ProjectListModel.cpp
        hub/ProjectCardDelegate.cpp
    DEPS lumora_appcommon lumora_launcher)

lumora_qt_module(editor
    SOURCES
        editor/EditorContext.cpp
        editor/Commands.cpp
        editor/EditorWindow.cpp
        editor/EditorViewportClient.cpp
        editor/TransformGizmo.cpp
        editor/EntityFactory.cpp
        editor/SceneHierarchyPanel.cpp
        editor/PropertyWidgets.cpp
        editor/InspectorPanel.cpp
        editor/ContentBrowserPanel.cpp
        editor/ConsolePanel.cpp
        editor/ProfilerPanel.cpp
    DEPS lumora_appcommon lumora_collab)

lumora_qt_module(player
    SOURCES player/PlayerWindow.cpp
    DEPS lumora_appcommon)

# Top-level window orchestration and headless mode (linked by the executable and the UI tests).
lumora_qt_module(shell
    SOURCES
        main/Application.cpp
        main/Headless.cpp
        main/Launch.cpp
    DEPS lumora_hub lumora_editor lumora_player)

# Lumora: development build (Hub + editor in one process). lumora-editor: one installable editor
# version. lumora-hub: the Hub, which installs editors and opens projects in them.
foreach(app IN ITEMS Lumora lumora-editor lumora-hub)
    if(app STREQUAL "Lumora")
        set(entry main/main.cpp)
    elseif(app STREQUAL "lumora-editor")
        set(entry main/EditorMain.cpp)
    else()
        set(entry main/HubMain.cpp)
    endif()
    qt_add_executable(${app} ${entry})  # Qt records the plugins its deployment must copy
    target_include_directories(${app} PRIVATE ${CMAKE_SOURCE_DIR}/application)
    target_link_libraries(${app} PRIVATE lumora_shell lumora_compile_options)
    add_dependencies(${app} lumora_shaders)
    if(NOT LUMORA_SHARED_MODULES)
        target_sources(${app} PRIVATE common/resources.qrc)  # a static library would drop them
    endif()
    set_target_properties(${app} PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release> INSTALL_RPATH "$ORIGIN/../lib")
endforeach()

# ---- Packages: cmake --install <build> --component editor|hub --prefix <dir> -------------------
#   bin/lumora-editor (or lumora-hub), bin/shaders/*.spv, lib/liblumora_<module>.so + Qt libraries
#   and plugins on Linux; everything in bin/ on Windows (DLLs next to the executable).
get_property(lumora_modules GLOBAL PROPERTY LUMORA_MODULE_TARGETS)
foreach(component editor hub)
    if(LUMORA_SHARED_MODULES)
        install(TARGETS ${lumora_modules}
            RUNTIME DESTINATION bin COMPONENT ${component}
            LIBRARY DESTINATION lib COMPONENT ${component})
    endif()
    install(DIRECTORY ${LUMORA_SHADER_OUTPUT_DIR}/ DESTINATION bin/shaders COMPONENT ${component}
        FILES_MATCHING PATTERN "*.spv")
endforeach()
install(TARGETS lumora-editor RUNTIME DESTINATION bin COMPONENT editor)
foreach(component editor hub)
    install(FILES ${CMAKE_SOURCE_DIR}/LICENSE ${CMAKE_SOURCE_DIR}/THIRD_PARTY.md DESTINATION . COMPONENT ${component})
endforeach()
install(TARGETS lumora-hub RUNTIME DESTINATION bin COMPONENT hub)
# Qt's libraries and plugins (platform integrations, image formats, TLS for downloads, ...) go
# into the package. On Linux a deterministic script bundles only Qt and its ICU, never system
# libraries (libstdc++, xcb, Wayland, the Vulkan loader). Windows uses windeployqt through Qt's
# deployment API; the executables link the Qt modules so it knows which plugins they need.
foreach(app lumora-editor lumora-hub)
    string(REPLACE "lumora-" "" component ${app})
    target_link_libraries(${app} PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg)
    if(WIN32)
        qt_generate_deploy_app_script(TARGET ${app} OUTPUT_SCRIPT deploy_script NO_TRANSLATIONS)
        install(SCRIPT ${deploy_script} COMPONENT ${component})
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(LUMORA_APP ${app})
        set(LUMORA_QT_PLUGIN_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}")
        set(LUMORA_QT_LIB_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBS}")
        configure_file(${CMAKE_SOURCE_DIR}/cmake/LumoraDeployLinux.cmake.in
                       ${CMAKE_BINARY_DIR}/deploy/${app}.cmake @ONLY)
        install(SCRIPT ${CMAKE_BINARY_DIR}/deploy/${app}.cmake COMPONENT ${component})
    endif()
endforeach()
