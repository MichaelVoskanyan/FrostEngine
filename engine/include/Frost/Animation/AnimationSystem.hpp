#pragma once

#include <Frost/Platform/Input.hpp>

#include <functional>

namespace Frost {

    class Registry;

    // Plays the animations of a level's skeletal meshes (SkeletalMeshComponent): every frame, each
    // playing component's animation time moves on by the frame's time (times its play rate), and
    // the pose follows when the level is next drawn. Keys play one-shot animations (see
    // AnimationTriggerComponent). Run by the game; the editor shows levels paused, at each
    // component's animation time.
    class AnimationSystem final {
    public:
        // Starts playing every skeletal mesh set to play in the game (SkeletalMeshComponent's auto
        // play) from its animation time. Call once, when the level starts.
        void Start(Registry& registry);

        // Plays the animations whose trigger key went down this frame (AnimationTriggerComponent),
        // once each on their object's skeletal mesh, unless it is already playing a one-shot, and
        // records which trigger plays it (AnimationTriggerComponent::playing), whose visibility
        // keys then apply. Call once per frame, before Update.
        void HandleInput(Registry& registry, const Input& input);

        // As above, with `wasPressed` telling whether a key went down this frame.
        void HandleInput(Registry& registry, const std::function<bool(Key)>& wasPressed);

        // Advances every playing skeletal mesh (and one-shot animation) by `deltaTime` seconds. Call
        // once per frame.
        void Update(Registry& registry, float deltaTime);
    };

    inline void AnimationSystem::HandleInput(Registry& registry, const Input& input) {
        HandleInput(registry, [&input](const Key key) { return input.WasKeyPressed(key); });
    }

}  // namespace Frost
