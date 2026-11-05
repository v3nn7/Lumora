# Every engine and application module is its own shared library (liblumora_<module>.so /
# lumora_<module>.dll), so an update only replaces the modules that changed. Set
# LUMORA_SHARED_MODULES=OFF for static libraries (one self-contained executable per app).
option(LUMORA_SHARED_MODULES "Build each engine/application module as a shared library" ON)
if(LUMORA_SHARED_MODULES)
    set(LUMORA_MODULE_TYPE SHARED)
    # Functions are exported automatically on Windows; classes whose data other modules use
    # (Qt's staticMetaObject) carry LUMORA_<MODULE>_API (application/common/ModuleExport.hpp).
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    set(LUMORA_MODULE_TYPE STATIC)
endif()

# Registers a module target: install list, RPATH and link checks.
function(lumora_module_setup target)
    set_property(GLOBAL APPEND PROPERTY LUMORA_MODULE_TARGETS ${target})
    if(NOT LUMORA_SHARED_MODULES)
        return()
    endif()
    target_compile_definitions(${target} PUBLIC LUMORA_SHARED_MODULES)
    set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(${target} PRIVATE "LINKER:--no-undefined")  # every dependency declared
    endif()
endfunction()
