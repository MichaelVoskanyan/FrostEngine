add_executable(CesiumGame)

target_sources(CesiumGame
    PRIVATE
        src/Main.cpp
        src/GameApplication.cpp
        src/GameApplication.hpp
)

target_compile_features(CesiumGame
    PRIVATE
        cxx_std_20
)

target_link_libraries(CesiumGame
    PRIVATE
        Frost::Engine
)

set_target_properties(CesiumGame PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(CesiumGame
        PRIVATE
            /W4
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
    )
else()
    target_compile_options(CesiumGame
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()