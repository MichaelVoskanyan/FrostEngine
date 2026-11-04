#include <Frost/Engine.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/ImGui/ImGuiLayer.hpp>
#include <Frost/Platform/Input.hpp>
#include <Frost/Platform/Window.hpp>
#include <Frost/Renderer/Renderer.hpp>

#include <chrono>
#include <memory>
#include <utility>

namespace Frost {

    Engine::Engine(const EngineSpecification& spec) : m_Project(spec.project) {
        WindowSpecification windowSpec{.title = spec.applicationName,
                                       .width = spec.windowWidth,
                                       .height = spec.windowHeight,
                                       .resizable = spec.windowResizable};
        m_Window = std::make_unique<Window>(windowSpec);
        m_Input = std::make_unique<Input>(*m_Window);
        m_Renderer = std::make_unique<Renderer>(*m_Window);
        m_AssetManager = std::make_unique<AssetManager>(*m_Renderer, m_Project.GetContentDirectory());

        // After the window: the ImGui GLFW backend chains onto the callbacks the window installed.
        if (spec.enableImGui) { m_ImGuiLayer = std::make_unique<ImGuiLayer>(*m_Window, *m_Renderer); }
    }

    Engine::~Engine() = default;

    bool Engine::IsRunning() const noexcept { return !m_Window->ShouldClose(); }

    void Engine::BeginFrame() {
        const auto now = std::chrono::steady_clock::now();
        m_DeltaTime = m_LastFrameTime.has_value() ? std::chrono::duration<float>(now - *m_LastFrameTime).count() : 0.0f;
        m_LastFrameTime = now;

        m_Window->PollEvents();
        m_Input->Update();

        if (m_ImGuiLayer != nullptr) { m_ImGuiLayer->BeginFrame(); }
    }

    void Engine::EndFrame() {
        // ImGui's frame is finalized even when the renderer ends up skipping this frame (e.g. the
        // window is minimized): every ImGui::NewFrame() must be matched by a Render().
        if (m_ImGuiLayer != nullptr) { m_ImGuiLayer->EndFrame(); }

        m_Renderer->Tick();
    }

    float Engine::GetDeltaTime() const noexcept { return m_DeltaTime; }

    Window& Engine::GetWindow() noexcept { return *m_Window; }

    Input& Engine::GetInput() noexcept { return *m_Input; }

    Renderer& Engine::GetRenderer() noexcept { return *m_Renderer; }

    AssetManager& Engine::GetAssetManager() noexcept { return *m_AssetManager; }

    const Project& Engine::GetProject() const noexcept { return m_Project; }

    bool Engine::IsImGuiEnabled() const noexcept { return m_ImGuiLayer != nullptr; }

}  // namespace Frost
