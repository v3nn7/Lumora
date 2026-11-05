# Shared compiler configuration applied through the lumora_compile_options interface target.
add_library(lumora_compile_options INTERFACE)
add_library(Lumora::CompileOptions ALIAS lumora_compile_options)

if(MSVC)
    target_compile_options(lumora_compile_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(LUMORA_WARNINGS_AS_ERRORS)
        target_compile_options(lumora_compile_options INTERFACE /WX)
    endif()
    target_compile_definitions(lumora_compile_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(lumora_compile_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align
        -Woverloaded-virtual -Wimplicit-fallthrough
        -Wno-missing-field-initializers)
    if(LUMORA_WARNINGS_AS_ERRORS)
        target_compile_options(lumora_compile_options INTERFACE -Werror)
    endif()
    if(LUMORA_ENABLE_SANITIZERS)
        target_compile_options(lumora_compile_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(lumora_compile_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

target_compile_definitions(lumora_compile_options INTERFACE
    $<$<CONFIG:Debug>:LUMORA_DEBUG=1>
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL)

if(WIN32)
    target_compile_definitions(lumora_compile_options INTERFACE LUMORA_PLATFORM_WINDOWS=1)
elseif(UNIX)
    target_compile_definitions(lumora_compile_options INTERFACE LUMORA_PLATFORM_LINUX=1)
endif()
