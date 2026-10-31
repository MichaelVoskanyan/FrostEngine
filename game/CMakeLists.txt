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
