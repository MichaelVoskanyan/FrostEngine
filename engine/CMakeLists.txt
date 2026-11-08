add_library(FrostEngine STATIC)
add_library(Frost::Engine ALIAS FrostEngine)

target_sources(FrostEngine
    PRIVATE
        src/Animation/AnimationClip.cpp
        src/Animation/AnimationSystem.cpp
        src/Animation/Skeleton.cpp
        src/Animation/Socket.cpp
        src/Animation/SkinnedMesh.cpp
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
        src/Assets/Sound.cpp
        src/Audio/AudioEngine.cpp
        src/Audio/AudioSystem.cpp
        src/Debug/DebugDraw.cpp
        src/Debug/DebugDrawImGui.cpp
        src/Debug/PhysicsDebugDraw.cpp
        src/Engine.cpp
        src/ImGui/ImGuiLayer.cpp
        src/Physics/Collision.cpp
        src/Physics/PhysicsSystem.cpp
        src/Physics/ColliderQuery.hpp
        src/Physics/Overlap.cpp
        src/Physics/PhysicsQuerySystem.cpp
        src/Physics/Raycast.cpp
        src/Physics/ShapeCast.cpp
        src/Platform/Input.cpp
        src/Platform/KeyNames.cpp
        src/Platform/Window.cpp
        src/Project/GameLaunchOptions.cpp
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
        src/Scene/Components/AnimationTriggerComponent.cpp
        src/Scene/Components/AudioListenerComponent.cpp
        src/Scene/Components/AudioSourceComponent.cpp
        src/Scene/Components/CameraComponent.cpp
        src/Scene/Components/CharacterControllerComponent.cpp
        src/Scene/Components/FlyControllerComponent.cpp
        src/Scene/Components/LightComponent.cpp
        src/Scene/Components/MaterialOverrides.cpp
        src/Scene/Components/MeshComponent.cpp
        src/Scene/Components/PhysicsQueryComponent.cpp
        src/Scene/Components/SkeletalMeshComponent.cpp
        src/Scene/Components/TransformComponent.cpp
        src/Scene/GameObject.cpp
        src/Scene/Hierarchy.cpp
        src/Scene/Level.cpp
        src/Scene/LevelSerializer.cpp

    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS
            include
        FILES
            include/Frost/Animation/AnimationClip.hpp
            include/Frost/Animation/AnimationSystem.hpp
            include/Frost/Animation/Skeleton.hpp
            include/Frost/Animation/Socket.hpp
            include/Frost/Animation/SkinnedMesh.hpp
            include/Frost/Application.hpp
            include/Frost/Audio/AudioEngine.hpp
            include/Frost/Audio/AudioSystem.hpp
            include/Frost/Assets/AssetFile.hpp
            include/Frost/Assets/AssetId.hpp
            include/Frost/Assets/AssetImporter.hpp
            include/Frost/Assets/AssetManager.hpp
            include/Frost/Assets/AssetRegistry.hpp
            include/Frost/Assets/AssetType.hpp
            include/Frost/Assets/GltfImporter.hpp
            include/Frost/Assets/Image.hpp
            include/Frost/Assets/ImportError.hpp
            include/Frost/Assets/Sound.hpp
            include/Frost/Debug/DebugDraw.hpp
            include/Frost/Debug/PhysicsDebugDraw.hpp
            include/Frost/Engine.hpp
            include/Frost/ImGui/ImGuiLayer.hpp
            include/Frost/Physics/Collision.hpp
            include/Frost/Physics/CollisionFilter.hpp
            include/Frost/Physics/Overlap.hpp
            include/Frost/Physics/PhysicsQuerySystem.hpp
            include/Frost/Physics/PhysicsSystem.hpp
            include/Frost/Physics/Raycast.hpp
            include/Frost/Physics/ShapeCast.hpp
            include/Frost/Platform/Filesystem.hpp
            include/Frost/Platform/Input.hpp
            include/Frost/Platform/KeyNames.hpp
            include/Frost/Platform/Window.hpp
            include/Frost/Project/GameLaunchOptions.hpp
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
            include/Frost/Scene/Components/AnimationTriggerComponent.hpp
            include/Frost/Scene/Components/AttachmentComponent.hpp
            include/Frost/Scene/Components/AudioListenerComponent.hpp
            include/Frost/Scene/Components/AudioSourceComponent.hpp
            include/Frost/Scene/Components/BoxColliderComponent.hpp
            include/Frost/Scene/Components/CameraComponent.hpp
            include/Frost/Scene/Components/CapsuleColliderComponent.hpp
            include/Frost/Scene/Components/CharacterControllerComponent.hpp
            include/Frost/Scene/Components/FlyControllerComponent.hpp
            include/Frost/Scene/Components/Components.hpp
            include/Frost/Scene/Components/LightComponent.hpp
            include/Frost/Scene/Components/MaterialOverrides.hpp
            include/Frost/Scene/Components/MeshComponent.hpp
            include/Frost/Scene/Components/PhysicsQueryComponent.hpp
            include/Frost/Scene/Components/RigidBodyComponent.hpp
            include/Frost/Scene/Components/SkeletalMeshComponent.hpp
            include/Frost/Scene/Components/TransformComponent.hpp
            include/Frost/Scene/GameObject.hpp
            include/Frost/Scene/Hierarchy.hpp
            include/Frost/Scene/Level.hpp
            include/Frost/Scene/Ray.hpp
            include/Frost/Scene/LevelSerializer.hpp
            include/Frost/Scene/Ray.hpp
            include/Frost/Scene/Registry.hpp
            include/Frost/Units.hpp
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

# miniaudio (see cmake/Dependencies.cmake): its library, or its header, whose implementation
# AudioEngine.cpp then compiles (SYSTEM: its warnings are not the engine's).
if(FROST_MINIAUDIO_TARGET)
    target_link_libraries(FrostEngine PRIVATE ${FROST_MINIAUDIO_TARGET})
else()
    target_include_directories(FrostEngine SYSTEM PRIVATE ${Miniaudio_INCLUDE_DIR})
    target_compile_definitions(FrostEngine PRIVATE FROST_MINIAUDIO_IMPLEMENTATION)
    target_link_libraries(FrostEngine PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(UNIX AND NOT APPLE)
        target_link_libraries(FrostEngine PRIVATE m)
    endif()
endif()

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
