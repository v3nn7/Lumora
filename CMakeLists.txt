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

# Pre-release builds carry a suffix (0.2.0-beta.1); it also selects the release channel the
# build belongs to: alpha*, beta*/rc* or stable (no suffix).
set(LUMORA_VERSION_SUFFIX "" CACHE STRING "Pre-release suffix, e.g. alpha.1, beta.2 or rc.1 (empty: stable)")
if(LUMORA_VERSION_SUFFIX AND NOT LUMORA_VERSION_SUFFIX MATCHES "^(alpha|beta|rc)(\\.[0-9A-Za-z]+)*$")
    message(FATAL_ERROR "LUMORA_VERSION_SUFFIX '${LUMORA_VERSION_SUFFIX}' must look like alpha.1, beta.2 or rc.1")
endif()
set(LUMORA_VERSION_STRING ${PROJECT_VERSION})
set(LUMORA_RELEASE_CHANNEL stable)
if(LUMORA_VERSION_SUFFIX)
    string(APPEND LUMORA_VERSION_STRING "-${LUMORA_VERSION_SUFFIX}")
    if(LUMORA_VERSION_SUFFIX MATCHES "^alpha")
        set(LUMORA_RELEASE_CHANNEL alpha)
    else()
        set(LUMORA_RELEASE_CHANNEL beta)
    endif()
endif()

option(LUMORA_BUILD_TESTS "Build Lumora unit tests" ON)
option(LUMORA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LUMORA_ENABLE_SANITIZERS "Enable ASan/UBSan (Debug, GCC/Clang)" OFF)

# Everything is position independent: modules are shared libraries, and third-party static
# libraries (Jolt, miniz, ...) are linked into them.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# All binaries land in one directory so the executable finds bin/shaders at runtime.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(LUMORA_SHADER_OUTPUT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(LumoraCompilerOptions)
include(LumoraModules)
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
