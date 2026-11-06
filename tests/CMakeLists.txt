include(GoogleTest)

add_executable(lumora_tests
    TestMain.cpp
    CoreTests.cpp
    PrimitivesTests.cpp
    SceneTests.cpp
    SerializationTests.cpp
    ScenePatchTests.cpp
    CameraTests.cpp
    ProjectTests.cpp
    SimulationTests.cpp
    RenderGraphTests.cpp
    RendererTests.cpp
    CommandLineTests.cpp
    GltfTests.cpp
    StbWriteImpl.cpp
    JobSystemTests.cpp
    PhysicsTests.cpp
    EnvironmentTests.cpp
    AudioTests.cpp
    MaterialTests.cpp
    GameUiTests.cpp
    ParticleTests.cpp
    InputTests.cpp
    NavigationTests.cpp
    LandscapeTests.cpp
    LocalizationTests.cpp
    TimelineTests.cpp
    LuaScriptTests.cpp
    VisualScriptTests.cpp)
target_link_libraries(lumora_tests PRIVATE
    lumora_core lumora_scene lumora_project lumora_runtime lumora_renderer lumora_cli
    GTest::gtest lumora_compile_options)
add_dependencies(lumora_tests lumora_shaders)
gtest_discover_tests(lumora_tests DISCOVERY_TIMEOUT 30)

# UI integration tests: real Hub/Editor widgets on Qt's offscreen platform, isolated config dir.
add_executable(lumora_ui_tests UiTestMain.cpp EditorUiTests.cpp GizmoTests.cpp HubUiTests.cpp CollabTests.cpp ScriptTests.cpp StbWriteImpl.cpp ${CMAKE_SOURCE_DIR}/application/common/resources.qrc)
set_target_properties(lumora_ui_tests PROPERTIES AUTORCC ON)
target_link_libraries(lumora_ui_tests PRIVATE lumora_shell lumora_collab stb GTest::gtest lumora_compile_options)
gtest_discover_tests(lumora_ui_tests DISCOVERY_TIMEOUT 30
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Editor installer (release feed, signatures, ZIP extraction, installs from file:// feeds).
add_executable(lumora_launcher_tests LauncherTestMain.cpp LauncherTests.cpp)
target_link_libraries(lumora_launcher_tests PRIVATE lumora_launcher lumora_miniz lumora_monocypher
    GTest::gtest lumora_compile_options)
gtest_discover_tests(lumora_launcher_tests DISCOVERY_TIMEOUT 30)
