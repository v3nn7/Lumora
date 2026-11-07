# HLSL -> SPIR-V compilation at build time (glslc / shaderc HLSL front-end), and with the D3D12
# back end also HLSL -> DXIL (DXC, from the Vulkan or Windows SDK) next to each .spv.
# Slang can replace the compilers later without touching call sites.
find_program(LUMORA_GLSLC glslc
    HINTS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin
    REQUIRED)
option(LUMORA_BUILD_DXIL "Compile the shaders to DXIL too (needs DXC)" ${LUMORA_WITH_D3D12})
if(LUMORA_BUILD_DXIL)
    find_program(LUMORA_DXC dxc
        HINTS $ENV{VULKAN_SDK}/bin $ENV{VULKAN_SDK}/Bin "$ENV{WindowsSdkVerBinPath}/x64"
        REQUIRED)
    file(GLOB LUMORA_SHADER_HEADERS ${CMAKE_SOURCE_DIR}/shaders/*.hlsli)
endif()

# lumora_add_shader(<out_list_var> SOURCE <file.hlsl> STAGE <vert|frag|comp> ENTRY <fn> OUTPUT <name.spv>)
# Appends the produced .spv path to <out_list_var> in the caller's scope.
function(lumora_add_shader out_list)
    cmake_parse_arguments(SH "" "SOURCE;STAGE;ENTRY;OUTPUT" "" ${ARGN})
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${SH_SOURCE})
    set(out ${LUMORA_SHADER_OUTPUT_DIR}/${SH_OUTPUT})
    set(dep ${CMAKE_CURRENT_BINARY_DIR}/${SH_OUTPUT}.d)
    string(TOUPPER "${SH_STAGE}" stage_upper)
    set(opt -O)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(opt -g -O0)
    endif()
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${LUMORA_SHADER_OUTPUT_DIR}
        COMMAND ${LUMORA_GLSLC} -x hlsl -fshader-stage=${SH_STAGE} -fentry-point=${SH_ENTRY}
                --target-env=vulkan1.4 ${opt} -Werror -DLUMORA_STAGE_${stage_upper}=1
                -I ${CMAKE_CURRENT_SOURCE_DIR}
                -MD -MF ${dep} -MT ${out}
                -o ${out} ${src}
        DEPENDS ${src}
        DEPFILE ${dep}
        COMMENT "Compiling shader ${SH_OUTPUT}"
        VERBATIM)
    list(APPEND ${out_list} ${out})
    if(LUMORA_BUILD_DXIL)
        # Shader model 6.5; registers come from DX_REGISTER in common.hlsli (LUMORA_DXIL).
        string(REGEX REPLACE "\\.spv$" ".dxil" dxil ${out})
        set(profile_vert vs_6_5)
        set(profile_frag ps_6_5)
        set(profile_comp cs_6_5)
        set(dxopt -O3)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(dxopt -Od -Zi -Qembed_debug)
        endif()
        add_custom_command(
            OUTPUT ${dxil}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${LUMORA_SHADER_OUTPUT_DIR}
            COMMAND ${LUMORA_DXC} -T ${profile_${SH_STAGE}} -E ${SH_ENTRY} ${dxopt} -HV 2021 -WX
                    -Wno-unknown-attributes -Wno-ignored-attributes
                    -DLUMORA_DXIL=1 -DLUMORA_STAGE_${stage_upper}=1
                    -I ${CMAKE_CURRENT_SOURCE_DIR} -Fo ${dxil} ${src}
            DEPENDS ${src} ${LUMORA_SHADER_HEADERS}
            COMMENT "Compiling shader ${SH_OUTPUT} to DXIL"
            VERBATIM)
        list(APPEND ${out_list} ${dxil})
    endif()
    set(${out_list} ${${out_list}} PARENT_SCOPE)
endfunction()
