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
            m_Engine->BeginFrame();

            OnUpdate(m_Engine->GetDeltaTime());
            if (m_Engine->IsImGuiEnabled()) { OnImGuiRender(); }

            m_Engine->EndFrame();
        }

        OnStop();
    }

    void Application::OnStart() {}
    void Application::OnUpdate(float /*deltaTime*/) {}
    void Application::OnImGuiRender() {}
    void Application::OnStop() {}

    Engine& Application::GetEngine() noexcept { return *m_Engine; }

}  // namespace Frost