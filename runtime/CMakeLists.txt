add_library(lumora_runtime STATIC
    Simulation.cpp
    PhysicsWorld.cpp)
add_library(Lumora::runtime ALIAS lumora_runtime)
target_include_directories(lumora_runtime PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(lumora_runtime PUBLIC lumora_scene PRIVATE Jolt lumora_compile_options)
