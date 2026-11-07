#include <Frost/Scene/Components/AnimationTriggerComponent.hpp>

#include <Frost/Scene/Components/SkeletalMeshComponent.hpp>

#include <algorithm>

namespace Frost {

    std::optional<bool> AnimationTrigger::EvaluateVisibility(const EntityId object, const float time) const noexcept {
        const TriggerVisibilityKey* latest = nullptr;
        for (const TriggerVisibilityKey& candidate : visibilityKeys) {
            if (candidate.object == object && candidate.time <= time && (latest == nullptr || candidate.time >= latest->time)) {
                latest = &candidate;
            }
        }
        if (latest == nullptr) { return std::nullopt; }
        return latest->visible;
    }

    void AnimationTrigger::SortVisibilityKeys() {
        std::stable_sort(visibilityKeys.begin(), visibilityKeys.end(),
                         [](const TriggerVisibilityKey& a, const TriggerVisibilityKey& b) { return a.time < b.time; });
    }

    const AnimationTrigger* AnimationTriggerComponent::GetPlaying(const SkeletalMeshComponent* const mesh) const noexcept {
        if (!playing.has_value() || *playing >= triggers.size() || mesh == nullptr || !mesh->IsPlayingOnce()) { return nullptr; }

        const AnimationTrigger& trigger = triggers[*playing];
        return trigger.animation != nullptr && mesh->GetOneShotAnimation() == trigger.animation ? &trigger : nullptr;
    }

}  // namespace Frost
