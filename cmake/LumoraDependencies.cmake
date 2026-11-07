# Third-party dependencies. Versions are pinned and archives are hash-verified.
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Vulkan headers pinned for every platform (NVRHI needs 1.4.318+; distributions ship older ones).
# The loader still comes from the system or the Vulkan SDK.
FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.352.tar.gz
    URL_HASH SHA256=4850909d22a8a9767c27daea2b972e49d7c298560573d5b6221ee50db9bf49db
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
FetchContent_MakeAvailable(VulkanHeaders)
# Before the system's include directory everywhere, so every file sees the same headers.
include_directories(BEFORE SYSTEM ${vulkanheaders_SOURCE_DIR}/include)
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

# miniaudio 0.11.25: audio playback, decoding (WAV, FLAC, MP3) and 3D spatialisation. A single
# header, fetched as the file itself (public domain or MIT-0).
FetchContent_Declare(miniaudio
    URL https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h
    URL_HASH SHA256=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
    DOWNLOAD_NO_EXTRACT TRUE)

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

# Recast & Detour 1.6.0 (zlib): navigation meshes, path finding and crowds of agents.
FetchContent_Declare(recastnavigation
    URL https://github.com/recastnavigation/recastnavigation/archive/refs/tags/v1.6.0.tar.gz
    URL_HASH SHA256=d48ca0121962fa0639502c0f56c4e3ae72f98e55d88727225444f500775c0074
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)

# NVRHI (NVIDIA Rendering Hardware Interface, MIT): the renderer's GPU layer, with automatic
# resource state tracking. Vulkan on every platform; D3D12 on Windows. Pinned commit (2026-10-28).
FetchContent_Declare(nvrhi
    URL https://github.com/NVIDIA-RTX/NVRHI/archive/bf0bbd9b666a7297492beb1999a4ddc0dfded143.tar.gz
    URL_HASH SHA256=059ce649063d7bfe8fd7cb2f4cdd05a606f5526f7060237f9e37ecf75bc5f9e2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 ${LUMORA_WITH_D3D12} CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)  # the Vulkan SDK's headers (Vulkan::Headers)

# Lua 5.4.7 (MIT): the language of Lua behaviours. Pinned to the v5.4.7 release commit; built
# below from its C sources (the interpreter, compiler and test files are left out).
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG 1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3)

# libwebp 1.6.0 (BSD): WebP decoding (EXT_texture_webp, .webp textures). Only the decoder
# library is built (EXCLUDE_FROM_ALL; the tools and encoder are not).
FetchContent_Declare(libwebp
    URL https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz
    URL_HASH SHA256=93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
foreach(opt WEBP_BUILD_ANIM_UTILS WEBP_BUILD_CWEBP WEBP_BUILD_DWEBP WEBP_BUILD_GIF2WEBP WEBP_BUILD_IMG2WEBP
            WEBP_BUILD_VWEBP WEBP_BUILD_WEBPINFO WEBP_BUILD_LIBWEBPMUX WEBP_BUILD_WEBPMUX WEBP_BUILD_EXTRAS
            WEBP_BUILD_WEBP_JS WEBP_BUILD_FUZZTEST)
    set(${opt} OFF CACHE INTERNAL "")
endforeach()

FetchContent_MakeAvailable(glm EnTT nlohmann_json cgltf stb JoltPhysics miniaudio nvrhi recastnavigation libwebp)
FetchContent_GetProperties(lua)
if(NOT lua_POPULATED)
    FetchContent_Populate(lua)
endif()
enable_language(C)
set(lua_sources lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate
    lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib lmathlib loadlib loslib
    lstrlib ltablib lutf8lib linit)
list(TRANSFORM lua_sources PREPEND ${lua_SOURCE_DIR}/)
list(TRANSFORM lua_sources APPEND .c)
add_library(lumora_lua STATIC ${lua_sources})
target_include_directories(lumora_lua SYSTEM PUBLIC ${lua_SOURCE_DIR})
set_target_properties(lumora_lua PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
if(UNIX)
    target_compile_definitions(lumora_lua PRIVATE LUA_USE_POSIX)
    target_link_libraries(lumora_lua PRIVATE m)
endif()
# NVRHI is static; the renderer module may be a shared library.
set_target_properties(nvrhi nvrhi_vk Recast Detour DetourCrowd PROPERTIES POSITION_INDEPENDENT_CODE ON)
# On Windows NVRHI publishes VK_USE_PLATFORM_WIN32_KHR, which makes <vulkan/vulkan.h> include
# <windows.h> everywhere (its near/far/DeviceCapabilities... macros break ordinary code). Qt
# creates the window surfaces, so only NVRHI's own sources keep it.
if(WIN32)
    set_property(TARGET nvrhi_vk PROPERTY INTERFACE_COMPILE_DEFINITIONS "")
endif()
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE ${stb_SOURCE_DIR})
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE ${cgltf_SOURCE_DIR})
add_library(lumora_webp INTERFACE)  # <webp/decode.h>
target_include_directories(lumora_webp SYSTEM INTERFACE ${libwebp_SOURCE_DIR}/src)
target_link_libraries(lumora_webp INTERFACE webpdecoder)
set_target_properties(webpdecoder PROPERTIES POSITION_INDEPENDENT_CODE ON)

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

# Single files fetched one by one from a pinned tag, each hash-verified, into
# <build>/_deps/<name>-src (for projects whose archive is far larger than the few files used).
function(lumora_fetch_files name base_url)
    set(dir ${FETCHCONTENT_BASE_DIR}/${name}-src)
    set(args ${ARGN})
    list(LENGTH args count)
    math(EXPR last "${count} - 1")
    foreach(i RANGE 0 ${last} 2)
        math(EXPR j "${i} + 1")
        list(GET args ${i} file)
        list(GET args ${j} hash)
        file(DOWNLOAD ${base_url}/${file} ${dir}/${file} EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "Downloading ${base_url}/${file} failed: ${status}")
        endif()
    endforeach()
    set(${name}_SOURCE_DIR ${dir} PARENT_SCOPE)
endfunction()

# miniz 3.0.2 (MIT): deflate/inflate and ZIP reading (EXR files, the editor installer).
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(miniz)  # no usable CMake project: the amalgamated source only
add_library(lumora_miniz STATIC ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(lumora_miniz SYSTEM PUBLIC ${miniz_SOURCE_DIR})
target_compile_definitions(lumora_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(lumora_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Monocypher 4.0.2 (BSD-2-Clause or CC0): Ed25519 signatures (editor installer), X25519 and
# XChaCha20-Poly1305 (collaboration and game networking).
FetchContent_Declare(monocypher
    URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz
    URL_HASH SHA256=bc1ca30b1b2654e4e7daf2492c0d204200e55137f23fda6b7142fd7d523bd6b4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(monocypher)  # sources only
add_library(lumora_monocypher STATIC
    ${monocypher_SOURCE_DIR}/src/monocypher.c
    ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c)
target_include_directories(lumora_monocypher SYSTEM PUBLIC
    ${monocypher_SOURCE_DIR}/src ${monocypher_SOURCE_DIR}/src/optional)
# Stateless, so modules that use it (collaboration, game networking) each keep a private copy.
set_target_properties(lumora_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

# Basis Universal 2.50 transcoder (Apache-2.0) with its Zstandard decoder (BSD): KTX2 textures
# (KHR_texture_basisu). The encoder is not needed.
lumora_fetch_files(basisu https://raw.githubusercontent.com/BinomialLLC/basis_universal/v2_50
    LICENSE 065fcf48d6af21c0b75e23be5ed5753aee75c892e1c2cf178fa6736305614a5c
    transcoder/basisu_astc_cfgs.inl 2f56d16cc37ddb6a5f361be42e3ab32ecbeefd94fd287d5bd46c15e0e1fdb34e
    transcoder/basisu_astc_hdr_core.h 4d1e2b68275c7f402623ab0824722d7e8d6e85adc2fc747a44c07e5321b366d4
    transcoder/basisu_astc_helpers.h f698c23506c0e59f3bd4cb645bd3b4d071b06f1112a0cf6a0de19a19cff837f8
    transcoder/basisu_containers.h 84a17dcb701dd22a44381ae226e3e9cdc9d361e74efc640d0ccb8bbbb081ee41
    transcoder/basisu_containers_impl.h 9810baeca8b39497101997cf309c0cee153f096af21f1eb8e12c142410f5ef2a
    transcoder/basisu_dds_transcoder.inl 18f731ebe27b311efda2a576b3bc76f1e1b541b25e9a790a9759f76effc61833
    transcoder/basisu_etc1_mods.inl ddf9efd40f07209c914f4051b94bbe1821915ae61b6ca39d61aafd0126c6dd0d
    transcoder/basisu_file_headers.h 5988f2d27e41a3f36ef5a2e350675dd30bd1ee2c70853e42a1d796a74d8178a9
    transcoder/basisu.h 2724dd59acbd332d43dde5f15a9b546ed851c45c7c2acbcdcab4a288c58628e0
    transcoder/basisu_idct.h 0b218ce8a9f72036c1e531387d2a168b950a2af2d5a40204f08a5ec381a2e4ee
    transcoder/basisu_transcoder.cpp 244cf0da60941c0311feed5e9c01b9225d6ca796292391d808a4fa3cf9e406f2
    transcoder/basisu_transcoder.h d60e4d56c7dbc392ec476e484cc5030046ecd378c554a5ffa898ccd0e1ec1b08
    transcoder/basisu_transcoder_internal.h 9e12e4543d2872c4c0beb91bf9438d5bed1205d52358fd15671b747d8485369f
    transcoder/basisu_transcoder_tables_astc_0_255.inc f41acb498be64349c48600e965664f4bec1161781169e3ec3411827c490ef4f8
    transcoder/basisu_transcoder_tables_astc.inc 7dd41c648d3ed8e7d511eb4af865669c4dd50b8ecead62a68fca471138c46e1a
    transcoder/basisu_transcoder_tables_atc_55.inc 4a510a20bffc3738d85d41e44633d44145931aa5734affb390c7836ec8b5dabb
    transcoder/basisu_transcoder_tables_atc_56.inc 94714b54f0c13c2f3ba5481fbfb40b974892048d59ab68419b369222f0a68075
    transcoder/basisu_transcoder_tables_bc7_m5_alpha.inc 4d713b839594f429d95f329d60f8ad495e932619111b5dfddffb688e3634b495
    transcoder/basisu_transcoder_tables_bc7_m5_color.inc e6f63d3ebce0ac84d801ec54614b6a4eeda3be3e994f3abbfc732377cfe5a531
    transcoder/basisu_transcoder_tables_dxt1_5.inc 500e640b5e6858fd10194eb4845670eb36d3ef9dc117a017632c1104ba3eb9d5
    transcoder/basisu_transcoder_tables_dxt1_6.inc 3d203465f9097845eddb821943d312bf8cffb04d8f5ce09c18d4c6df40519ed3
    transcoder/basisu_transcoder_tables_pvrtc2_45.inc 780711642098a91239ee37b3f9110469ee1c7da8dd9e440a43c7ba70f992ff9d
    transcoder/basisu_transcoder_tables_pvrtc2_alpha_33.inc 250d389235c0213757266441f72aee37cbe138a3c2a3f58b590ed53d6fb746fa
    transcoder/basisu_transcoder_uastc.h 8796f8e755b868c56967e3d4d53089c2aaac7e1f345b03be1e975016fcbaf11e
    transcoder/basisu_xbc7_decoder.h 7487d5c43fe2ff97aba5a3cee87cfcdcb433f003b72c47927d4d4aeebde7b2ea
    transcoder/basisu_xbc7_decoder.inl 485b6c4b9297047430ea8124319501d58389d96da38642f59dafd974d88d65d1
    zstd/LICENSE 2c1a7fa704df8f3a606f6fc010b8b5aaebf403f3aeec339a12048f1ba7331a0b
    zstd/zstddeclib.c 3a007442219d631e3748113518dd723d577647c8c00ca3f70371d0c2f7bda5a8
    zstd/zstd.h 9b4bc8245565c98ccfc61c07749928b57e7c0f6fddb0530c4f6aa1971893d88b
    zstd/zstd_errors.h 66a8c3f71d12ea6e797e4f622f31f3f8f81c41b36f48cad4f5de7d8bfb6aac0a)
add_library(lumora_basisu STATIC ${basisu_SOURCE_DIR}/transcoder/basisu_transcoder.cpp ${basisu_SOURCE_DIR}/zstd/zstddeclib.c)
target_include_directories(lumora_basisu SYSTEM PUBLIC ${basisu_SOURCE_DIR}/transcoder)
# KTX2 with Zstandard supercompression; no .basis-only paths need anything else.
target_compile_definitions(lumora_basisu PUBLIC BASISD_SUPPORT_KTX2=1 BASISD_SUPPORT_KTX2_ZSTD=1)
set_target_properties(lumora_basisu PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(lumora_basisu PRIVATE /W0)
else()
    target_compile_options(lumora_basisu PRIVATE -w)
endif()

# tinyexr 1.0.13 (BSD): OpenEXR environment maps; compiled in engine/assets/ExrImpl.cpp, inflating
# with miniz.
lumora_fetch_files(tinyexr https://raw.githubusercontent.com/syoyo/tinyexr/v1.0.13
    tinyexr.h 56e041e8ca7e76748d1730790b11c5f5daa5fe03c18cf371ded1e672bec49dbb
    exr_reader.hh 9e0bfef0487b9c3082165b2a637c75ef91d9fcc4aa55a5f32eb97f77db19f99b
    streamreader.hh 81f2d1502930ca9ca9957e7607c2a7924d3ef2fe4b08b1a4001245dc6d3fefb6)
add_library(lumora_tinyexr INTERFACE)
target_include_directories(lumora_tinyexr SYSTEM INTERFACE ${tinyexr_SOURCE_DIR})
target_link_libraries(lumora_tinyexr INTERFACE lumora_miniz)

# ufbx 0.23.1 (MIT or public domain): FBX and Wavefront OBJ (+ MTL) import. One C file.
lumora_fetch_files(ufbx https://raw.githubusercontent.com/ufbx/ufbx/v0.23.1
    LICENSE 0dd48ebadf52273c736256325c8f078c03c8bb4facee22a4122de0ad3f615391
    ufbx.h f787be529af577efc04f3e3e735844a08556718dbb613d1211c2b313d72895dc
    ufbx.c 4a956c26a708e40d82ecb0aeaee54da709d5ba7c144c9642174970059bda7e1d)
add_library(lumora_ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(lumora_ufbx SYSTEM PUBLIC ${ufbx_SOURCE_DIR})
set_target_properties(lumora_ufbx PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
if(MSVC)
    target_compile_options(lumora_ufbx PRIVATE /W0)
else()
    target_compile_options(lumora_ufbx PRIVATE -w)
    target_link_libraries(lumora_ufbx PRIVATE m)
endif()
