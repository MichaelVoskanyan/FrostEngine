# cmake/Shaders.cmake
#
# Compiles GLSL shader sources to SPIR-V with glslc and places the .spv output next to a target's
# built executable, under a "shaders" subdirectory, so VulkanPipeline's relative shader paths
# ("shaders/<name>.spv") resolve at runtime regardless of build configuration.

find_program(GLSLC_EXECUTABLE glslc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")

if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found. Install the Vulkan SDK (https://vulkan.lunarg.com/) and "
                        "ensure it is on PATH or that VULKAN_SDK is set.")
endif()

# frost_compile_shaders(<target> <shader-source>...)
#
# Compiles each shader source to SPIR-V and makes <target> depend on the compiled output, so
# editing a shader and rebuilding <target> recompiles it automatically.
function(frost_compile_shaders TARGET_NAME)
    set(SHADER_OUTPUT_DIR "$<TARGET_FILE_DIR:${TARGET_NAME}>/shaders")
    set(SPIRV_FILES "")

    foreach(SHADER_SOURCE ${ARGN})
        get_filename_component(SHADER_NAME ${SHADER_SOURCE} NAME)
        set(SPIRV_FILE "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")

        add_custom_command(
            OUTPUT ${SPIRV_FILE}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}
            COMMAND ${GLSLC_EXECUTABLE} ${SHADER_SOURCE} -o ${SPIRV_FILE}
            DEPENDS ${SHADER_SOURCE}
            COMMENT "Compiling shader ${SHADER_NAME}"
            VERBATIM
        )

        list(APPEND SPIRV_FILES ${SPIRV_FILE})
    endforeach()

    add_custom_target(${TARGET_NAME}Shaders DEPENDS ${SPIRV_FILES})
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}Shaders)
endfunction()
