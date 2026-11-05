# Application layer: one executable (Lumora) hosting Hub, Editor, Player and headless mode.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Command-line parsing has no Qt dependency so it can be unit tested.
add_library(lumora_cli STATIC main/CommandLine.cpp)
target_include_directories(lumora_cli PUBLIC ${CMAKE_SOURCE_DIR}/application)
target_link_libraries(lumora_cli PUBLIC lumora_core PRIVATE lumora_compile_options)

function(lumora_qt_module name)
    cmake_parse_arguments(M "" "" "SOURCES;DEPS" ${ARGN})
    add_library(lumora_${name} STATIC ${M_SOURCES})
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
    DEPS lumora_project lumora_renderer lumora_runtime lumora_cli Qt6::Widgets Qt6::Svg)

lumora_qt_module(hub
    SOURCES
        hub/HubWindow.cpp
        hub/NewProjectDialog.cpp
        hub/ProjectListModel.cpp
        hub/ProjectCardDelegate.cpp
    DEPS lumora_appcommon)

lumora_qt_module(editor
    SOURCES
        editor/EditorContext.cpp
        editor/EditorWindow.cpp
        editor/EditorViewportClient.cpp
        editor/EntityFactory.cpp
        editor/SceneHierarchyPanel.cpp
        editor/PropertyWidgets.cpp
        editor/InspectorPanel.cpp
        editor/ContentBrowserPanel.cpp
        editor/ConsolePanel.cpp
        editor/ProfilerPanel.cpp
    DEPS lumora_appcommon)

lumora_qt_module(player
    SOURCES player/PlayerWindow.cpp
    DEPS lumora_appcommon)

# Top-level window orchestration and headless mode (linked by the executable and the UI tests).
lumora_qt_module(shell
    SOURCES
        main/Application.cpp
        main/Headless.cpp
    DEPS lumora_hub lumora_editor lumora_player)

add_executable(Lumora main/main.cpp)
target_link_libraries(Lumora PRIVATE lumora_shell lumora_compile_options)
add_dependencies(Lumora lumora_shaders)
set_target_properties(Lumora PROPERTIES WIN32_EXECUTABLE $<CONFIG:Release>)
# Icons are compiled into the executable (not a static library) so their registration is never dropped.
target_sources(Lumora PRIVATE common/resources.qrc)
