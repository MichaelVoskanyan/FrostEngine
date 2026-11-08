#include <Frost/Scene/Components/AudioListenerComponent.hpp>

#include <Frost/Scene/Hierarchy.hpp>

#include <glm/gtc/quaternion.hpp>

namespace Frost {

    std::optional<EntityId> FindActiveListener(const Registry& registry) {
        std::optional<EntityId> found;
        registry.Each<AudioListenerComponent>([&](const EntityId object, const AudioListenerComponent& listener) {
            if (listener.active && (!found.has_value() || object < *found)) { found = object; }
        });
        return found;
    }

    void SetActiveListener(Registry& registry, const EntityId object) {
        if (!registry.Has<AudioListenerComponent>(object)) { return; }
        registry.Each<AudioListenerComponent>(
                [object](const EntityId other, AudioListenerComponent& listener) { listener.active = other == object; });
    }

    AudioListener MakeAudioListener(const Registry& registry, const EntityId object) {
        // Facing as objects face: forward is -Z in their own space, up is +Y (see
        // TransformComponent::GetForward).
        const WorldPlacement placement = ComputeWorldPlacement(registry, object);
        const AudioListenerComponent* const listener = registry.TryGet<AudioListenerComponent>(object);
        const glm::vec3 offset = listener != nullptr ? listener->offset : glm::vec3(0.0f);
        return AudioListener{.position = placement.position + placement.rotation * offset,
                             .forward = placement.rotation * glm::vec3(0.0f, 0.0f, -1.0f),
                             .up = placement.rotation * glm::vec3(0.0f, 1.0f, 0.0f)};
    }

}  // namespace Frost
