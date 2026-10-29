add_library(FrostEngine STATIC)
add_library(Frost::Engine ALIAS FrostEngine)

target_sources(FrostEngine
    PRIVATE
        src/Application.cpp
        src/Engine.cpp
        src/Platform/Window.cpp
        src/Renderer/Mesh.cpp
        src/Renderer/Renderer.cpp
        src/Renderer/Vertex.cpp
        src/Renderer/Vulkan/VulkanAllocator.cpp
        src/Renderer/Vulkan/VulkanBuffer.cpp
        src/Renderer/Vulkan/VulkanCommandPool.cpp
        src/Renderer/Vulkan/VulkanContext.cpp
        src/Renderer/Vulkan/VulkanPhysicalDevice.cpp
        src/Renderer/Vulkan/VulkanDevice.cpp
        src/Renderer/Vulkan/VulkanPipeline.cpp
        src/Renderer/Vulkan/VulkanSwapchain.cpp

    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS
            include
        FILES
            include/Frost/Application.hpp
            include/Frost/Engine.hpp
            include/Frost/Platform/Window.hpp
            include/Frost/Renderer/Mesh.hpp
            include/Frost/Renderer/Renderer.hpp
            include/Frost/Renderer/Vertex.hpp
            include/Frost/Renderer/Vulkan/VulkanAllocator.hpp
            include/Frost/Renderer/Vulkan/VulkanBuffer.hpp
            include/Frost/Renderer/Vulkan/VulkanCommandPool.hpp
            include/Frost/Renderer/Vulkan/VulkanContext.hpp
            include/Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp
            include/Frost/Renderer/Vulkan/VulkanDevice.hpp
            include/Frost/Renderer/Vulkan/VulkanPipeline.hpp
            include/Frost/Renderer/Vulkan/VulkanSwapchain.hpp
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
        glm::glm

    PRIVATE
        glfw
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
