#include <Frost/Renderer/Renderer.hpp>

#include <Frost/Platform/Window.hpp>
#include <Frost/Renderer/Material.hpp>
#include <Frost/Renderer/MaterialSystem.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Renderer/RenderOverlay.hpp>
#include <Frost/Renderer/ShaderData.hpp>
#include <Frost/Renderer/Texture.hpp>
#include <Frost/Renderer/Vertex.hpp>
#include <Frost/Renderer/Vulkan/VulkanAllocator.hpp>
#include <Frost/Renderer/Vulkan/VulkanBuffer.hpp>
#include <Frost/Renderer/Vulkan/VulkanCommandPool.hpp>
#include <Frost/Renderer/Vulkan/VulkanContext.hpp>
#include <Frost/Renderer/Vulkan/VulkanDescriptorAllocator.hpp>
#include <Frost/Renderer/Vulkan/VulkanDevice.hpp>
#include <Frost/Renderer/Vulkan/VulkanImage.hpp>
#include <Frost/Renderer/Vulkan/VulkanPhysicalDevice.hpp>
#include <Frost/Renderer/Vulkan/VulkanPipeline.hpp>
#include <Frost/Renderer/Vulkan/VulkanSwapchain.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

    // How long shutdown will wait for outstanding presentation fences before destroying the
    // swapchain anyway. Presents complete in bounded time on a healthy system; the timeout only
    // matters when the presentation engine has gone away (e.g. surface lost) and guards against
    // hanging the process on exit.
    constexpr std::uint64_t kShutdownPresentTimeoutNs = 2'000'000'000;  // 2 s

    // Depth attachment candidates, best first: 32-bit float (the format reversed-Z is designed
    // for), then 24-bit fixed point, then 16-bit, which every implementation must support.
    constexpr std::array<VkFormat, 3> kDepthFormatCandidates = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32,
                                                                VK_FORMAT_D16_UNORM};

    // Reversed-Z: the far plane is at depth 0, so that is what "nothing drawn yet" is.
    constexpr float kDepthClearValue = 0.0f;

    [[nodiscard]]
    std::runtime_error VulkanError(const char* const operation, const VkResult result) {
        std::ostringstream message;

        message << operation << " failed with VkResult " << static_cast<int>(result) << '.';

        return std::runtime_error(message.str());
    }

}  // namespace

namespace Frost {

    Renderer::Renderer(Window& window) : m_Window(window) {
        m_Context = std::make_unique<VulkanContext>(window);
        m_PhysicalDevice = std::make_unique<VulkanPhysicalDevice>(*m_Context);
        m_Device = std::make_unique<VulkanDevice>(*m_PhysicalDevice);
        m_Allocator = std::make_unique<VulkanAllocator>(*m_Context, *m_PhysicalDevice, *m_Device);
        m_Swapchain = std::make_unique<VulkanSwapchain>(*m_Context, *m_PhysicalDevice, *m_Device, window);
        m_CommandPool = std::make_unique<VulkanCommandPool>(*m_PhysicalDevice, *m_Device);
        m_DescriptorAllocator = std::make_unique<VulkanDescriptorAllocator>(*m_Device);

        m_DepthFormat = ChooseDepthFormat();
        CreateDepthImage();

        CreatePipelines();

        m_CommandBuffers = m_CommandPool->AllocateCommandBuffers(*m_Device, kFramesInFlight);

        m_UsePresentFences = m_Device->HasSwapchainMaintenance1();
        m_PresentQueueIsGraphicsQueue = m_Device->GetPresentQueue() == m_Device->GetGraphicsQueue();

        // From here on raw (non-RAII) handles exist. If anything below throws, the destructor
        // will not run (the object was never fully constructed), so they have to be released here;
        // the unique_ptr members above unwind on their own.
        try {
            CreateSyncObjects();
            CreateFrameUniformBuffers();

            m_MaterialSystem =
                    std::make_unique<MaterialSystem>(*m_Allocator, *m_Device, *m_CommandPool, *m_DescriptorAllocator);

            m_FrameDraws.resize(kFramesInFlight);
        }
        catch (...) {
            vkDeviceWaitIdle(m_Device->GetHandle());
            DestroyFrameUniformBuffers();
            DestroySyncObjects();
            DestroyPresentFences();
            throw;
        }
    }

    Renderer::~Renderer() {
        // Shutdown is the one place a full idle is appropriate: nothing below may be destroyed
        // while the device could still be using it, and there is no later frame to defer to.
        vkDeviceWaitIdle(m_Device->GetHandle());

        // The renderer's own references to submitted meshes/materials. Anything still shared with
        // the application is the application's to release (before the renderer, see Engine).
        m_PendingDraws.clear();
        m_FrameDraws.clear();

        // Retired swapchains first (each bundle destroys its swapchain before its semaphores), then
        // the live swapchain, then the live semaphores, then the presentation fences none of them
        // reference any more. With presentation fences, `force` waits for outstanding presents to
        // complete, which is both the spec's requirement for destroying a swapchain that used them
        // and the only exact guarantee that the presentation engine is done with the semaphores.
        // Without them, the idle above is the best available (see ReleaseRetiredResources).
        ReleaseRetiredResources(true);

        if (m_UsePresentFences && !m_LivePresentFences.empty()) {
            const VkResult waited =
                    vkWaitForFences(m_Device->GetHandle(), static_cast<std::uint32_t>(m_LivePresentFences.size()),
                                    m_LivePresentFences.data(), VK_TRUE, kShutdownPresentTimeoutNs);
            if (waited != VK_SUCCESS) {
                std::cerr << "[Frost::Renderer] Presentation fences did not signal within the shutdown timeout "
                             "(VkResult "
                          << static_cast<int>(waited) << "); tearing down anyway.\n";
            }
        }

        m_Swapchain.reset();

        DestroyFrameUniformBuffers();
        DestroySyncObjects();
        DestroyPresentFences();
    }

    void Renderer::SetCamera(const Camera& camera) { m_Camera = camera; }

    void Renderer::SetOverlay(RenderOverlay* const overlay) noexcept { m_Overlay = overlay; }

    const VulkanContext& Renderer::GetContext() const noexcept { return *m_Context; }

    const VulkanPhysicalDevice& Renderer::GetPhysicalDevice() const noexcept { return *m_PhysicalDevice; }

    const VulkanDevice& Renderer::GetDevice() const noexcept { return *m_Device; }

    VkFormat Renderer::GetSwapchainImageFormat() const noexcept { return m_Swapchain->GetImageFormat(); }

    std::shared_ptr<Mesh> Renderer::CreateMesh(const MeshData& data, const std::span<const SkinnedVertex> skin) const {
        // Skinned meshes carry no embedded materials of their own here: their materials are
        // attached by whoever loads them (see AssetManager::LoadSkeletalMesh).
        return std::make_shared<Mesh>(*m_Allocator, *m_Device, *m_CommandPool, data, skin);
    }

    std::shared_ptr<Mesh> Renderer::CreateMesh(const MeshData& data) const {
        auto mesh = std::make_shared<Mesh>(*m_Allocator, *m_Device, *m_CommandPool, data);

        // One Texture per imported texture and one Material per imported material, shared by
        // everything that references them.
        std::vector<std::shared_ptr<Texture>> textures;
        textures.reserve(data.textures.size());
        for (const TextureData& textureData : data.textures) {
            textures.push_back(m_MaterialSystem->CreateTexture(textureData.image, textureData.settings));
        }

        auto textureAt = [&textures](const std::int32_t index) -> std::shared_ptr<Texture> {
            if (index < 0) { return nullptr; }
            if (static_cast<std::size_t>(index) >= textures.size()) {
                throw std::runtime_error("Mesh data references a texture index that does not exist.");
            }
            return textures[static_cast<std::size_t>(index)];
        };

        std::vector<std::shared_ptr<Material>> materials;
        materials.reserve(data.materials.size());
        for (const MaterialData& materialData : data.materials) {
            materials.push_back(m_MaterialSystem->CreateMaterial(
                    materialData.parameters,
                    MaterialTextures{.baseColor = textureAt(materialData.baseColorTexture),
                                     .metallicRoughness = textureAt(materialData.metallicRoughnessTexture),
                                     .normal = textureAt(materialData.normalTexture),
                                     .occlusion = textureAt(materialData.occlusionTexture),
                                     .emissive = textureAt(materialData.emissiveTexture)}));
        }

        for (std::size_t i = 0; i < data.subMeshes.size(); ++i) {
            const std::int32_t materialIndex = data.subMeshes[i].materialIndex;
            if (materialIndex < 0) { continue; }

            if (static_cast<std::size_t>(materialIndex) >= materials.size()) {
                throw std::runtime_error("Mesh data references a material index that does not exist.");
            }
            mesh->SetMaterial(i, materials[static_cast<std::size_t>(materialIndex)]);
        }

        return mesh;
    }

    void Renderer::Submit(std::shared_ptr<const Mesh> mesh, const glm::mat4& transform,
                          std::vector<std::shared_ptr<Material>> materialOverrides) {
        if (mesh == nullptr) { return; }
        if (mesh->IsSkinned()) {
            if (!m_SkinnedMisuseWarned) {
                m_SkinnedMisuseWarned = true;
                std::cerr << "[Frost::Renderer] A skinned mesh was submitted without a palette (use SubmitSkinned); "
                             "it is not drawn.\n";
            }
            return;
        }

        m_PendingDraws.push_back(DrawCommand{.mesh = std::move(mesh),
                                             .transform = transform,
                                             .materialOverrides = std::move(materialOverrides),
                                             .firstJoint = 0});
    }

    void Renderer::SubmitSkinned(std::shared_ptr<const Mesh> mesh, const glm::mat4& transform,
                                 std::vector<std::shared_ptr<Material>> materialOverrides,
                                 const std::span<const glm::mat4> palette) {
        if (mesh == nullptr) { return; }

        // The shader indexes the palette with the mesh's joint indices: a short palette would have
        // it read other draws' matrices (or past the buffer), so such a draw is refused.
        if (!mesh->IsSkinned() || palette.size() < mesh->GetJointCount()) {
            if (!m_SkinnedMisuseWarned) {
                m_SkinnedMisuseWarned = true;
                std::cerr << "[Frost::Renderer] SubmitSkinned: " << (mesh->IsSkinned() ? "the palette is too short for the mesh"
                                                                                      : "the mesh is not skinned")
                          << "; it is not drawn.\n";
            }
            return;
        }

        if (m_PendingJoints.size() + palette.size() > kMaxJointMatrices) {
            if (!m_JointLimitWarned) {
                m_JointLimitWarned = true;
                std::cerr << "[Frost::Renderer] More than " << kMaxJointMatrices
                          << " joint matrices submitted in one frame; the skinned meshes past that are not drawn.\n";
            }
            return;
        }

        const auto firstJoint = static_cast<std::uint32_t>(m_PendingJoints.size());
        m_PendingJoints.insert(m_PendingJoints.end(), palette.begin(), palette.end());
        m_PendingDraws.push_back(DrawCommand{.mesh = std::move(mesh),
                                             .transform = transform,
                                             .materialOverrides = std::move(materialOverrides),
                                             .firstJoint = firstJoint});
    }

    void Renderer::SubmitLight(const Light& light) {
        if (m_PendingLights.size() >= kMaxLights) {
            if (!m_LightLimitWarned) {
                m_LightLimitWarned = true;
                std::cerr << "[Frost::Renderer] More than " << kMaxLights
                          << " lights submitted in one frame; the extra lights are ignored.\n";
            }
            return;
        }

        m_PendingLights.push_back(light);
    }

    void Renderer::SetAmbientLight(const glm::vec3& color) noexcept { m_AmbientLight = color; }

    const MaterialSystem& Renderer::GetMaterialSystem() const noexcept { return *m_MaterialSystem; }

    void Renderer::SetSceneViewport(const std::optional<SceneViewport>& viewport) noexcept {
        m_SceneViewport = viewport;
    }

    VkFormat Renderer::GetDepthFormat() const noexcept { return m_DepthFormat; }

    VkFormat Renderer::ChooseDepthFormat() const {
        for (const VkFormat format : kDepthFormatCandidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice->GetHandle(), format, &properties);

            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                return format;
            }
        }

        // Unreachable on a conformant implementation (D16_UNORM is mandatory).
        throw std::runtime_error("Renderer: the device supports none of the candidate depth formats.");
    }

    void Renderer::CreateDepthImage() {
        m_DepthImage = std::make_unique<VulkanImage>(*m_Allocator, *m_Device, m_Swapchain->GetExtent(), m_DepthFormat,
                                                     1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

    VkRect2D Renderer::GetSceneRect() const noexcept {
        const VkExtent2D extent = m_Swapchain->GetExtent();
        if (!m_SceneViewport.has_value()) { return VkRect2D{.offset = {0, 0}, .extent = extent}; }

        // Clamp to the image: a viewport panel can hang off the window's edge, and the rectangle
        // comes from UI layout while the extent may just have changed.
        const SceneViewport& viewport = *m_SceneViewport;
        const std::int64_t width = extent.width;
        const std::int64_t height = extent.height;

        const std::int64_t left = std::clamp<std::int64_t>(viewport.x, 0, width);
        const std::int64_t top = std::clamp<std::int64_t>(viewport.y, 0, height);
        const std::int64_t right = std::clamp<std::int64_t>(std::int64_t{viewport.x} + viewport.width, left, width);
        const std::int64_t bottom = std::clamp<std::int64_t>(std::int64_t{viewport.y} + viewport.height, top, height);

        return VkRect2D{.offset = {static_cast<std::int32_t>(left), static_cast<std::int32_t>(top)},
                        .extent = {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}};
    }

    void Renderer::CreateFrameUniformBuffers() {
        m_FrameUniforms.resize(kFramesInFlight);

        for (FrameUniformBuffer& frame : m_FrameUniforms) {
            // Host-visible (persistently mapped) rather than device-local: it is rewritten every
            // frame and read once, so a staging upload would cost more than it saves.
            frame.buffer = std::make_unique<VulkanBuffer>(*m_Allocator, sizeof(FrameUniforms),
                                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
            // Sized for the maximum up front (64 KiB): only the frame's lights are written each
            // frame, and a fixed size means the descriptor never has to be rewritten.
            frame.lightBuffer = std::make_unique<VulkanBuffer>(*m_Allocator, sizeof(GpuLight) * kMaxLights,
                                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            // Likewise the joint matrices (2 MiB): fixed size, only the frame's palettes written.
            frame.jointBuffer = std::make_unique<VulkanBuffer>(*m_Allocator, sizeof(GpuJointMatrix) * kMaxJointMatrices,
                                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
            frame.descriptorSet = m_DescriptorAllocator->Allocate(m_DescriptorAllocator->GetFrameSetLayout());

            const VkDescriptorBufferInfo bufferInfo{
                    .buffer = frame.buffer->GetHandle(),
                    .offset = 0,
                    .range = sizeof(FrameUniforms),
            };

            const VkDescriptorBufferInfo lightBufferInfo{
                    .buffer = frame.lightBuffer->GetHandle(),
                    .offset = 0,
                    .range = VK_WHOLE_SIZE,
            };

            const VkDescriptorBufferInfo jointBufferInfo{
                    .buffer = frame.jointBuffer->GetHandle(),
                    .offset = 0,
                    .range = VK_WHOLE_SIZE,
            };

            const std::array<VkWriteDescriptorSet, 3> writes = {{
                    {
                            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            .pNext = nullptr,
                            .dstSet = frame.descriptorSet,
                            .dstBinding = 0,
                            .dstArrayElement = 0,
                            .descriptorCount = 1,
                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                            .pImageInfo = nullptr,
                            .pBufferInfo = &bufferInfo,
                            .pTexelBufferView = nullptr,
                    },
                    {
                            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            .pNext = nullptr,
                            .dstSet = frame.descriptorSet,
                            .dstBinding = 1,
                            .dstArrayElement = 0,
                            .descriptorCount = 1,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .pImageInfo = nullptr,
                            .pBufferInfo = &lightBufferInfo,
                            .pTexelBufferView = nullptr,
                    },
                    {
                            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            .pNext = nullptr,
                            .dstSet = frame.descriptorSet,
                            .dstBinding = 2,
                            .dstArrayElement = 0,
                            .descriptorCount = 1,
                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                            .pImageInfo = nullptr,
                            .pBufferInfo = &jointBufferInfo,
                            .pTexelBufferView = nullptr,
                    },
            }};

            vkUpdateDescriptorSets(m_Device->GetHandle(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                                   nullptr);

            // Valid contents from the start (identity matrices), should a frame ever be recorded
            // before its first update.
            const FrameUniforms initial{};
            frame.buffer->CopyData(&initial, sizeof(initial));
        }
    }

    void Renderer::DestroyFrameUniformBuffers() noexcept {
        for (FrameUniformBuffer& frame : m_FrameUniforms) {
            if (frame.descriptorSet != VK_NULL_HANDLE) {
                m_DescriptorAllocator->Free(frame.descriptorSet);
                frame.descriptorSet = VK_NULL_HANDLE;
            }
            frame.buffer.reset();
            frame.lightBuffer.reset();
            frame.jointBuffer.reset();
        }

        m_FrameUniforms.clear();
    }

    void Renderer::UpdateFrameUniforms(const std::uint32_t frameIndex, const VkRect2D& sceneRect,
                                       const std::vector<Light>& lights, const std::vector<glm::mat4>& joints) const {
        // The rectangle may be empty (a collapsed viewport panel); nothing is drawn then, so any
        // finite aspect will do.
        const float aspect = sceneRect.extent.width > 0 && sceneRect.extent.height > 0
                                     ? static_cast<float>(sceneRect.extent.width) / static_cast<float>(sceneRect.extent.height)
                                     : 1.0f;

        glm::mat4 view(1.0f);
        glm::mat4 projection(1.0f);

        if (m_Camera.has_value()) {
            // The camera's projection is built here, against the extent actually being rendered,
            // so it can never disagree with the target's shape (e.g. right after a resize).
            view = m_Camera->GetViewMatrix();
            projection = m_Camera->GetProjectionMatrix(aspect);
        }
        else {
            // No camera set (e.g. the game, which has none yet): the view is the identity (the
            // viewer sits at the origin looking down -Z, +Y up, matching the flipped viewport in
            // RecordCommandBuffer). The projection is the placeholder that keeps the picture sane
            // without one: an orthographic projection that maps the *shorter* framebuffer axis to
            // [-1, 1] and widens the longer one by the aspect ratio, so the model keeps its
            // proportions, stays centered and is never clipped at any window shape; its depth
            // range [-1, 1] -> [0, 1] (Vulkan's) also keeps geometry slightly behind the origin
            // from being clipped by the near plane. The planes are passed swapped for reversed-Z
            // (view z = +1, nearest the viewer, maps to depth 1), like Camera's projection.
            const float halfWidth = aspect >= 1.0f ? aspect : 1.0f;
            const float halfHeight = aspect >= 1.0f ? 1.0f : 1.0f / aspect;

            projection = glm::orthoRH_ZO(-halfWidth, halfWidth, -halfHeight, halfHeight, 1.0f, -1.0f);
        }

        // The lights, in the layout the shaders read (see GpuLight). Submit() already capped the
        // count at kMaxLights.
        std::vector<GpuLight> gpuLights;
        gpuLights.reserve(lights.size());
        for (const Light& light : lights) {
            GpuLight gpu{};
            gpu.color = glm::vec4(light.color, 0.0f);

            switch (light.type) {
                case LightType::Directional:
                    gpu.directionType = glm::vec4(light.direction, static_cast<float>(GpuLightType::Directional));
                    break;

                case LightType::Point:
                    gpu.positionRange = glm::vec4(light.position, light.range);
                    gpu.directionType = glm::vec4(light.direction, static_cast<float>(GpuLightType::Point));
                    break;

                case LightType::Spot: {
                    gpu.positionRange = glm::vec4(light.position, light.range);
                    gpu.directionType = glm::vec4(light.direction, static_cast<float>(GpuLightType::Spot));

                    // Precomputed so the shader's cone falloff is one multiply-add: the falloff
                    // runs from outerConeCos (0) to innerConeCos (1). The difference is kept away
                    // from zero so equal cones give a hard edge rather than a division by zero.
                    const float coneDifference = std::max(light.innerConeCos - light.outerConeCos, 1.0e-4f);
                    gpu.spotCone = glm::vec4(light.outerConeCos, 1.0f / coneDifference, 0.0f, 0.0f);
                    break;
                }
            }

            gpuLights.push_back(gpu);
        }

        const FrameUniforms uniforms{
                .view = view,
                .projection = projection,
                .cameraPosition = glm::inverse(view)[3],
                .ambientColor = glm::vec4(m_AmbientLight, 1.0f),
                .lightCount = glm::uvec4(static_cast<std::uint32_t>(gpuLights.size()), 0u, 0u, 0u),
        };

        m_FrameUniforms[frameIndex].buffer->CopyData(&uniforms, sizeof(uniforms));
        if (!gpuLights.empty()) {
            m_FrameUniforms[frameIndex].lightBuffer->CopyData(gpuLights.data(), sizeof(GpuLight) * gpuLights.size());
        }
        // SubmitSkinned already capped the count at kMaxJointMatrices.
        if (!joints.empty()) {
            m_FrameUniforms[frameIndex].jointBuffer->CopyData(joints.data(), sizeof(GpuJointMatrix) * joints.size());
        }
    }

    void Renderer::CreatePipelines() {
        m_Pipeline = std::make_unique<VulkanPipeline>(*m_Device, m_Swapchain->GetImageFormat(), m_DepthFormat,
                                                      m_DescriptorAllocator->GetFrameSetLayout(),
                                                      m_DescriptorAllocator->GetMaterialSetLayout(),
                                                      "shaders/basic.vert.spv", "shaders/basic.frag.spv",
                                                      VertexLayout::Static);
        m_SkinnedPipeline = std::make_unique<VulkanPipeline>(*m_Device, m_Swapchain->GetImageFormat(), m_DepthFormat,
                                                             m_DescriptorAllocator->GetFrameSetLayout(),
                                                             m_DescriptorAllocator->GetMaterialSetLayout(),
                                                             "shaders/skinned.vert.spv", "shaders/basic.frag.spv",
                                                             VertexLayout::Skinned);
    }

    void Renderer::CreateSyncObjects() {
        m_FrameSync.resize(kFramesInFlight);

        constexpr VkSemaphoreCreateInfo semaphoreInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
        };

        constexpr VkFenceCreateInfo fenceInfo{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };

        const VkDevice device = m_Device->GetHandle();

        for (FrameSync& sync : m_FrameSync) {
            VkResult result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &sync.imageAvailable);
            if (result != VK_SUCCESS) { throw VulkanError("vkCreateSemaphore", result); }

            result = vkCreateFence(device, &fenceInfo, nullptr, &sync.inFlight);
            if (result != VK_SUCCESS) { throw VulkanError("vkCreateFence", result); }
        }

        CreateRenderFinishedSemaphores();
    }

    void Renderer::CreateRenderFinishedSemaphores() {
        constexpr VkSemaphoreCreateInfo semaphoreInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
        };

        const VkDevice device = m_Device->GetHandle();

        // Sized to the swapchain's actual image count (which may exceed kFramesInFlight), not the
        // frame-in-flight count; see the m_RenderFinished declaration for why.
        m_RenderFinished.resize(m_Swapchain->GetImages().size(), VK_NULL_HANDLE);

        for (VkSemaphore& renderFinished : m_RenderFinished) {
            const VkResult result = vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinished);
            if (result != VK_SUCCESS) { throw VulkanError("vkCreateSemaphore", result); }
        }
    }

    void Renderer::DestroyRenderFinishedSemaphores() noexcept {
        const VkDevice device = m_Device->GetHandle();

        for (const VkSemaphore renderFinished : m_RenderFinished) {
            if (renderFinished != VK_NULL_HANDLE) { vkDestroySemaphore(device, renderFinished, nullptr); }
        }

        m_RenderFinished.clear();
    }

    void Renderer::DestroySyncObjects() noexcept {
        const VkDevice device = m_Device->GetHandle();

        DestroyRenderFinishedSemaphores();

        for (FrameSync& sync : m_FrameSync) {
            if (sync.imageAvailable != VK_NULL_HANDLE) { vkDestroySemaphore(device, sync.imageAvailable, nullptr); }
            if (sync.inFlight != VK_NULL_HANDLE) { vkDestroyFence(device, sync.inFlight, nullptr); }
        }

        m_FrameSync.clear();
    }

    bool Renderer::RecreateSwapchain() {
        // No vkDeviceWaitIdle here. The replaced resources are not destroyed but moved into
        // m_Retired, tagged with the last submitted frame number; ReleaseRetiredResources destroys
        // them later, once the frame fences prove the GPU is done and the presentation engine is
        // known (or, without presentation fences, reasonably inferred) to be done with them.
        //
        // The surface can have collapsed to zero area between the window's last event poll and
        // now (the OS changes window state asynchronously). Recreate() refuses that case without
        // retiring anything, so the existing swapchain stays usable for recovery. Recovery then
        // needs no bookkeeping: either the window reports zero size next frame (frames are skipped
        // until restoration fires a fresh resize event), or the stale swapchain's next acquire/
        // present returns VK_ERROR_OUT_OF_DATE_KHR and lands back here.
        // Everything that belonged to the old swapchain goes with it: its per-image semaphores
        // (the presentation engine may still be waiting on them) and the fences of presents issued
        // on it. The last submission that can reference any of this is the most recent one. The
        // bundle is parked in m_Retired *before* anything below can throw: a Vulkan failure here is
        // fatal by policy (the exception propagates and the process exits), but ~Renderer still
        // runs during unwinding and must find these handles somewhere it can release them. The
        // same applies if Recreate() itself throws after it has moved the old resources out.
        RetiredResources retired;
        retired.retireFrame = m_SubmittedFrame;

        bool recreated = false;
        try {
            recreated = m_Swapchain->Recreate(*m_Context, *m_PhysicalDevice, m_Window, retired.swapchain);
        }
        catch (...) {
            if (retired.swapchain.swapchain != VK_NULL_HANDLE) { m_Retired.push_back(std::move(retired)); }
            throw;
        }

        if (!recreated) { return false; }

        retired.depthImage = std::move(m_DepthImage);
        retired.renderFinished = std::move(m_RenderFinished);
        m_RenderFinished.clear();
        retired.presentFences = std::move(m_LivePresentFences);
        m_LivePresentFences.clear();

        m_Retired.push_back(std::move(retired));

        // The depth buffer must match the new extent. The old one may still be in use by frames in
        // flight, so it retired with the swapchain above.
        CreateDepthImage();

        // Viewport/scissor/render area are all taken from the swapchain each frame, so the new
        // extent is picked up automatically. The pipeline only bakes in the attachment format,
        // which ChooseSurfaceFormat keeps stable for a given surface; rebuild it in the unlikely
        // event the surface's format set changed underneath us. The old one may still be bound by
        // in-flight command buffers, so it retires with the swapchain rather than being destroyed.
        if (m_Pipeline->GetColorAttachmentFormat() != m_Swapchain->GetImageFormat()) {
            m_Retired.back().pipeline = std::move(m_Pipeline);
            m_Retired.back().skinnedPipeline = std::move(m_SkinnedPipeline);
            CreatePipelines();
        }

        // Keyed by swapchain image, and the image count can change with the extent.
        CreateRenderFinishedSemaphores();

        // Backstop against unbounded growth if presents were never to complete (which would also
        // mean nothing is being displayed): trade a one-off stall for bounded resource usage. Being
        // here at all is abnormal (bundles normally release within a few frames), so say so.
        if (m_Retired.size() >= kMaxRetiredSwapchains) {
            std::cerr << "[Frost::Renderer] " << m_Retired.size()
                      << " retired swapchains pending; falling back to vkDeviceWaitIdle to release them.\n";

            // Checked: the forced release below relies on the idle having actually completed. If it
            // failed (VK_ERROR_DEVICE_LOST), destroying resources as if it had would be a real race.
            const VkResult idled = vkDeviceWaitIdle(m_Device->GetHandle());
            if (idled != VK_SUCCESS) { throw VulkanError("vkDeviceWaitIdle", idled); }

            ReleaseRetiredResources(true);
        }

        // Any resize events that accumulated up to this point are satisfied by the rebuild just
        // performed; clear them so the next Tick() does not rebuild again for nothing.
        static_cast<void>(m_Window.ConsumeFramebufferResize());

        return true;
    }

    void Renderer::ReleaseRetiredResources(const bool force) {
        const VkDevice device = m_Device->GetHandle();

        // Presentation fences of presents on the live swapchain: recycle the signaled ones so the
        // pool does not grow with the number of frames presented.
        if (m_UsePresentFences) {
            RecycleSignaledPresentFences(m_LivePresentFences);

            if (!m_PresentFenceGrowthWarned && m_LivePresentFences.size() > kLivePresentFenceWarningCount) {
                m_PresentFenceGrowthWarned = true;
                std::cerr << "[Frost::Renderer] " << m_LivePresentFences.size()
                          << " presentation fences outstanding on the live swapchain; the presentation engine "
                             "does not appear to be signaling them.\n";
            }
        }

        for (std::size_t i = 0; i < m_Retired.size();) {
            RetiredResources& retired = m_Retired[i];

            // Diagnostic (once per bundle): lingering this long means a presentation fence is not
            // signaling, or the fallback inference never became true. Not an error by itself -
            // the backstop bounds it - but on real drivers it is the symptom worth seeing.
            if (!retired.retentionWarned && m_SubmittedFrame - retired.retireFrame > kRetentionWarningFrames) {
                retired.retentionWarned = true;
                std::cerr << "[Frost::Renderer] Retired swapchain still pending after "
                          << (m_SubmittedFrame - retired.retireFrame) << " frames ("
                          << retired.presentFences.size() << " presentation fence(s) unsignaled, "
                          << (m_UsePresentFences ? "presentation-fence" : "fallback") << " tracking).\n";
            }

            // (a) GPU side, exact: every submission numbered <= retireFrame has completed once a
            //     fence for a frame >= retireFrame has been observed signaled (submission-order
            //     rule for fence signal operations).
            const bool gpuDone = force || m_CompletedFrame >= retired.retireFrame;

            if (!gpuDone) {
                ++i;
                continue;
            }

            // (b) Presentation side.
            bool presentDone = false;

            if (m_UsePresentFences) {
                // Exact: each presentation fence signals once the presentation engine has finished
                // with that present's wait semaphores (and, implicitly, issued it to the display
                // pipeline). With `force` we block; otherwise poll.
                if (force && !retired.presentFences.empty()) {
                    const VkResult waited =
                            vkWaitForFences(device, static_cast<std::uint32_t>(retired.presentFences.size()),
                                            retired.presentFences.data(), VK_TRUE, kShutdownPresentTimeoutNs);
                    if (waited != VK_SUCCESS) {
                        std::cerr << "[Frost::Renderer] Presentation fences of a retired swapchain did not signal "
                                     "within the timeout (VkResult "
                                  << static_cast<int>(waited) << "); destroying it anyway.\n";
                    }
                }

                RecycleSignaledPresentFences(retired.presentFences);
                presentDone = retired.presentFences.empty() || force;
            }
            else if (force) {
                // Caller has already idled the device. Core Vulkan defines vkQueue/DeviceWaitIdle in
                // terms of fences on *submissions*, which a present is not, so strictly this is the
                // same inference as below; it is also exactly what every fence-less teardown does.
                presentDone = true;
            }
            else if (m_PresentQueueIsGraphicsQueue) {
                // Heuristic, not spec-guaranteed: the present's semaphore wait was enqueued on this
                // queue before frame retireFrame + 1 was submitted to it. Queues process operations
                // in order, so that later frame's fence having signaled implies the wait has executed.
                // This is the same in-order assumption a vkQueueWaitIdle-based teardown relies on,
                // without the stall.
                presentDone = m_CompletedFrame > retired.retireFrame;
            }
            else {
                // Separate present queue: nothing submitted to it carries a fence we could observe.
                // Draining it is the only option; it holds only presents, and by now (GPU side done
                // for frames before retirement) those presents' waits have long been satisfiable, so
                // this is normally instantaneous.
                const VkResult result = vkQueueWaitIdle(m_Device->GetPresentQueue());
                if (result != VK_SUCCESS) { throw VulkanError("vkQueueWaitIdle", result); }
                presentDone = true;
            }

            if (!presentDone) {
                ++i;
                continue;
            }

            // Destroy order matters: the swapchain first (it is what the presentation engine holds
            // the semaphore references through), then the semaphores, then the pipeline.
            VulkanSwapchain::DestroyRetired(device, retired.swapchain);

            for (const VkSemaphore semaphore : retired.renderFinished) {
                if (semaphore != VK_NULL_HANDLE) { vkDestroySemaphore(device, semaphore, nullptr); }
            }

            retired.pipeline.reset();
            retired.skinnedPipeline.reset();
            retired.depthImage.reset();

            // Signaled fences were already recycled above. Anything still here is an unsignaled
            // fence left behind by a timed-out forced wait, i.e. the presentation engine is not
            // responding. It may still be pending, so it must not be reset or reused; destroying it
            // is the least bad option on this (shutdown/backstop) path.
            for (const VkFence fence : retired.presentFences) { vkDestroyFence(device, fence, nullptr); }
            retired.presentFences.clear();

            m_Retired.erase(m_Retired.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    VkFence Renderer::AcquirePresentFence() {
        if (!m_FreePresentFences.empty()) {
            const VkFence fence = m_FreePresentFences.back();
            m_FreePresentFences.pop_back();
            return fence;
        }

        constexpr VkFenceCreateInfo fenceInfo{
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,  // Unsignaled: a presentation fence must be unsignaled when attached.
        };

        VkFence fence = VK_NULL_HANDLE;
        const VkResult result = vkCreateFence(m_Device->GetHandle(), &fenceInfo, nullptr, &fence);
        if (result != VK_SUCCESS) { throw VulkanError("vkCreateFence", result); }

        return fence;
    }

    void Renderer::RecycleSignaledPresentFences(std::vector<VkFence>& fences) {
        const VkDevice device = m_Device->GetHandle();

        for (std::size_t i = 0; i < fences.size();) {
            const VkResult status = vkGetFenceStatus(device, fences[i]);

            if (status == VK_NOT_READY) {
                ++i;
                continue;
            }

            if (status != VK_SUCCESS) { throw VulkanError("vkGetFenceStatus", status); }

            const VkResult reset = vkResetFences(device, 1, &fences[i]);
            if (reset != VK_SUCCESS) { throw VulkanError("vkResetFences", reset); }

            m_FreePresentFences.push_back(fences[i]);
            fences.erase(fences.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    void Renderer::DestroyPresentFences() noexcept {
        const VkDevice device = m_Device->GetHandle();

        for (const VkFence fence : m_LivePresentFences) { vkDestroyFence(device, fence, nullptr); }
        for (const VkFence fence : m_FreePresentFences) { vkDestroyFence(device, fence, nullptr); }

        m_LivePresentFences.clear();
        m_FreePresentFences.clear();
    }

    void Renderer::Tick() {
        // This frame's draws. Taken up front so they are consumed however Tick() exits: the
        // application resubmits every frame, and a skipped frame must not let them pile up.
        // Dropping them here is safe; command buffers only ever reference m_FrameDraws entries.
        std::vector<DrawCommand> draws = std::move(m_PendingDraws);
        m_PendingDraws.clear();
        std::vector<Light> lights = std::move(m_PendingLights);
        m_PendingLights.clear();
        std::vector<glm::mat4> joints = std::move(m_PendingJoints);
        m_PendingJoints.clear();

        // A minimized window reports a zero-area framebuffer. There is nothing to present to, and
        // a zero-extent swapchain is invalid, so neither render nor recreate: just wait for the
        // window to come back. The pending resize flag (if any) is left set so the swapchain is
        // rebuilt on the first frame after restoration.
        if (const auto [width, height] = m_Window.GetFramebufferSize(); width == 0 || height == 0) { return; }

        // Rebuild up front on an explicit resize rather than waiting for the presentation engine to
        // notice: not every driver reports VK_ERROR_OUT_OF_DATE_KHR/VK_SUBOPTIMAL_KHR on resize,
        // and doing it here means this very frame already renders at the new size. If the surface
        // turns out to have no area right now there is nothing to render to; skip the frame.
        if (m_Window.ConsumeFramebufferResize() && !RecreateSwapchain()) { return; }

        const VkDevice device = m_Device->GetHandle();
        FrameSync& sync = m_FrameSync[m_CurrentFrame];

        // Checked, not fire-and-forget: this wait is what makes it safe to reset and re-record this
        // slot's command buffer below. If it fails (e.g. VK_ERROR_DEVICE_LOST) the buffer may still
        // be executing, and proceeding would be a genuine GPU race rather than a reportable error.
        VkResult result = vkWaitForFences(device, 1, &sync.inFlight, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) { throw VulkanError("vkWaitForFences", result); }

        // This slot's submission, and by the submission-order rule every earlier one, is complete.
        if (sync.frameNumber > m_CompletedFrame) { m_CompletedFrame = sync.frameNumber; }

        // Now is the best moment to retire deferred resources: m_CompletedFrame was just advanced
        // and nothing new has been submitted yet. Non-blocking.
        ReleaseRetiredResources(false);

        std::uint32_t imageIndex = 0;
        result = vkAcquireNextImageKHR(device, m_Swapchain->GetHandle(), std::numeric_limits<std::uint64_t>::max(),
                                       sync.imageAvailable, VK_NULL_HANDLE, &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // No image was acquired and, per spec, imageAvailable was *not* signaled, so it is safe
            // to reuse next frame; the fence is still signaled because it has not been reset yet,
            // and m_CurrentFrame is left unchanged so this slot is simply retried. Rebuild (if the
            // surface currently allows it) and let the next Tick() render at the new size.
            static_cast<void>(RecreateSwapchain());
            return;
        }

        // VK_SUBOPTIMAL_KHR still delivered an image and signaled the semaphore, so it must be
        // rendered and presented to consume them; the rebuild happens after present instead.
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) { throw VulkanError("vkAcquireNextImageKHR", result); }

        const bool acquiredSuboptimal = result == VK_SUBOPTIMAL_KHR;

        // Only reset once an image is in hand: a fence reset with no matching submit would deadlock
        // the next wait on this slot.
        result = vkResetFences(device, 1, &sync.inFlight);
        if (result != VK_SUCCESS) { throw VulkanError("vkResetFences", result); }

        const VkCommandBuffer commandBuffer = m_CommandBuffers[m_CurrentFrame];
        result = vkResetCommandBuffer(commandBuffer, 0);
        if (result != VK_SUCCESS) { throw VulkanError("vkResetCommandBuffer", result); }

        // Safe to overwrite: the fence wait above proved the last frame that used this slot's
        // buffer has completed. The same goes for the draws recorded into this slot last time:
        // replacing them releases the renderer's references to their meshes/materials.
        m_FrameDraws[m_CurrentFrame] = std::move(draws);

        const VkRect2D sceneRect = GetSceneRect();
        UpdateFrameUniforms(m_CurrentFrame, sceneRect, lights, joints);

        RecordCommandBuffer(commandBuffer, imageIndex, sceneRect);

        // Keyed by the image just acquired, not by m_CurrentFrame: a successful acquire of this
        // image is the only guarantee that the presentation engine is done waiting on it. The
        // count is re-derived from the swapchain on every rebuild; a mismatch here would be a
        // bookkeeping bug, and silently indexing out of range is the worst way to learn about it.
        if (imageIndex >= m_RenderFinished.size()) {
            throw std::runtime_error("Renderer: acquired image index has no render-finished semaphore (swapchain/"
                                     "semaphore bookkeeping out of sync).");
        }

        const VkSemaphore renderFinished = m_RenderFinished[imageIndex];

        // Block only the stage that first touches the swapchain image (the pre-render layout
        // transition + clear in RecordCommandBuffer are scoped to COLOR_ATTACHMENT_OUTPUT for
        // exactly this reason), so vertex work can start while the image is still being presented.
        const VkSemaphoreSubmitInfo waitSemaphoreInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = sync.imageAvailable,
                .value = 0,
                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .deviceIndex = 0,
        };

        // ALL_COMMANDS rather than ALL_GRAPHICS: the signal must not fire until the final barrier's
        // transition to PRESENT_SRC_KHR has completed too, otherwise the presentation engine can
        // observe the image while it is still in COLOR_ATTACHMENT_OPTIMAL.
        const VkSemaphoreSubmitInfo signalSemaphoreInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                .pNext = nullptr,
                .semaphore = renderFinished,
                .value = 0,
                .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .deviceIndex = 0,
        };

        const VkCommandBufferSubmitInfo commandBufferInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = commandBuffer,
                .deviceMask = 0,
        };

        const VkSubmitInfo2 submitInfo{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                .pNext = nullptr,
                .flags = 0,
                .waitSemaphoreInfoCount = 1,
                .pWaitSemaphoreInfos = &waitSemaphoreInfo,
                .commandBufferInfoCount = 1,
                .pCommandBufferInfos = &commandBufferInfo,
                .signalSemaphoreInfoCount = 1,
                .pSignalSemaphoreInfos = &signalSemaphoreInfo,
        };

        result = vkQueueSubmit2(m_Device->GetGraphicsQueue(), 1, &submitInfo, sync.inFlight);
        if (result != VK_SUCCESS) { throw VulkanError("vkQueueSubmit2", result); }

        sync.frameNumber = ++m_SubmittedFrame;

        // With swapchain-maintenance1, attach a fence the presentation engine signals once it has
        // finished with this present's wait semaphore: the exact answer to "may renderFinished be
        // destroyed yet?" that core Vulkan lacks. Tracked per swapchain so it retires with it.
        VkFence presentFence = VK_NULL_HANDLE;
        if (m_UsePresentFences) {
            presentFence = AcquirePresentFence();
            m_LivePresentFences.push_back(presentFence);
        }

        const VkSwapchainKHR swapchain = m_Swapchain->GetHandle();
        const VkSwapchainPresentFenceInfoEXT presentFenceInfo{
                .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
                .pNext = nullptr,
                .swapchainCount = 1,
                .pFences = &presentFence,
        };

        const VkPresentInfoKHR presentInfo{
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .pNext = m_UsePresentFences ? &presentFenceInfo : nullptr,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &renderFinished,
                .swapchainCount = 1,
                .pSwapchains = &swapchain,
                .pImageIndices = &imageIndex,
                .pResults = nullptr,
        };

        result = vkQueuePresentKHR(m_Device->GetPresentQueue(), &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || acquiredSuboptimal) {
            // The surface no longer matches the swapchain (or matches it imperfectly). Both are
            // recoverable: even a rejected present still enqueues its semaphore wait (and signals
            // its presentation fence), so sync state is consistent, and a rebuild makes the next
            // frame correct. A zero-area surface here just defers the rebuild to a later frame.
            static_cast<void>(RecreateSwapchain());
        }
        else if (result != VK_SUCCESS) {
            throw VulkanError("vkQueuePresentKHR", result);
        }

        m_CurrentFrame = (m_CurrentFrame + 1) % kFramesInFlight;
    }

    void Renderer::RecordCommandBuffer(const VkCommandBuffer commandBuffer, const std::uint32_t imageIndex,
                                       const VkRect2D& sceneRect) const {
        constexpr VkCommandBufferBeginInfo beginInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = 0,
                .pInheritanceInfo = nullptr,
        };

        VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
        if (result != VK_SUCCESS) { throw VulkanError("vkBeginCommandBuffer", result); }

        const VkImage image = m_Swapchain->GetImages()[imageIndex];
        const VkImageView imageView = m_Swapchain->GetImageViews()[imageIndex];
        const VkExtent2D extent = m_Swapchain->GetExtent();

        // Acquired image -> color attachment. The src scope is COLOR_ATTACHMENT_OUTPUT with no
        // access so that this transition chains onto Tick()'s imageAvailable semaphore wait (which
        // blocks that same stage); anything broader would let the transition (and the clear that
        // follows it) execute before the presentation engine has released the image. The dst scope
        // is the LOAD_OP_CLEAR/attachment writes performed by the rendering below.
        TransitionImage(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // Depth image -> depth attachment, contents discarded (it is cleared below). The one depth
        // image is shared by every frame in flight, so the src scope is the previous frame's depth
        // writes (fragment tests): a barrier's first scope covers all earlier submissions on the
        // queue, so this orders this frame's clear after them (write-after-write).
        TransitionImage(commandBuffer, m_DepthImage->GetHandle(), VulkanImage::AspectMaskFor(m_DepthFormat),
                        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        const VkClearValue clearColor{.color = {.float32 = {0.02f, 0.02f, 0.05f, 1.0f}}};
        const VkClearValue clearDepth{.depthStencil = {.depth = kDepthClearValue, .stencil = 0}};

        const VkRenderingAttachmentInfo colorAttachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = imageView,
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .resolveImageView = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = clearColor,
        };

        // Depth is only needed within the frame, so it is never stored.
        const VkRenderingAttachmentInfo depthAttachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = m_DepthImage->GetView(),
                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .resolveImageView = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .clearValue = clearDepth,
        };

        const VkRenderingInfo renderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext = nullptr,
                .flags = 0,
                .renderArea = {.offset = {0, 0}, .extent = extent},
                .layerCount = 1,
                .viewMask = 0,
                .colorAttachmentCount = 1,
                .pColorAttachments = &colorAttachment,
                .pDepthAttachment = &depthAttachment,
                .pStencilAttachment = nullptr,
        };

        vkCmdBeginRendering(commandBuffer, &renderingInfo);

        // The scene, into its rectangle (the whole image unless SetSceneViewport restricted it).
        // Skipped entirely for an empty rectangle: zero-sized viewports are invalid.
        if (sceneRect.extent.width > 0 && sceneRect.extent.height > 0) {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline->GetHandle());

            // Flip the viewport vertically (negative height, y at the bottom) so +Y is up in
            // model/world space, matching the winding the pipeline's CCW front-face setting
            // assumes. Vulkan's window-space Y otherwise points down, which would invert
            // front-facing triangles.
            const VkViewport viewport{
                    .x = static_cast<float>(sceneRect.offset.x),
                    .y = static_cast<float>(sceneRect.offset.y) + static_cast<float>(sceneRect.extent.height),
                    .width = static_cast<float>(sceneRect.extent.width),
                    .height = -static_cast<float>(sceneRect.extent.height),
                    .minDepth = 0.0f,
                    .maxDepth = 1.0f,
            };
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &sceneRect);

            // Per-frame constants (set 0) for every draw of this frame; Mesh::Draw supplies the
            // per-draw push constants and the per-sub-mesh material set.
            const VkDescriptorSet frameSet = m_FrameUniforms[m_CurrentFrame].descriptorSet;
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline->GetLayout(), 0, 1,
                                    &frameSet, 0, nullptr);

            // Static and skinned meshes need different pipelines. Their layouts are compatible, so
            // switching keeps set 0 bound; only the pipeline itself is rebound when it changes.
            const Material& fallbackMaterial = *m_MaterialSystem->GetDefaultMaterial();
            const VulkanPipeline* bound = m_Pipeline.get();
            for (const DrawCommand& draw : m_FrameDraws[m_CurrentFrame]) {
                const VulkanPipeline* const wanted = draw.mesh->IsSkinned() ? m_SkinnedPipeline.get() : m_Pipeline.get();
                if (wanted != bound) {
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted->GetHandle());
                    bound = wanted;
                }
                draw.mesh->Draw(commandBuffer, bound->GetLayout(), draw.transform, draw.materialOverrides, fallbackMaterial,
                                draw.firstJoint);
            }
        }

        // Overlay (e.g. the editor UI) last, on top of everything, inside the same rendering scope.
        if (m_Overlay != nullptr) {
            m_Overlay->RecordOverlay(commandBuffer, RenderOverlayTarget{.colorAttachmentFormat = m_Swapchain->GetImageFormat(),
                                                                        .depthAttachmentFormat = m_DepthFormat,
                                                                        .extent = extent});
        }

        vkCmdEndRendering(commandBuffer);

        // Color attachment -> presentable. The src scope makes the attachment writes above visible
        // to the transition. The presentation engine is synchronized by the renderFinished
        // semaphore rather than by this barrier, so the dst scope is ALL_COMMANDS with no access
        // (the synchronization2 spelling of a bottom-of-pipe/no-consumer dependency).
        TransitionImage(commandBuffer, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE);

        result = vkEndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS) { throw VulkanError("vkEndCommandBuffer", result); }
    }

    void Renderer::TransitionImage(const VkCommandBuffer commandBuffer, const VkImage image,
                                   const VkImageAspectFlags aspectMask, const VkImageLayout oldLayout,
                                   const VkImageLayout newLayout, const VkPipelineStageFlags2 srcStageMask,
                                   const VkAccessFlags2 srcAccessMask, const VkPipelineStageFlags2 dstStageMask,
                                   const VkAccessFlags2 dstAccessMask) {
        // Stage/access masks are supplied by the caller rather than fixed at ALL_COMMANDS: for the
        // swapchain image the src scope must line up with the stage the acquire semaphore wait
        // blocks, or the transition is not actually ordered after the presentation engine's reads.
        const VkImageMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = srcStageMask,
                .srcAccessMask = srcAccessMask,
                .dstStageMask = dstStageMask,
                .dstAccessMask = dstAccessMask,
                .oldLayout = oldLayout,
                .newLayout = newLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = {.aspectMask = aspectMask,
                                     .baseMipLevel = 0,
                                     .levelCount = 1,
                                     .baseArrayLayer = 0,
                                     .layerCount = 1},
        };

        const VkDependencyInfo dependencyInfo{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext = nullptr,
                .dependencyFlags = 0,
                .memoryBarrierCount = 0,
                .pMemoryBarriers = nullptr,
                .bufferMemoryBarrierCount = 0,
                .pBufferMemoryBarriers = nullptr,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &barrier,
        };

        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

}  // namespace Frost
