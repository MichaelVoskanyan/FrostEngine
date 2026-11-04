# cmake/Assets.cmake
#
# Stages the project's content directory (Frost assets and levels) next to a target's built
# executable, under a "content" subdirectory, so the executable's directory is laid out as a Frost
# project (which is how the game runs: as the project in its working directory), regardless of
# build configuration. Mirrors cmake/Shaders.cmake, which does the same for
# compiled shaders. Runs on every build; only changed files are copied.

# frost_stage_content(<target> <content-directory>)
function(frost_stage_content TARGET_NAME CONTENT_DIR)
    add_custom_target(${TARGET_NAME}Content ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${CONTENT_DIR}"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>/content"
        COMMENT "Staging content for ${TARGET_NAME}"
        VERBATIM
    )

    add_dependencies(${TARGET_NAME} ${TARGET_NAME}Content)
endfunction()
