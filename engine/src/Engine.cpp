#include <Frost/Engine.hpp>

#include <Frost/Platform/Window.hpp>
#include <Frost/Renderer/Vulkan/VulkanContext.hpp>

#include <memory>
#include <utility>

namespace Frost {

    Engine::Engine(const EngineSpecification& spec) {
        WindowSpecification windowSpec{.title = spec.applicationName,
                                       .width = spec.windowWidth,
                                       .height = spec.windowHeight,
                                       .resizable = spec.windowResizable};
        m_Window = std::make_unique<Window>(windowSpec);
        m_VulkanContext = std::make_unique<VulkanContext>(*m_Window);
    }

    Engine::~Engine() = default;

    bool Engine::IsRunning() const noexcept { return !m_Window->ShouldClose(); }

    void Engine::Tick() { m_Window->PollEvents(); }

}  // namespace Frost