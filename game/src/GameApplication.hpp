#pragma once
#include <Frost/Animation/AnimationSystem.hpp>
#include <Frost/Application.hpp>
#include <Frost/Debug/DebugDraw.hpp>
#include <Frost/Physics/PhysicsSystem.hpp>
#include <Frost/Project/GameLaunchOptions.hpp>
#include <Frost/Renderer/Camera.hpp>
#include <Frost/Scene/Registry.hpp>

#include <memory>
#include <optional>
#include <string>

namespace Frost {
    class Level;
}

// The game: plays one level of a project (see Frost::GameLaunchOptions for which). Every frame,
// the level's input-driven components (FlyControllerComponent, CharacterControllerComponent) poll
// the input and move their objects, its physics runs, and it is drawn as seen through its camera:
// the first object with a CameraComponent (e.g. the player character's).
//
// A level without a camera gets one the player can fly around: an object with a CameraComponent
// and a FlyControllerComponent, added when the level starts, where the editor's view was (when
// started from the editor) or looking at the whole level. Clicking in the window gives the mouse to
// looking around; Escape gives it back.
//
// Started by the editor (Play In Editor), it plays the editor's snapshot of the level being
// edited, and closes on F5 (the key that started it) or Escape.
//
// Physics queries (PhysicsQueryComponent) run every frame after physics. F3 toggles debug drawing
// (on at the start): each query's way, hits and what it touches, drawn over the scene. Queries
// with `debug` set print what they start and stop hitting to the console.
class GameApplication final : public Frost::Application {
public:
    // Throws Frost::ProjectError if options.projectPath is not a project.
    explicit GameApplication(const Frost::GameLaunchOptions& options);
    ~GameApplication() override;

protected:
    // Throws Frost::LevelError if a level given on the command line cannot be loaded (the default
    // startup level failing is not fatal: the game runs an empty level).
    void OnStart() override;
    void OnUpdate(float deltaTime) override;
    // Draws the frame's debug drawing over the scene.
    void OnImGuiRender() override;

private:
    // If the level has no camera, adds the free camera (see above).
    void AddFreeCameraIfNeeded();

    // Prints what the physics queries with `debug` set began and stopped touching this frame.
    void LogPhysicsQueries();

    // Adds every physics query to this frame's debug drawing.
    void DrawPhysicsQueries();

    // An object's name for logs and labels ("#id" if it has none, or is gone).
    [[nodiscard]]
    std::string NameOf(Frost::EntityId entity) const;

    Frost::GameLaunchOptions m_Options;

    // Set by OnStart (an empty level if the startup level could not be loaded).
    std::unique_ptr<Frost::Level> m_Level;

    // Moves the level's simulated rigid bodies every frame.
    Frost::PhysicsSystem m_Physics;
    Frost::AnimationSystem m_Animation;

    // Filled during OnUpdate, drawn (as seen through m_ViewCamera, this frame's camera) and cleared
    // in OnImGuiRender.
    Frost::DebugDraw m_DebugDraw;
    std::optional<Frost::Camera> m_ViewCamera;
    bool m_ShowDebugDraw = true;
};