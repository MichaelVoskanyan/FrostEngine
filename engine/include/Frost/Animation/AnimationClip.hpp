#pragma once

#include <Frost/Animation/Skeleton.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Frost {

    // How values between two keyframes are found (glTF's sampler interpolation modes).
    enum class AnimationInterpolation : std::uint8_t {
        Step,         // The earlier key's value until the next key.
        Linear,       // Straight blend (spherical for rotations).
        CubicSpline,  // Cubic Hermite spline through the keys, with a tangent either side of each.
    };

    // One animated property of one bone: keyframe times and the property's value at each.
    //
    // For Step and Linear there is one value per key. For CubicSpline there are three per key, in
    // glTF's order: in-tangent, value, out-tangent (tangents are per second, in the value's units).
    template <typename T>
    struct AnimationChannel {
        AnimationInterpolation interpolation = AnimationInterpolation::Linear;
        std::vector<float> times;  // Seconds, non-decreasing.
        std::vector<T> values;

        [[nodiscard]]
        bool IsEmpty() const noexcept {
            return times.empty();
        }

        // Values per key for this channel's interpolation (1 or 3).
        [[nodiscard]]
        std::size_t GetValuesPerKey() const noexcept {
            return interpolation == AnimationInterpolation::CubicSpline ? 3 : 1;
        }
    };

    using VectorChannel = AnimationChannel<glm::vec3>;
    using RotationChannel = AnimationChannel<glm::quat>;

    // The value of a channel at `time` (clamped to its first/last key), or nullopt if it has no keys.
    // Rotations come out unit length.
    [[nodiscard]]
    std::optional<glm::vec3> SampleChannel(const VectorChannel& channel, float time);

    [[nodiscard]]
    std::optional<glm::quat> SampleChannel(const RotationChannel& channel, float time);

    // Everything an animation does to one bone (found by name in the skeleton it is played on). A
    // channel without keys leaves that property at whatever the pose already has (the rest pose).
    struct BoneTrack {
        std::string bone;
        VectorChannel translation;  // World units (centimeters).
        RotationChannel rotation;
        VectorChannel scale;
    };

    // From `time` on, shows or hides the attachments in a slot of the object playing the animation
    // (see AttachmentComponent), while it plays as that object's own animation. (An animation
    // played by an animation trigger is left alone: the trigger's own keys name the objects it
    // shows and hides; see AnimationTriggerComponent.)
    struct VisibilityKey {
        float time = 0.0f;  // Seconds.
        std::string slot;
        bool visible = true;

        auto operator<=>(const VisibilityKey&) const = default;
    };

    // An animation ("track", "sequence"): how the bones of a skeleton move over time, imported from
    // glTF animations, one clip per glTF animation; and, authored in the editor, when the
    // attachments of the object playing it are shown or hidden.
    struct AnimationClipData {
        // Seconds: the time of the last key of any channel (a clip whose keys all sit at 0 is a
        // single pose, with duration 0).
        float duration = 0.0f;
        std::vector<BoneTrack> tracks;

        // In time order (keys at the same time in the order they were added). Not part of the source
        // file: a reimport keeps them.
        std::vector<VisibilityKey> visibilityKeys;

        // The bone each track moves in `skeleton` (by name), or -1 for a track whose bone the
        // skeleton does not have. Bind once per skeleton, then Sample as often as needed.
        [[nodiscard]]
        std::vector<std::int32_t> BindTracks(const SkeletonData& skeleton) const;

        // Overwrites the properties this clip animates in `pose` (local transforms by bone index,
        // typically starting out as the rest pose) with their values at `time` (clamped to the
        // clip). `bindings` comes from BindTracks for the skeleton `pose` belongs to.
        void Sample(float time, std::span<const std::int32_t> bindings, std::span<BoneTransform> pose) const;

        // Whether the attachments in `slot` are shown at `time` (clamped to the clip): as the last
        // visibility key for the slot at or before that time says, or nullopt before the first (the
        // clip has no say then).
        [[nodiscard]]
        std::optional<bool> EvaluateVisibility(std::string_view slot, float time) const;

        // The slots the visibility keys name, each once, in order of first appearance.
        [[nodiscard]]
        std::vector<std::string> GetVisibilitySlots() const;

        // Puts the visibility keys in time order (keeping the order of keys at the same time).
        void SortVisibilityKeys();

        // Why this is not a usable clip (a channel whose times are not finite and non-decreasing,
        // whose value count does not match its key count, or with non-finite values; a track
        // without a bone name; a visibility key out of order, at a non-finite or negative time, or
        // without a slot), or nullopt if it is one.
        [[nodiscard]]
        std::optional<std::string> Validate() const;
    };

}  // namespace Frost
