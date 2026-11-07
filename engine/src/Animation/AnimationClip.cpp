#include <Frost/Animation/AnimationClip.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

namespace {

    using Frost::AnimationChannel;
    using Frost::AnimationInterpolation;

    // Where `time` falls among a channel's keys: between key `index` and `index + 1`, a fraction
    // `t` (0..1) of the way. With `t` = 0 and index = last key when past the end (or at a single
    // key), so callers read one key only.
    struct KeySpan {
        std::size_t index = 0;
        float t = 0.0f;
        float duration = 0.0f;  // Seconds between the two keys (0 if there is only the one).
    };

    [[nodiscard]]
    KeySpan FindKeys(const std::vector<float>& times, const float time) {
        const std::size_t last = times.size() - 1;
        if (!(time > times.front())) { return KeySpan{.index = 0, .t = 0.0f, .duration = 0.0f}; }  // Also NaN.
        if (time >= times.back()) { return KeySpan{.index = last, .t = 0.0f, .duration = 0.0f}; }

        // The first key after `time`; the one before it starts the span.
        const auto next = std::upper_bound(times.begin(), times.end(), time);
        const std::size_t index = static_cast<std::size_t>(next - times.begin()) - 1;
        const float duration = times[index + 1] - times[index];
        const float t = duration > 0.0f ? (time - times[index]) / duration : 0.0f;
        return KeySpan{.index = index, .t = std::clamp(t, 0.0f, 1.0f), .duration = duration};
    }

    // The key's value (for cubic splines, the middle of its three).
    template <typename T>
    [[nodiscard]]
    const T& KeyValue(const AnimationChannel<T>& channel, const std::size_t key) {
        return channel.values[key * channel.GetValuesPerKey() + (channel.GetValuesPerKey() == 3 ? 1 : 0)];
    }

    // glTF's cubic Hermite spline (Appendix C of the specification): value_k and value_k+1 with
    // the out-tangent of k and the in-tangent of k+1, the tangents scaled by the span's duration.
    template <typename T>
    [[nodiscard]]
    T CubicSpline(const AnimationChannel<T>& channel, const KeySpan& span) {
        const std::size_t base = span.index * 3;
        const T& value0 = channel.values[base + 1];
        const T& outTangent0 = channel.values[base + 2];
        const T& inTangent1 = channel.values[base + 3];
        const T& value1 = channel.values[base + 4];

        const float t = span.t;
        const float t2 = t * t;
        const float t3 = t2 * t;
        return value0 * (2.0f * t3 - 3.0f * t2 + 1.0f) + outTangent0 * (span.duration * (t3 - 2.0f * t2 + t)) +
               value1 * (-2.0f * t3 + 3.0f * t2) + inTangent1 * (span.duration * (t3 - t2));
    }

    [[nodiscard]]
    glm::quat Normalized(const glm::quat& q) {
        const float length = glm::length(q);
        return length > 0.0f && std::isfinite(length) ? q / length : Frost::IdentityQuaternion();
    }

    [[nodiscard]]
    bool IsFinite(const glm::vec3& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    [[nodiscard]]
    bool IsFinite(const glm::quat& q) {
        return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
    }

    template <typename T>
    [[nodiscard]]
    std::optional<std::string> ValidateChannel(const AnimationChannel<T>& channel) {
        if (channel.interpolation > AnimationInterpolation::CubicSpline) { return "has an unknown interpolation"; }
        if (channel.values.size() != channel.times.size() * channel.GetValuesPerKey()) {
            return "has a different number of values than its keys need";
        }
        for (std::size_t i = 0; i < channel.times.size(); ++i) {
            if (!std::isfinite(channel.times[i]) || (i > 0 && channel.times[i] < channel.times[i - 1])) {
                return "has key times that are not finite and in order";
            }
        }
        for (const T& value : channel.values) {
            if (!IsFinite(value)) { return "has non-finite values"; }
        }
        return std::nullopt;
    }

}  // namespace

namespace Frost {

    std::optional<glm::vec3> SampleChannel(const VectorChannel& channel, const float time) {
        if (channel.IsEmpty()) { return std::nullopt; }

        const KeySpan span = FindKeys(channel.times, time);
        if (span.t == 0.0f || span.index + 1 >= channel.times.size()) { return KeyValue(channel, span.index); }

        switch (channel.interpolation) {
            case AnimationInterpolation::Step: return KeyValue(channel, span.index);
            case AnimationInterpolation::Linear:
                return glm::mix(KeyValue(channel, span.index), KeyValue(channel, span.index + 1), span.t);
            case AnimationInterpolation::CubicSpline: return CubicSpline(channel, span);
        }
        return KeyValue(channel, span.index);
    }

    std::optional<glm::quat> SampleChannel(const RotationChannel& channel, const float time) {
        if (channel.IsEmpty()) { return std::nullopt; }

        const KeySpan span = FindKeys(channel.times, time);
        if (span.t == 0.0f || span.index + 1 >= channel.times.size()) { return Normalized(KeyValue(channel, span.index)); }

        switch (channel.interpolation) {
            case AnimationInterpolation::Step: return Normalized(KeyValue(channel, span.index));
            case AnimationInterpolation::Linear:
                // Spherical, along the shorter arc (glm::slerp flips the second key if needed).
                return Normalized(glm::slerp(Normalized(KeyValue(channel, span.index)),
                                             Normalized(KeyValue(channel, span.index + 1)), span.t));
            case AnimationInterpolation::CubicSpline: return Normalized(CubicSpline(channel, span));
        }
        return Normalized(KeyValue(channel, span.index));
    }

    std::vector<std::int32_t> AnimationClipData::BindTracks(const SkeletonData& skeleton) const {
        std::vector<std::int32_t> bindings;
        bindings.reserve(tracks.size());
        for (const BoneTrack& track : tracks) {
            const std::optional<std::size_t> bone = skeleton.FindBone(track.bone);
            bindings.push_back(bone.has_value() ? static_cast<std::int32_t>(*bone) : -1);
        }
        return bindings;
    }

    void AnimationClipData::Sample(const float time, const std::span<const std::int32_t> bindings,
                                   const std::span<BoneTransform> pose) const {
        const float clamped = std::clamp(std::isfinite(time) ? time : 0.0f, 0.0f, std::max(duration, 0.0f));

        for (std::size_t i = 0; i < tracks.size() && i < bindings.size(); ++i) {
            const std::int32_t bone = bindings[i];
            if (bone < 0 || static_cast<std::size_t>(bone) >= pose.size()) { continue; }

            const BoneTrack& track = tracks[i];
            BoneTransform& target = pose[static_cast<std::size_t>(bone)];
            if (const std::optional<glm::vec3> translation = SampleChannel(track.translation, clamped)) {
                target.translation = *translation;
            }
            if (const std::optional<glm::quat> rotation = SampleChannel(track.rotation, clamped)) {
                target.rotation = *rotation;
            }
            if (const std::optional<glm::vec3> scale = SampleChannel(track.scale, clamped)) { target.scale = *scale; }
        }
    }

    std::optional<bool> AnimationClipData::EvaluateVisibility(const std::string_view slot, const float time) const {
        const float clamped = std::clamp(std::isfinite(time) ? time : 0.0f, 0.0f, std::max(duration, 0.0f));

        std::optional<bool> visible;
        for (const VisibilityKey& key : visibilityKeys) {
            if (key.time > clamped) { break; }  // In time order: the rest are later.
            if (key.slot == slot) { visible = key.visible; }
        }
        return visible;
    }

    std::vector<std::string> AnimationClipData::GetVisibilitySlots() const {
        std::vector<std::string> slots;
        for (const VisibilityKey& key : visibilityKeys) {
            if (std::find(slots.begin(), slots.end(), key.slot) == slots.end()) { slots.push_back(key.slot); }
        }
        return slots;
    }

    void AnimationClipData::SortVisibilityKeys() {
        std::stable_sort(visibilityKeys.begin(), visibilityKeys.end(),
                         [](const VisibilityKey& a, const VisibilityKey& b) { return a.time < b.time; });
    }

    std::optional<std::string> AnimationClipData::Validate() const {
        if (!std::isfinite(duration) || duration < 0.0f) { return "its duration is not a finite, non-negative time"; }

        for (std::size_t i = 0; i < visibilityKeys.size(); ++i) {
            const VisibilityKey& key = visibilityKeys[i];
            if (!std::isfinite(key.time) || key.time < 0.0f || (i > 0 && key.time < visibilityKeys[i - 1].time)) {
                return "its visibility keys are not at finite, non-negative times in order";
            }
            if (key.slot.empty()) { return "a visibility key names no slot"; }
        }

        for (const BoneTrack& track : tracks) {
            if (track.bone.empty()) { return "a track has no bone name"; }

            const std::string where = "the track of bone '" + track.bone + "'";
            if (const std::optional<std::string> problem = ValidateChannel(track.translation)) {
                return where + " (translation) " + *problem;
            }
            if (const std::optional<std::string> problem = ValidateChannel(track.rotation)) {
                return where + " (rotation) " + *problem;
            }
            if (const std::optional<std::string> problem = ValidateChannel(track.scale)) {
                return where + " (scale) " + *problem;
            }
        }
        return std::nullopt;
    }

}  // namespace Frost
