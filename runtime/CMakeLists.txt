find_package(Threads REQUIRED)
add_library(lumora_runtime ${LUMORA_MODULE_TYPE}
    Simulation.cpp
    PhysicsWorld.cpp
    Scripting.cpp
    GameUi.cpp
    InputActions.cpp
    SaveData.cpp
    AudioWorld.cpp
    AnimationSystem.cpp
    Navigation.cpp)
lumora_module_setup(lumora_runtime)
add_library(Lumora::runtime ALIAS lumora_runtime)
# The scripting ABI lives in the SDK header that game libraries compile against.
target_include_directories(lumora_runtime PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lumora_runtime PUBLIC lumora_scene lumora_input PRIVATE Jolt miniaudio Recast Detour DetourCrowd Threads::Threads lumora_compile_options ${CMAKE_DL_LIBS})
