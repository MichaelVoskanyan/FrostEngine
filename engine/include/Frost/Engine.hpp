#pragma once

#include <Frost/Audio/AudioEngine.hpp>
#include <Frost/Project/Project.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Frost {

    class Window;
    class Input;
    class Renderer;
    class AssetManager;
    class ImGuiLayer;

    struct EngineSpecification {
        std::string applicationName = "Frost Application";
        std::uint32_t windowWidth = 1280;
        std::uint32_t windowHeight = 720;
        bool windowResizable = true;
        // Creates an ImGuiLayer (Dear ImGui context + GLFW/Vulkan backends) drawn on top of the
        // scene. Off by default: the editor wants it, a shipping game does not.
        bool enableImGui = false;
        // The project the engine runs: its content directory is scanned so assets can be found by
        // id (see AssetRegistry), and is the only place assets are loaded from. By default the
        // working directory, which is how a game runs (its project's content staged beside it).
        Project project{"."};
        // Where sound goes: the default device (or, without one, nowhere, silently), or explicitly
        // nowhere (see AudioOutput).
        AudioOutput audioOutput = AudioOutput::Default;
    };

    class Engine final {
    public:
        explicit Engine(const EngineSpecification& desc);
        ~Engine();

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        Engine(Engine&&) = delete;
        Engine& operator=(Engine&&) = delete;

        [[nodiscard]]
        bool IsRunning() const noexcept;

        // One frame is BeginFrame(), then application update/UI code, then EndFrame().
        // BeginFrame polls window events, snapshots input, advances the frame timer and (if
        // enabled) starts the ImGui frame. EndFrame finalizes the ImGui frame and renders.
        void BeginFrame();
        void EndFrame();

        // Seconds between the two most recent BeginFrame() calls; 0 on the first frame.
        [[nodiscard]]
        float GetDeltaTime() const noexcept;

        [[nodiscard]]
        Window& GetWindow() noexcept;

        [[nodiscard]]
        Input& GetInput() noexcept;

        [[nodiscard]]
        Renderer& GetRenderer() noexcept;

        [[nodiscard]]
        AssetManager& GetAssetManager() noexcept;

        [[nodiscard]]
        AudioEngine& GetAudio() noexcept;

        [[nodiscard]]
        const Project& GetProject() const noexcept;

        [[nodiscard]]
        bool IsImGuiEnabled() const noexcept;

    private:
        Project m_Project;

        // Declared in dependency order. Destruction occurs in reverse order: the ImGui layer
        // (which renders through the renderer and chains onto the window's callbacks) goes first,
        // then the assets (GPU resources created through the renderer), then the audio device (the
        // voices playing sound assets belong to the application's systems, gone by then), then the
        // renderer, then input, then the window they all reference.
        std::unique_ptr<Window> m_Window;
        std::unique_ptr<Input> m_Input;
        std::unique_ptr<Renderer> m_Renderer;
        std::unique_ptr<AudioEngine> m_Audio;
        std::unique_ptr<AssetManager> m_AssetManager;
        std::unique_ptr<ImGuiLayer> m_ImGuiLayer;  // Null unless EngineSpecification::enableImGui.

        std::optional<std::chrono::steady_clock::time_point> m_LastFrameTime;  // Unset before the first frame.
        float m_DeltaTime = 0.0f;
    };

}  // namespace Frost
