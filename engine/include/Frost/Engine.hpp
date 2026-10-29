#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace Frost {

    class Window;
    class Renderer;

    struct EngineSpecification {
        std::string applicationName = "Frost Application";
        std::uint32_t windowWidth = 1280;
        std::uint32_t windowHeight = 720;
        bool windowResizable = true;
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

        void Tick();

    private:
        // Declared in dependency order. Destruction occurs in reverse order,
        // so Renderer is destroyed before Window.
        std::unique_ptr<Window> m_Window;
        std::unique_ptr<Renderer> m_Renderer;
    };

}  // namespace Frost
