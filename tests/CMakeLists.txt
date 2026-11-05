include(GoogleTest)

add_executable(lumora_tests
    TestMain.cpp
    CoreTests.cpp
    PrimitivesTests.cpp
    SceneTests.cpp
    SerializationTests.cpp
    CameraTests.cpp
    ProjectTests.cpp
    SimulationTests.cpp)
target_link_libraries(lumora_tests PRIVATE
    lumora_core lumora_scene lumora_project lumora_runtime
    GTest::gtest lumora_compile_options)
gtest_discover_tests(lumora_tests DISCOVERY_TIMEOUT 30)
