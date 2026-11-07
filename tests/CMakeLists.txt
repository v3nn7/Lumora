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
    VisualScriptTests.cpp
    MeshLodTests.cpp
    ImageFormatTests.cpp
    FbxObjTests.cpp
    MaterialGraphTests.cpp
    UiDocumentTests.cpp
    NetTests.cpp
    NetGameTests.cpp)
# A KTX2 (Basis ETC1S) texture and its original PNG, from Basis Universal's test files.
lumora_fetch_files(basisu_test_images https://raw.githubusercontent.com/BinomialLLC/basis_universal/v2_50
    test_files/kodim23.ktx2 0eb567d4e19be85562c74a77067631b7804d4042cc2ff78e161dc5915f8390a8
    test_files/kodim23.png e3111a2fd4da24af15d6459ef9eacfe54106b38e27b4a21821b75c3f5d2d5baf)
target_compile_definitions(lumora_tests PRIVATE LUMORA_TEST_BASISU_DIR="${basisu_test_images_SOURCE_DIR}")
target_link_libraries(lumora_tests PRIVATE
    lumora_core lumora_scene lumora_project lumora_runtime lumora_renderer lumora_cli
    GTest::gtest lumora_compile_options)
add_dependencies(lumora_tests lumora_shaders)
gtest_discover_tests(lumora_tests DISCOVERY_TIMEOUT 30)

# UI integration tests: real Hub/Editor widgets on Qt's offscreen platform, isolated config dir.
add_executable(lumora_ui_tests UiTestMain.cpp EditorUiTests.cpp GizmoTests.cpp HubUiTests.cpp CollabTests.cpp ScriptTests.cpp GraphEditorTests.cpp StbWriteImpl.cpp ${CMAKE_SOURCE_DIR}/application/common/resources.qrc)
set_target_properties(lumora_ui_tests PROPERTIES AUTORCC ON)
target_link_libraries(lumora_ui_tests PRIVATE lumora_shell lumora_collab stb GTest::gtest lumora_compile_options)
gtest_discover_tests(lumora_ui_tests DISCOVERY_TIMEOUT 30
    PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Editor installer (release feed, signatures, ZIP extraction, installs from file:// feeds).
add_executable(lumora_launcher_tests LauncherTestMain.cpp LauncherTests.cpp)
target_link_libraries(lumora_launcher_tests PRIVATE lumora_launcher lumora_miniz lumora_monocypher
    GTest::gtest lumora_compile_options)
gtest_discover_tests(lumora_launcher_tests DISCOVERY_TIMEOUT 30)
