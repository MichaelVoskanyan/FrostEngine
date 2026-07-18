add_library(FrostEngine STATIC)
add_library(Frost::Engine ALIAS FrostEngine)

target_sources(FrostEngine
    PRIVATE
        src/Application.cpp
        src/Engine.cpp
        src/Platform/Window.cpp
        src/Renderer/Vulkan/VulkanContext.cpp

    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS
            include
        FILES
            include/Frost/Application.hpp
            include/Frost/Engine.hpp
            include/Frost/Platform/Window.hpp
            include/Frost/Renderer/Vulkan/VulkanContext.hpp
)

target_compile_features(FrostEngine
    PUBLIC
        cxx_std_20
)

target_include_directories(FrostEngine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(FrostEngine
    PUBLIC
        Vulkan::Vulkan

    PRIVATE
        glfw
        glm::glm
        GPUOpen::VulkanMemoryAllocator
)

if(MSVC)
    target_compile_options(FrostEngine
        PRIVATE
            /W4
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
    )
else()
    target_compile_options(FrostEngine
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
    )
endif()

set_target_properties(FrostEngine PROPERTIES
    CXX_EXTENSIONS OFF
    DEBUG_POSTFIX "d"
)