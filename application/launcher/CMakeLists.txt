# Editor installer used by the Hub: release feed, Ed25519 signatures, ZIP extraction, installs.

include(FetchContent)
set(CMAKE_AUTOMOC ON)

# Monocypher and miniz: cmake/LumoraDependencies.cmake.

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
        HubUpdate.cpp
    DEPS lumora_core Qt6::Network nlohmann_json::nlohmann_json)
target_link_libraries(lumora_launcher PRIVATE lumora_miniz lumora_monocypher)
target_include_directories(lumora_launcher PUBLIC ${CMAKE_SOURCE_DIR}/application)
if(LUMORA_RELEASE_PUBLIC_KEY)
    target_compile_definitions(lumora_launcher PRIVATE LUMORA_RELEASE_PUBLIC_KEY="${LUMORA_RELEASE_PUBLIC_KEY}")
endif()
if(LUMORA_RELEASE_FEED)
    target_compile_definitions(lumora_launcher PUBLIC LUMORA_RELEASE_FEED="${LUMORA_RELEASE_FEED}")
endif()
