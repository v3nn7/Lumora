# Third-party dependencies. Versions are pinned and archives are hash-verified.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

find_package(Vulkan 1.4 REQUIRED)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg Network)
find_package(Threads REQUIRED)

FetchContent_Declare(glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz
    URL_HASH SHA256=9f3174561fd26904b23f0db5e560971cbf9b3cbda0b280f04d5c379d03bf234c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

FetchContent_Declare(EnTT
    URL https://github.com/skypjack/entt/archive/refs/tags/v3.16.0.tar.gz
    URL_HASH SHA256=7d7b4037b737992342049ffab14f22fa10243e01664f8c3a0657aa247ac52f71
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

FetchContent_Declare(VulkanMemoryAllocator
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
    URL_HASH SHA256=c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

# cgltf: single-header glTF 2.0 loader (no CMake project; exposed as an INTERFACE target).
FetchContent_Declare(cgltf
    URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz
    URL_HASH SHA256=84e352092e5cd6aab7f66de62ddb66beb5e6f18d412ca9d12950d7a55bfef25a
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# stb (stb_image for PNG/JPEG decoding; stb_image_write for tests). Pinned commit (2026-08-02).
FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/2c980bb59875b0d32144a71867fbdebb2f77cd20.tar.gz
    URL_HASH SHA256=9a955b1b49a4410088a2e0ee2a9c057c3c907d0c1d75454144cb980aca0ba515
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# Jolt Physics. Portable x86-64 baseline (SSE4.2); no AVX/LZCNT/F16C/FMA assumptions about the
# user's CPU. Samples, tests, GPU compute, debug renderer and profiler are not built.
FetchContent_Declare(JoltPhysics
    URL https://github.com/jrouwe/JoltPhysics/archive/refs/tags/v5.6.0.tar.gz
    URL_HASH SHA256=6e069ee0172478cc78182047aac87e5310ba14a67a53348ae14cc37801fd3f8e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR Build
    SYSTEM)
foreach(opt TARGET_UNIT_TESTS TARGET_HELLO_WORLD TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER
            USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD
            INTERPROCEDURAL_OPTIMIZATION ENABLE_ALL_WARNINGS OVERRIDE_CXX_FLAGS FLOATING_POINT_EXCEPTIONS_ENABLED
            DEBUG_RENDERER_IN_DEBUG_AND_RELEASE PROFILER_IN_DEBUG_AND_RELEASE ENABLE_OBJECT_STREAM ENABLE_INSTALL
            JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE
            USE_STATIC_MSVC_RUNTIME_LIBRARY)  # MSVC: same /MD runtime as Qt and every Lumora module
    set(${opt} OFF CACHE INTERNAL "")
endforeach()
# RTTI on: sanitizers (UBSan vptr) and dynamic_cast across the boundary need Jolt's typeinfo.
set(CPP_RTTI_ENABLED ON CACHE INTERNAL "")
set(USE_SSE4_1 ON CACHE INTERNAL "")
set(USE_SSE4_2 ON CACHE INTERNAL "")
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(USE_ASSERTS ON CACHE INTERNAL "")
endif()

set(GLM_BUILD_TESTS OFF CACHE INTERNAL "")
set(GLM_BUILD_LIBRARY OFF CACHE INTERNAL "")
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
set(VMA_ENABLE_INSTALL OFF CACHE INTERNAL "")

FetchContent_MakeAvailable(glm EnTT VulkanMemoryAllocator nlohmann_json cgltf stb JoltPhysics)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})

if(LUMORA_BUILD_TESTS)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SYSTEM)
    set(INSTALL_GTEST OFF CACHE INTERNAL "")
    set(BUILD_GMOCK OFF CACHE INTERNAL "")
    set(gtest_force_shared_crt ON CACHE INTERNAL "")
    FetchContent_MakeAvailable(googletest)
endif()
