# for building in linux, ensure the following are installed
# sudo apt update
# sudo apt install -y build-essential ninja-build git curl zip unzip tar pkg-config \
# libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev \ 
# libxkbcommon-dev libgll-mesa-dev libvulkan-dev glslc
#
# Ensure cmake version is 3.29 or newer. Check with cmake --version
#
# vcpkg is required. Install with:
# git clone https://github.com/microsoft/gcpkg.git ~/vcpkg
# ~/vcpkg/bootstrap-vcpkg.sh
# export VCPKG_ROOT="$HOME/vcpkg"
#
# then run 
# cmake --preset linux-gcc-debug
# cmake --build --preset linux-debug

cmake_minimum_required(VERSION 3.29)

project(
    Frost
    VERSION 0.1.0
    LANGUAGES CXX
)

include(cmake/Dependencies.cmake)

# The renderer loads its shaders by relative path; every executable hosting the engine gets them
# compiled and staged beside it (see cmake/Shaders.cmake).
set(FROST_SHADER_SOURCES
    ${CMAKE_SOURCE_DIR}/shaders/basic.vert
    ${CMAKE_SOURCE_DIR}/shaders/basic.frag
    ${CMAKE_SOURCE_DIR}/shaders/skinned.vert
)

# The Frost project the game is built for and the editor opens by default: a self-contained
# directory with the project's content (Frost assets and levels, in content/; see
# engine/include/Frost/Project/Project.hpp). This repository is itself the sample project. The
# game loads a staged copy of the project's content (see cmake/Assets.cmake); the editor works on
# the project directly, so what it imports and saves ends up in it. Source files (glTF, PNG) are not
# content: they live anywhere (e.g. assets/) and are imported into content/.
set(FROST_PROJECT_DIR ${CMAKE_SOURCE_DIR} CACHE PATH "The Frost project the game is built for and the editor opens by default")
set(FROST_CONTENT_DIR ${FROST_PROJECT_DIR}/content)

add_subdirectory(engine)
add_subdirectory(game)
add_subdirectory(editor)