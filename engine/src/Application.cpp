#include <Frost/Application.hpp>

#include <memory>

namespace Frost {

    Application::Application() : Application(EngineSpecification{}) {}

    Application::Application(const EngineSpecification& specification)
        : m_Engine(std::make_unique<Engine>(specification)) {}

    Application::~Application() = default;

    void Application::Run() {
        OnStart();

        while (m_Engine->IsRunning()) {
            m_Engine->Tick();
            OnUpdate();
        }

        OnStop();
    }

    void Application::OnStart() {}
    void Application::OnUpdate() {}
    void Application::OnStop() {}

}  // namespace Frost