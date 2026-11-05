cmake_minimum_required(VERSION 3.28)

project(Lumora
    VERSION 0.1.0
    DESCRIPTION "Lumora Engine - high-fidelity real-time 3D engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(LUMORA_BUILD_TESTS "Build Lumora unit tests" ON)
option(LUMORA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LUMORA_ENABLE_SANITIZERS "Enable ASan/UBSan (Debug, GCC/Clang)" OFF)

# All binaries land in one directory so the executable finds bin/shaders at runtime.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(LUMORA_SHADER_OUTPUT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(LumoraCompilerOptions)
include(LumoraDependencies)
include(LumoraWayland)
include(LumoraShaders)

add_subdirectory(engine)
add_subdirectory(runtime)
add_subdirectory(shaders)
add_subdirectory(application)

if(LUMORA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
