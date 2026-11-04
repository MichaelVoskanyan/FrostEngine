#pragma once
#include <Frost/Application.hpp>

#include <memory>

namespace Frost {
    class Level;
}

class GameApplication final : public Frost::Application {
public:
    GameApplication();
    ~GameApplication() override;

protected:
    void OnStart() override;
    void OnUpdate(float deltaTime) override;

private:
    // Null if the level could not be loaded (the game then runs with an empty scene).
    std::unique_ptr<Frost::Level> m_Level;
};