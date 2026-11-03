add_executable(FrostGame)

target_sources(FrostGame
    PRIVATE
        src/Main.cpp
        src/GameApplication.cpp
        src/GameApplication.hpp
)

target_compile_features(FrostGame
    PRIVATE
        cxx_std_20
)

target_link_libraries(FrostGame
    PRIVATE
        Frost::Engine
)

set_target_properties(FrostGame PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(FrostGame
        PRIVATE
            /W4
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
    )
else()
    target_compile_options(FrostGame
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

include(${CMAKE_SOURCE_DIR}/cmake/Shaders.cmake)

frost_compile_shaders(FrostGame
    ${CMAKE_SOURCE_DIR}/shaders/basic.vert
    ${CMAKE_SOURCE_DIR}/shaders/basic.frag
)

include(${CMAKE_SOURCE_DIR}/cmake/Assets.cmake)

frost_copy_assets(FrostGame
    ${CMAKE_SOURCE_DIR}/assets/Glock19X.glb
    ${CMAKE_SOURCE_DIR}/assets/Glock19.gltf
    ${CMAKE_SOURCE_DIR}/assets/Glock19.bin
    ${CMAKE_SOURCE_DIR}/assets/broken.gltf
    ${CMAKE_SOURCE_DIR}/assets/broken.bin
    ${CMAKE_SOURCE_DIR}/assets/black64.png
    ${CMAKE_SOURCE_DIR}/assets/mr_metal_smooth.png
    ${CMAKE_SOURCE_DIR}/assets/mr_metal_rough.png
    ${CMAKE_SOURCE_DIR}/assets/mr_rough.png
    ${CMAKE_SOURCE_DIR}/assets/normal_grip.png
)
