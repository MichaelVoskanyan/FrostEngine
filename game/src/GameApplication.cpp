#include "GameApplication.hpp"

#include <Frost/Engine.hpp>
#include <Frost/Scene/Level.hpp>
#include <Frost/Scene/LevelSerializer.hpp>

#include <iostream>
#include <utility>
#include <string>

namespace {

    // In the project's Levels directory. The game runs as the project in its working directory:
    // the project's content is staged next to the executable by cmake/Assets.cmake (see
    // FROST_PROJECT_DIR). (To be read from the project descriptor once it has settings.)
    constexpr const char* kStartupLevel = "TestLevel.flevel";

}  // namespace

GameApplication::GameApplication()
    : Frost::Application(Frost::EngineSpecification{.applicationName = "Cesium",
                                                    .windowWidth = 1280,
                                                    .windowHeight = 720,
                                                    .windowResizable = true}) {}

GameApplication::~GameApplication() = default;

void GameApplication::OnStart() {
    // A missing or broken level is not fatal: report it and run with nothing to draw.
    try {
        Frost::LevelLoadResult result = Frost::LevelSerializer::Load(GetEngine().GetProject().GetLevelsDirectory() / kStartupLevel,
                                                                     GetEngine().GetAssetManager());
        for (const std::string& warning : result.warnings) { std::cerr << "[Game] " << warning << '\n'; }
        m_Level = std::move(result.level);
    }
    catch (const Frost::LevelError& e) {
        std::cerr << "[Game] Could not load the startup level; nothing will be drawn. " << e.what() << '\n';
    }
}

void GameApplication::OnUpdate(float /*deltaTime*/) {
    if (m_Level != nullptr) { m_Level->Submit(GetEngine().GetRenderer(), GetEngine().GetAssetManager()); }
}