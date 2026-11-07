#include <Frost/Animation/AnimationSystem.hpp>

#include <Frost/Scene/Components/AnimationTriggerComponent.hpp>
#include <Frost/Scene/Components/SkeletalMeshComponent.hpp>
#include <Frost/Scene/Registry.hpp>

namespace Frost {

    void AnimationSystem::Start(Registry& registry) {
        registry.Each<SkeletalMeshComponent>([](EntityId, SkeletalMeshComponent& mesh) {
            if (mesh.GetAutoPlay() && mesh.GetAnimation() != nullptr) { mesh.Play(); }
        });
    }

    void AnimationSystem::HandleInput(Registry& registry, const std::function<bool(Key)>& wasPressed) {
        registry.Each<AnimationTriggerComponent>([&](const EntityId object, AnimationTriggerComponent& component) {
            SkeletalMeshComponent* const mesh = registry.TryGet<SkeletalMeshComponent>(object);
            if (mesh == nullptr || mesh->IsPlayingOnce()) { return; }

            for (std::size_t i = 0; i < component.triggers.size(); ++i) {
                const AnimationTrigger& trigger = component.triggers[i];
                if (trigger.animation != nullptr && wasPressed(trigger.key)) {
                    mesh->PlayOnce(trigger.animation);
                    component.playing = i;
                    break;  // One at a time.
                }
            }
        });
    }

    void AnimationSystem::Update(Registry& registry, const float deltaTime) {
        registry.Each<SkeletalMeshComponent>([deltaTime](EntityId, SkeletalMeshComponent& mesh) { mesh.Advance(deltaTime); });

        // A trigger's animation that has ended is no longer playing.
        registry.Each<AnimationTriggerComponent>([&registry](const EntityId object, AnimationTriggerComponent& component) {
            if (component.playing.has_value() && component.GetPlaying(registry.TryGet<SkeletalMeshComponent>(object)) == nullptr) {
                component.playing.reset();
            }
        });
    }

}  // namespace Frost
