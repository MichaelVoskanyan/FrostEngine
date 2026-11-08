#include "GameApplication.hpp"

#include <Frost/Engine.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Platform/Input.hpp>
#include <Frost/Platform/Window.hpp>
#include <Frost/Project/Project.hpp>
#include <Frost/Renderer/Renderer.hpp>
#include <Frost/Scene/Hierarchy.hpp>
#include <Frost/Scene/Level.hpp>
#include <Frost/Scene/LevelSerializer.hpp>
#include <Frost/Units.hpp>

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace {

    constexpr const char* kApplicationName = "Cesium";

    // In the project's Levels directory. The game runs as the project in its working directory:
    // the project's content is staged next to the executable by cmake/Assets.cmake (see
    // FROST_PROJECT_DIR). (To be read from the project descriptor once it has settings.)
    constexpr const char* kStartupLevel = "TestLevel.flevel";

    // Where the free camera starts without a view to start from and a level to look at: up and
    // back from the origin, looking slightly down at it.
    constexpr glm::vec3 kDefaultCameraPosition{0.0f, 2.0f * Frost::kMeter, 6.0f * Frost::kMeter};
    constexpr float kDefaultCameraPitch = -15.0f;

    // Looking at the whole level leaves this much room around it.
    constexpr float kFrameMargin = 1.25f;

    // The project to run: the one on the command line, else the working directory.
    [[nodiscard]]
    Frost::Project ResolveProject(const Frost::GameLaunchOptions& options) {
        return options.projectPath.has_value() ? Frost::Project::Open(*options.projectPath) : Frost::Project(".");
    }

    // The object the level is seen through: the first one with a camera.
    [[nodiscard]]
    const Frost::GameObject* FindCamera(const Frost::Level& level) {
        for (const std::unique_ptr<Frost::GameObject>& object : level.GetObjects()) {
            if (object->HasComponent<Frost::CameraComponent>()) { return object.get(); }
        }
        return nullptr;
    }

}  // namespace

GameApplication::GameApplication(const Frost::GameLaunchOptions& options)
    : Frost::Application(Frost::EngineSpecification{.applicationName = kApplicationName,
                                                    .windowWidth = 1280,
                                                    .windowHeight = 720,
                                                    .windowResizable = true,
                                                    .project = ResolveProject(options)}),
      m_Options(options),
      m_Audio(GetEngine().GetAudio()) {}

GameApplication::~GameApplication() = default;

void GameApplication::OnStart() {
    const Frost::Project& project = GetEngine().GetProject();

    if (m_Options.levelPath.has_value()) {
        // Asked for by name (e.g. by the editor): failing to load it is failing to start.
        Frost::LevelLoadResult result = Frost::LevelSerializer::Load(*m_Options.levelPath, GetEngine().GetAssetManager());
        for (const std::string& warning : result.warnings) { std::cerr << "[Game] " << warning << '\n'; }
        m_Level = std::move(result.level);
    }
    else {
        // A missing or broken startup level is not fatal: report it and run an empty level.
        try {
            Frost::LevelLoadResult result = Frost::LevelSerializer::Load(project.GetLevelsDirectory() / kStartupLevel,
                                                                         GetEngine().GetAssetManager());
            for (const std::string& warning : result.warnings) { std::cerr << "[Game] " << warning << '\n'; }
            m_Level = std::move(result.level);
        }
        catch (const Frost::LevelError& e) {
            std::cerr << "[Game] Could not load the startup level; nothing will be drawn. " << e.what() << '\n';
            m_Level = std::make_unique<Frost::Level>();
        }
    }

    AddFreeCameraIfNeeded();

    // Skeletal meshes set to play in the game start their animations, and audio sources set to
    // play on start their sounds.
    m_Animation.Start(m_Level->GetRegistry());
    for (const std::string& problem : m_Audio.Start(m_Level->GetRegistry())) { std::cerr << "[Game] " << problem << '\n'; }
    if (const std::optional<Frost::EntityId> listener = Frost::FindActiveListener(m_Level->GetRegistry())) {
        std::cout << "[Game] Hearing the level from '" << m_Level->FindObject(*listener)->GetName() << "' (its audio listener).\n";
    }

    if (m_Options.playInEditor) {
        GetEngine().GetWindow().SetTitle(std::string(kApplicationName) + " - " + m_Level->GetName() +
                                         " [Play In Editor: F5 or Esc to stop]");
        std::cout << "[Game] Playing '" << m_Level->GetName() << "' from the editor. F5 or Escape stops.\n";
    }
}

void GameApplication::OnUpdate(const float deltaTime) {
    Frost::Engine& engine = GetEngine();
    Frost::Input& input = engine.GetInput();
    Frost::Registry& registry = m_Level->GetRegistry();

    const bool mouseCaptured = input.GetCursorMode() == Frost::CursorMode::Captured;

    // Started from the editor, F5 stops playing here too (the editor notices the game has closed),
    // and so does Escape, once it is not needed to give back the mouse.
    if (m_Options.playInEditor) {
        const bool escapeStops = input.WasKeyPressed(Frost::Key::Escape) && !mouseCaptured;
        if (input.WasKeyPressed(Frost::Key::F5) || escapeStops) {
            engine.GetWindow().SetShouldClose(true);
            return;
        }
    }

    // The mouse is taken for looking around only if something is steered with it: clicking in the
    // window takes it, Escape gives it back. (Which component gets the input when several want it
    // is for input priority to decide, later; for now they all see it.)
    const auto anyOf = [&registry]<typename T>() {
        const Frost::ComponentPool<T>* const pool = registry.FindPool<T>();
        return pool != nullptr && pool->Size() > 0;
    };
    const bool anythingToSteer = anyOf.template operator()<Frost::FlyControllerComponent>() ||
                                 anyOf.template operator()<Frost::CharacterControllerComponent>();
    if (mouseCaptured && (input.WasKeyPressed(Frost::Key::Escape) || !anythingToSteer)) {
        input.SetCursorMode(Frost::CursorMode::Normal);
    }
    else if (!mouseCaptured && anythingToSteer && input.WasMouseButtonPressed(Frost::MouseButton::Left)) {
        input.SetCursorMode(Frost::CursorMode::Captured);
    }

    // Input-driven components poll this frame's input and move their objects: fly controllers
    // directly, characters through their rigid bodies (turning the body, tilting its camera).
    registry.Each<Frost::TransformComponent, Frost::FlyControllerComponent>(
            [&](Frost::EntityId, Frost::TransformComponent& transform, Frost::FlyControllerComponent& controller) {
                controller.Update(input, deltaTime, transform);
            });
    registry.Each<Frost::TransformComponent, Frost::RigidBodyComponent, Frost::CharacterControllerComponent>(
            [&](const Frost::EntityId entity, Frost::TransformComponent& transform, Frost::RigidBodyComponent& body,
                const Frost::CharacterControllerComponent& controller) {
                controller.Update(input, transform, body, registry.TryGet<Frost::CameraComponent>(entity));
            });

    // Keys that play animations (a reload on R, say).
    m_Animation.HandleInput(registry, input);

    // Then physics, so what is drawn is where things are this frame, and animations, so it is
    // posed as it is this frame.
    m_Physics.Update(registry, deltaTime);
    m_Animation.Update(registry, deltaTime);
    m_Level->UpdatePoses();

    // Seen through the level's camera, wherever its object is now (attached to something that moves,
    // it moves along); heard from it too, unless the level has an active audio listener.
    Frost::AudioListener listener;
    Frost::EntityId listenerObject = 0;
    if (const Frost::GameObject* const camera = FindCamera(*m_Level)) {
        const Frost::WorldPlacement placement = Frost::ComputeWorldPlacement(registry, camera->GetId());
        const Frost::Camera view = camera->GetComponent<Frost::CameraComponent>().ToCamera(placement.position, placement.rotation);
        engine.GetRenderer().SetCamera(view);
        listener = Frost::AudioListener{.position = view.GetPosition(), .forward = view.GetForward(), .up = view.GetUp()};
        listenerObject = camera->GetId();
    }
    // The one active listener, wherever its object is now. After everything moved this frame, so
    // sounds come from where their objects are drawn.
    if (const std::optional<Frost::EntityId> ears = Frost::FindActiveListener(registry)) {
        listener = Frost::MakeAudioListener(registry, *ears);
        listenerObject = *ears;
    }
    m_Audio.Update(registry, listener, deltaTime, listenerObject);

    m_Level->Submit(engine.GetRenderer(), engine.GetAssetManager());
}

void GameApplication::AddFreeCameraIfNeeded() {
    if (FindCamera(*m_Level) != nullptr) { return; }

    // Added for this run only (the game never saves levels).
    Frost::GameObject& camera = m_Level->CreateObject("Free Camera");
    const Frost::CameraComponent& lens = camera.AddComponent<Frost::CameraComponent>();
    camera.AddComponent<Frost::FlyControllerComponent>();

    // Yaw then pitch, no roll: the view convention and the transform's Euler order agree.
    Frost::TransformComponent& transform = camera.GetTransform();
    if (m_Options.startView.has_value()) {
        transform.position = m_Options.startView->position;
        transform.rotation = glm::vec3(std::clamp(m_Options.startView->pitchDegrees, -89.0f, 89.0f),
                                       m_Options.startView->yawDegrees, 0.0f);
        return;
    }

    transform.position = kDefaultCameraPosition;
    transform.rotation = glm::vec3(kDefaultCameraPitch, 0.0f, 0.0f);

    // Backed off along the view until a sphere around the level fills the vertical field of view.
    if (const std::optional<Frost::Bounds> bounds = m_Level->GetBounds()) {
        const float radius = std::max(glm::length(bounds->GetSize()) * 0.5f, 1.0f * Frost::kCentimeter);
        const float distance = kFrameMargin * radius / std::sin(glm::radians(lens.verticalFov) * 0.5f);
        transform.position = bounds->GetCenter() - transform.GetForward() * distance;
    }
}