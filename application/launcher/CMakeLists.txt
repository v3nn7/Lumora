# Editor installer used by the Hub: release feed, Ed25519 signatures, ZIP extraction, installs.

include(FetchContent)
set(CMAKE_AUTOMOC ON)

# miniz: ZIP reading/inflate (single C file, MIT).
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
    URL_HASH SHA256=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# Monocypher: Ed25519 signature checks (public domain / BSD).
FetchContent_Declare(monocypher
    URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz
    URL_HASH SHA256=bc1ca30b1b2654e4e7daf2492c0d204200e55137f23fda6b7142fd7d523bd6b4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(miniz monocypher)  # neither ships a usable CMake project: sources only

enable_language(C)
add_library(lumora_miniz STATIC ${miniz_SOURCE_DIR}/miniz.c)
target_include_directories(lumora_miniz SYSTEM PUBLIC ${miniz_SOURCE_DIR})
target_compile_definitions(lumora_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(lumora_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(lumora_monocypher STATIC
    ${monocypher_SOURCE_DIR}/src/monocypher.c
    ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c)
target_include_directories(lumora_monocypher SYSTEM PUBLIC
    ${monocypher_SOURCE_DIR}/src ${monocypher_SOURCE_DIR}/src/optional)
set_target_properties(lumora_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(LUMORA_RELEASE_PUBLIC_KEY "" CACHE STRING
    "Base64 Ed25519 public key release feeds must be signed with (empty: accept unsigned feeds)")
set(LUMORA_RELEASE_FEED "https://pub-dd139ac9b37f45229727e7708dd46000.r2.dev/editor/manifest.json" CACHE STRING
    "Default release feed URL shown in the Hub (the Hub's Editors page can change it)")

lumora_qt_module(launcher
    SOURCES
        ReleaseManifest.cpp
        Signature.cpp
        ZipExtractor.cpp
        EditorInstaller.cpp
        EditorRegistry.cpp
        FileList.cpp
    DEPS lumora_core Qt6::Network nlohmann_json::nlohmann_json)
target_link_libraries(lumora_launcher PRIVATE lumora_miniz lumora_monocypher)
target_include_directories(lumora_launcher PUBLIC ${CMAKE_SOURCE_DIR}/application)
if(LUMORA_RELEASE_PUBLIC_KEY)
    target_compile_definitions(lumora_launcher PRIVATE LUMORA_RELEASE_PUBLIC_KEY="${LUMORA_RELEASE_PUBLIC_KEY}")
endif()
if(LUMORA_RELEASE_FEED)
    target_compile_definitions(lumora_launcher PUBLIC LUMORA_RELEASE_FEED="${LUMORA_RELEASE_FEED}")
endif()
