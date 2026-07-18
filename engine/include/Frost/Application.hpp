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
        virtual void OnUpdate();
        virtual void OnStop();

    private:
        std::unique_ptr<Engine> m_Engine;
    };

}  // namespace Frost