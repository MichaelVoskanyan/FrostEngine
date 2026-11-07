#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Platform/Input.hpp>
#include <Frost/Scene/Registry.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace Frost {

    struct AnimationAsset;
    class SkeletalMeshComponent;

    // From `time` on (seconds into the trigger's animation), shows or hides one particular object
    // (by id): typically one of the objects attached to the trigger's object, such as the magazine
    // seated in a weapon, the one pulled out of it, or the one brought in.
    struct TriggerVisibilityKey {
        float time = 0.0f;
        EntityId object = 0;
        bool visible = true;

        bool operator==(const TriggerVisibilityKey&) const = default;
    };

    // A key that plays an animation once on the object's skeletal mesh in the game (see
    // SkeletalMeshComponent::PlayOnce): R for a reload, say. While it plays, its visibility keys
    // show and hide the objects they name.
    struct AnimationTrigger {
        Key key = Key::R;

        // The animation, by asset (id + last known path), kept even if it could not be loaded
        // (missing file) so saving the level does not lose it; and the loaded asset, or null.
        AssetReference animationReference;
        std::shared_ptr<const AnimationAsset> animation;

        // In any order. An object the keys name is, while the animation plays, as the last of its
        // keys at or before the current time says; before its first key (and whenever the
        // animation is not playing) as it is set itself (see MeshComponent::IsVisible).
        std::vector<TriggerVisibilityKey> visibilityKeys;

        // What the keys say about `object` at `time`: the latest of its keys at or before `time`
        // (of keys at the same time, the last one listed), or nullopt if it has none by then.
        [[nodiscard]]
        std::optional<bool> EvaluateVisibility(EntityId object, float time) const noexcept;

        // Puts the keys in time order (keeping the order of keys at the same time).
        void SortVisibilityKeys();
    };

    // Keys that play animations on the object's skeletal mesh (see AnimationSystem::HandleInput).
    // A key pressed while a one-shot animation is still playing on the object is ignored, so
    // holding or mashing it does not restart a reload halfway.
    //
    // The objects a trigger shows and hides are its own choice (its visibility keys name them), so
    // several objects of the same kind (three magazines) are told apart. An animation played by a
    // trigger leaves the visibility of the object's attachments to the trigger: the slot-based
    // visibility keys of the animation asset itself (see AnimationClipData::visibilityKeys) apply
    // only when it plays as the skeletal mesh's own animation.
    struct AnimationTriggerComponent {
        static constexpr const char* kDisplayName = "Animation Triggers";

        std::vector<AnimationTrigger> triggers;

        // Run-time (not saved): which trigger last started the skeletal mesh's one-shot animation.
        std::optional<std::size_t> playing;

        // The trigger whose animation is playing once on `mesh` (this object's skeletal mesh) now,
        // or null.
        [[nodiscard]]
        const AnimationTrigger* GetPlaying(const SkeletalMeshComponent* mesh) const noexcept;
    };

}  // namespace Frost
