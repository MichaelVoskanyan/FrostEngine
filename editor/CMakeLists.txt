add_executable(FrostEditor)

target_sources(FrostEditor
    PRIVATE
        src/Main.cpp
        src/AssetLibrary.cpp
        src/AssetLibrary.hpp
        src/EditorApplication.cpp
        src/EditorApplication.hpp
        src/EditorCamera.cpp
        src/EditorCamera.hpp
        src/EditorContext.cpp
        src/EditorContext.hpp
        src/Panels/AssetLibraryPanel.cpp
        src/Panels/AssetLibraryPanel.hpp
        src/Panels/InspectorPanel.cpp
        src/Panels/InspectorPanel.hpp
        src/Panels/LevelPanel.cpp
        src/Panels/LevelPanel.hpp
        src/Panels/ViewportPanel.cpp
        src/Panels/ViewportPanel.hpp
        src/Platform/ChildProcess.cpp
        src/Platform/ChildProcess.hpp
        src/PlaySession.cpp
        src/PlaySession.hpp
        src/Widgets/FileBrowser.cpp
        src/Widgets/FileBrowser.hpp
        src/Widgets/FileDialog.cpp
        src/Widgets/FileDialog.hpp
        src/Widgets/ImGuiHelpers.cpp
        src/Widgets/ImGuiHelpers.hpp
)

target_compile_features(FrostEditor
    PRIVATE
        cxx_std_20
)

# imgui::imgui directly, as the editor builds its UI with ImGui:: calls (the engine only links it
# privately, for the ImGuiLayer that hosts the context).
target_link_libraries(FrostEditor
    PRIVATE
        Frost::Engine
        imgui::imgui
)

set_target_properties(FrostEditor PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(FrostEditor
        PRIVATE
            /W4
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
    )
else()
    target_compile_options(FrostEditor
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

include(${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake)

frost_compile_shaders(FrostEditor ${FROST_SHADER_SOURCES})

# With no project given on the command line, the editor opens the project the build is for (in
# place, not a staged copy), so imported assets and saved levels become part of it.
target_compile_definitions(FrostEditor
    PRIVATE
        FROST_EDITOR_DEFAULT_PROJECT_DIR="${FROST_PROJECT_DIR}"
)

# Play In Editor (F5) runs the game on the level being edited: the editor starts the FrostGame
# built alongside it (for the same configuration), so building the editor builds the game too.
target_compile_definitions(FrostEditor
    PRIVATE
        FROST_GAME_EXECUTABLE="$<TARGET_FILE:FrostGame>"
)

add_dependencies(FrostEditor FrostGame)
