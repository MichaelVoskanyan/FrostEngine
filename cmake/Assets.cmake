# cmake/Assets.cmake
#
# Copies runtime asset files (models, etc.) next to a target's built executable, under an "assets"
# subdirectory, so relative asset paths ("assets/<name>") resolve at runtime regardless of build
# configuration. Mirrors cmake/Shaders.cmake, which does the same for compiled shaders.

# frost_copy_assets(<target> <asset-file>...)
function(frost_copy_assets TARGET_NAME)
    set(ASSET_OUTPUT_DIR "$<TARGET_FILE_DIR:${TARGET_NAME}>/assets")

    add_custom_target(${TARGET_NAME}Assets ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${ASSET_OUTPUT_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${ARGN}
            "${ASSET_OUTPUT_DIR}"
        DEPENDS ${ARGN}
        COMMENT "Staging assets for ${TARGET_NAME}"
        VERBATIM
    )

    add_dependencies(${TARGET_NAME} ${TARGET_NAME}Assets)
endfunction()
