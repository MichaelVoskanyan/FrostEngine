add_library(FrostEngine STATIC)
add_library(Frost::Engine ALIAS FrostEngine)

target_sources(FrostEngine
    PRIVATE
        src/Application.cpp
        src/Assets/AssetFile.cpp
        src/Assets/AssetId.cpp
        src/Assets/AssetImporter.cpp
        src/Assets/AssetManager.cpp
        src/Assets/AssetRegistry.cpp
        src/Assets/GltfImporter.cpp
        src/Assets/Image.cpp
        src/Assets/Json.cpp
        src/Assets/Json.hpp
        src/Engine.cpp
        src/ImGui/ImGuiLayer.cpp
        src/Platform/Input.cpp
        src/Platform/Window.cpp
        src/Project/Project.cpp
        src/Renderer/Camera.cpp
        src/Renderer/Light.cpp
        src/Renderer/Material.cpp
        src/Renderer/MaterialSystem.cpp
        src/Renderer/Mesh.cpp
        src/Renderer/Renderer.cpp
        src/Renderer/Texture.cpp
        src/Renderer/Vertex.cpp
        src/Renderer/Vulkan/VulkanAllocator.cpp
        src/Renderer/Vulkan/VulkanBuffer.cpp
        src/Renderer/Vulkan/VulkanCommandPool.cpp
        src/Renderer/Vulkan/VulkanContext.cpp
        src/Renderer/Vulkan/VulkanDescriptorAllocator.cpp
        src/Renderer/Vulkan/VulkanPhysicalDevice.cpp
        src/Renderer/Vulkan/VulkanDevice.cpp
        src/Renderer/Vulkan/VulkanImage.cpp
        src/Renderer/Vulkan/VulkanPipeline.cpp
        src/Renderer/Vulkan/VulkanSwapchain.cpp
        src/Scene/GameObject.cpp
        src/Scene/Level.cpp
        src/Scene/LevelSerializer.cpp

    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS
            include
        FILES
            include/Frost/Application.hpp
            include/Frost/Assets/AssetFile.hpp
            include/Frost/Assets/AssetId.hpp
            include/Frost/Assets/AssetImporter.hpp
            include/Frost/Assets/AssetManager.hpp
            include/Frost/Assets/AssetRegistry.hpp
            include/Frost/Assets/AssetType.hpp
            include/Frost/Assets/GltfImporter.hpp
            include/Frost/Assets/Image.hpp
            include/Frost/Assets/ImportError.hpp
            include/Frost/Engine.hpp
            include/Frost/ImGui/ImGuiLayer.hpp
            include/Frost/Platform/Filesystem.hpp
            include/Frost/Platform/Input.hpp
            include/Frost/Platform/Window.hpp
            include/Frost/Project/Project.hpp
            include/Frost/Renderer/Camera.hpp
            include/Frost/Renderer/Light.hpp
            include/Frost/Renderer/Material.hpp
            include/Frost/Renderer/MaterialSystem.hpp
            include/Frost/Renderer/Mesh.hpp
            include/Frost/Renderer/RenderOverlay.hpp
            include/Frost/Renderer/Renderer.hpp
            include/Frost/Renderer/ShaderData.hpp
            include/Frost/Renderer/Texture.hpp
            include/Frost/Renderer/Vertex.hpp
            include/Frost/Renderer/Vulkan/VulkanAllocator.hpp
            include/Frost/Renderer/Vulkan/VulkanBuffer.hpp
            include/Frost/Renderer/Vulkan/VulkanCommandPool.hpp
            include/Frost/Renderer/Vulkan/VulkanContext.hpp
            include/Frost/Renderer/Vulkan/VulkanDescriptorAllocator.hpp
            include/Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp
            include/Frost/Renderer/Vulkan/VulkanDevice.hpp
            include/Frost/Renderer/Vulkan/VulkanImage.hpp
            include/Frost/Renderer/Vulkan/VulkanPipeline.hpp
            include/Frost/Renderer/Vulkan/VulkanSwapchain.hpp
            include/Frost/Scene/Bounds.hpp
            include/Frost/Scene/GameObject.hpp
            include/Frost/Scene/Level.hpp
            include/Frost/Scene/LevelSerializer.hpp
)

target_compile_features(FrostEngine
    PUBLIC
        cxx_std_20
)

target_include_directories(FrostEngine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# stb_image is header-only and only included by src/Assets/Image.cpp; SYSTEM keeps its warnings
# out of the engine's -Wall/-W4 builds.
target_include_directories(FrostEngine
    SYSTEM PRIVATE
        ${Stb_INCLUDE_DIR}
)

target_link_libraries(FrostEngine
    PUBLIC
        Vulkan::Vulkan
        glm::glm

    # Dear ImGui is private: no engine header includes it. Applications that issue ImGui:: calls
    # (the editor) link imgui::imgui themselves; being a static library, there is still only the
    # one copy (and one global context) in the final executable.
    PRIVATE
        glfw
        GPUOpen::VulkanMemoryAllocator
        imgui::imgui
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
