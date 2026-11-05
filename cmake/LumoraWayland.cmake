# Wayland pointer lock for the viewport's mouse look. Wayland clients cannot warp the cursor,
# so looking around needs the pointer-constraints and relative-pointer protocols. Qt does not
# expose them; the client code is generated here with wayland-scanner from pinned XML.
set(LUMORA_WAYLAND_POINTER_LOCK OFF)
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(WaylandClient QUIET IMPORTED_TARGET wayland-client)
    endif()
    find_program(WAYLAND_SCANNER wayland-scanner)
    if(WaylandClient_FOUND AND WAYLAND_SCANNER)
        FetchContent_Declare(wayland_protocols
            URL https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/1.41/downloads/wayland-protocols-1.41.tar.xz
            URL_HASH SHA256=2786b6b1b79965e313f2c289c12075b9ed700d41844810c51afda10ee329576b
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(wayland_protocols)  # XML only (meson project): just populated
        enable_language(C)
        set(gen ${CMAKE_BINARY_DIR}/generated/wayland)
        file(MAKE_DIRECTORY ${gen})
        set(sources)
        foreach(proto pointer-constraints relative-pointer)
            set(xml ${wayland_protocols_SOURCE_DIR}/unstable/${proto}/${proto}-unstable-v1.xml)
            add_custom_command(OUTPUT ${gen}/${proto}-client.h
                COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${gen}/${proto}-client.h
                DEPENDS ${xml} VERBATIM)
            add_custom_command(OUTPUT ${gen}/${proto}-protocol.c
                COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${gen}/${proto}-protocol.c
                DEPENDS ${xml} VERBATIM)
            list(APPEND sources ${gen}/${proto}-client.h ${gen}/${proto}-protocol.c)
        endforeach()
        add_library(lumora_wayland_protocols STATIC ${sources})
        target_include_directories(lumora_wayland_protocols SYSTEM PUBLIC ${gen})
        target_link_libraries(lumora_wayland_protocols PUBLIC PkgConfig::WaylandClient)
        set(LUMORA_WAYLAND_POINTER_LOCK ON)
        message(STATUS "Lumora: Wayland pointer lock enabled (wayland-client ${WaylandClient_VERSION})")
    else()
        message(STATUS "Lumora: wayland-client/wayland-scanner not found - mouse look cannot lock the pointer on Wayland")
    endif()
endif()
