find_package(Threads REQUIRED)
add_library(lumora_runtime ${LUMORA_MODULE_TYPE}
    Simulation.cpp
    PhysicsWorld.cpp
    Scripting.cpp
    GameUi.cpp
    UiDocument.cpp
    UiScreens.cpp
    InputActions.cpp
    SaveData.cpp
    AudioWorld.cpp
    AnimationSystem.cpp
    Navigation.cpp
    Localization.cpp
    GameMenu.cpp
    TimelineSystem.cpp
    AnimatorStateMachine.cpp
    LuaScript.cpp
    VisualScript.cpp)
# The Lua API prelude is compiled into the module.
set(lua_prelude_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/LuaPrelude.inc)
add_custom_command(OUTPUT ${lua_prelude_inc}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/LuaPrelude.lua -DOUT=${lua_prelude_inc}
            -P ${CMAKE_SOURCE_DIR}/cmake/LumoraEmbedText.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/LuaPrelude.lua ${CMAKE_SOURCE_DIR}/cmake/LumoraEmbedText.cmake
    VERBATIM)
target_sources(lumora_runtime PRIVATE ${lua_prelude_inc})
target_include_directories(lumora_runtime PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
lumora_module_setup(lumora_runtime)
add_library(Lumora::runtime ALIAS lumora_runtime)
# The scripting ABI lives in the SDK header that game libraries compile against.
target_include_directories(lumora_runtime PUBLIC ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/sdk/include)
target_link_libraries(lumora_runtime PUBLIC lumora_scene lumora_input PRIVATE Jolt miniaudio lumora_lua Recast Detour DetourCrowd Threads::Threads lumora_compile_options ${CMAKE_DL_LIBS})
