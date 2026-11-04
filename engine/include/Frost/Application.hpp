#pragma once

#include <Frost/Engine.hpp>

#include <memory>

namespace Frost {

    class Application {
    public:
        Application();

        explicit Application(const EngineSpecification& specification);

        virtual ~Application();

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        Application(Application&&) = delete;
        Application& operator=(Application&&) = delete;

        void Run();

    protected:
        virtual void OnStart();

        // Once per frame, after input has been sampled and before the frame is rendered.
        // deltaTime is the time since the previous frame, in seconds (0 on the first frame).
        virtual void OnUpdate(float deltaTime);

        // Once per frame, after OnUpdate(), and only if the engine was created with ImGui enabled.
        // The place to issue ImGui:: calls.
        virtual void OnImGuiRender();

        virtual void OnStop();

        [[nodiscard]]
        Engine& GetEngine() noexcept;

    private:
        std::unique_ptr<Engine> m_Engine;
    };

}  // namespace Frost