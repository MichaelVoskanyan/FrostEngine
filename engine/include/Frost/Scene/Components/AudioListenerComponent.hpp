#pragma once

#include <Frost/Audio/AudioEngine.hpp>
#include <Frost/Scene/Registry.hpp>

#include <glm/vec3.hpp>

#include <optional>

namespace Frost {

    // Where the game hears its sounds from: the ears of the level. The listener is mounted on its
    // object: at `offset` from it (e.g. a character's head height, like its camera's offset), and
    // faces the way the object faces (its forward direction, up its up; see
    // TransformComponent::GetForward), following it as it moves and turns, also when it is attached
    // to another object. The object's scale does not matter (the offset turns with the object but is
    // not scaled by it). Put it on the player (or on the object carrying the camera) to hear the
    // level from there.
    //
    // The game hears through one listener at a time: the active one (see FindActiveListener and
    // SetActiveListener). A level keeps at most one listener active: loading a level with several
    // keeps the first active (with a warning), and the editor turns the others off when one is made
    // active. A level without an active listener is heard from its camera. The editor always hears
    // (previews sounds) from its viewport camera.
    struct AudioListenerComponent {
        static constexpr const char* kDisplayName = "Audio Listener";

        // Whether the game hears through this listener (saved with the level).
        bool active = true;

        // Where it sits relative to its object, in the object's space (centimeters).
        glm::vec3 offset{0.0f};
    };

    // The object whose listener is active. Should several be (a level as loaded never has more than
    // one, see above; gameplay code switches with SetActiveListener), the one with the lowest id.
    // Nullopt if no listener is active.
    [[nodiscard]]
    std::optional<EntityId> FindActiveListener(const Registry& registry);

    // Makes the listener of `object` the active one, and every other listener inactive. Does nothing
    // if the object has no listener.
    void SetActiveListener(Registry& registry, EntityId object);

    // The listener at `object` as the audio engine takes it: where its mount is in the world (the
    // object's place plus its offset, turned with the object) and which way the object faces (see
    // ComputeWorldPlacement). Without a listener component, at the object itself.
    [[nodiscard]]
    AudioListener MakeAudioListener(const Registry& registry, EntityId object);

}  // namespace Frost
