# HLSL -> SPIR-V compilation at build time (glslc / shaderc HLSL front-end).
# Slang can replace the compiler later without touching call sites.
find_program(LUMORA_GLSLC glslc
    HINTS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin
    REQUIRED)

# lumora_add_shader(<out_list_var> SOURCE <file.hlsl> STAGE <vert|frag|comp> ENTRY <fn> OUTPUT <name.spv>)
# Appends the produced .spv path to <out_list_var> in the caller's scope.
function(lumora_add_shader out_list)
    cmake_parse_arguments(SH "" "SOURCE;STAGE;ENTRY;OUTPUT" "" ${ARGN})
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${SH_SOURCE})
    set(out ${LUMORA_SHADER_OUTPUT_DIR}/${SH_OUTPUT})
    set(dep ${CMAKE_CURRENT_BINARY_DIR}/${SH_OUTPUT}.d)
    set(opt -O)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(opt -g -O0)
    endif()
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${LUMORA_SHADER_OUTPUT_DIR}
        COMMAND ${LUMORA_GLSLC} -x hlsl -fshader-stage=${SH_STAGE} -fentry-point=${SH_ENTRY}
                --target-env=vulkan1.4 ${opt} -Werror
                -I ${CMAKE_CURRENT_SOURCE_DIR}
                -MD -MF ${dep} -MT ${out}
                -o ${out} ${src}
        DEPENDS ${src}
        DEPFILE ${dep}
        COMMENT "Compiling shader ${SH_OUTPUT}"
        VERBATIM)
    list(APPEND ${out_list} ${out})
    set(${out_list} ${${out_list}} PARENT_SCOPE)
endfunction()
