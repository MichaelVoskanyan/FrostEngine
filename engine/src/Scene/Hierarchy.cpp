#include <Frost/Scene/Hierarchy.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Scene/Components/AnimationTriggerComponent.hpp>
#include <Frost/Scene/Components/AttachmentComponent.hpp>
#include <Frost/Scene/Components/MeshComponent.hpp>
#include <Frost/Scene/Components/SkeletalMeshComponent.hpp>
#include <Frost/Scene/Components/TransformComponent.hpp>

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#include <algorithm>
#include <optional>
#include <span>

namespace Frost {

    glm::mat4 ComputeBoneMotion(const Registry& registry, const EntityId object, const std::string_view bone) {
        if (bone.empty()) { return glm::mat4(1.0f); }

        const SkeletalMeshComponent* const mesh = registry.TryGet<SkeletalMeshComponent>(object);
        if (mesh == nullptr || mesh->GetSkeletalMesh() == nullptr || mesh->GetSkeletalMesh()->skeleton == nullptr) {
            return glm::mat4(1.0f);
        }

        const SkeletonAsset& skeleton = *mesh->GetSkeletalMesh()->skeleton;
        const std::optional<std::size_t> index = skeleton.skeleton.FindBone(bone);
        const std::vector<glm::mat4>& pose = mesh->GetBoneTransforms();
        if (!index.has_value() || *index >= pose.size() || *index >= skeleton.restTransforms.size()) { return glm::mat4(1.0f); }

        return pose[*index] * glm::inverse(skeleton.restTransforms[*index]);
    }

    glm::mat4 ComputeMountMatrix(const Registry& registry, const AttachmentComponent& attachment) {
        if (!attachment.socket.empty()) {
            if (const SkeletalMeshComponent* const mesh = registry.TryGet<SkeletalMeshComponent>(attachment.parent)) {
                if (const std::optional<glm::mat4> socket = mesh->GetSocketTransform(attachment.socket)) { return *socket; }
            }
        }
        return ComputeBoneMotion(registry, attachment.parent, attachment.bone);
    }

    glm::mat4 ComputeWorldMatrix(const Registry& registry, EntityId object) {
        glm::mat4 world(1.0f);
        for (std::size_t depth = 0; depth < kMaxAttachmentDepth; ++depth) {
            const TransformComponent* const transform = registry.TryGet<TransformComponent>(object);
            if (transform == nullptr) { break; }
            world = transform->ToMatrix() * world;

            const AttachmentComponent* const attachment = registry.TryGet<AttachmentComponent>(object);
            if (attachment == nullptr || attachment->parent == 0 || attachment->parent == object ||
                !registry.Has<TransformComponent>(attachment->parent)) {
                break;
            }
            world = ComputeMountMatrix(registry, *attachment) * world;
            object = attachment->parent;
        }
        return world;
    }

    WorldPlacement ComputeWorldPlacement(const Registry& registry, const EntityId object) {
        const TransformComponent* const transform = registry.TryGet<TransformComponent>(object);
        if (transform == nullptr) { return {}; }
        if (!IsAttached(registry, object)) { return {.position = transform->position, .rotation = transform->GetRotation()}; }

        // The rotation is what is left of the upper 3x3 with the scale taken out of each axis.
        const glm::mat4 world = ComputeWorldMatrix(registry, object);
        glm::mat3 axes(world);
        for (glm::length_t i = 0; i < 3; ++i) {
            const float length = glm::length(axes[i]);
            if (!(length > 0.0f)) { return {.position = glm::vec3(world[3]), .rotation = transform->GetRotation()}; }
            axes[i] /= length;
        }
        if (glm::determinant(axes) < 0.0f) { axes[0] = -axes[0]; }  // A mirroring scale: keep a rotation.
        return {.position = glm::vec3(world[3]), .rotation = glm::normalize(glm::quat_cast(axes))};
    }

    bool IsAttached(const Registry& registry, const EntityId object) {
        const AttachmentComponent* const attachment = registry.TryGet<AttachmentComponent>(object);
        return attachment != nullptr && attachment->parent != 0 && attachment->parent != object &&
               registry.Has<TransformComponent>(attachment->parent);
    }

    bool IsShownInHierarchy(const Registry& registry, EntityId object) {
        for (std::size_t depth = 0; depth < kMaxAttachmentDepth; ++depth) {
            if (const MeshComponent* const mesh = registry.TryGet<MeshComponent>(object); mesh != nullptr && !mesh->IsShown()) {
                return false;
            }
            if (const SkeletalMeshComponent* const mesh = registry.TryGet<SkeletalMeshComponent>(object);
                mesh != nullptr && !mesh->IsShown()) {
                return false;
            }
            if (!IsAttached(registry, object)) { break; }
            object = registry.Get<AttachmentComponent>(object).parent;
        }
        return true;
    }

    void UpdateAttachmentVisibility(Registry& registry) {
        // What the animation of the object it is attached to says about an object (every model's,
        // so one that was detached meanwhile loses what an animation said before).
        const auto animationSays = [&registry](const EntityId object) -> std::optional<bool> {
            const AttachmentComponent* const attachment = registry.TryGet<AttachmentComponent>(object);
            if (attachment == nullptr || attachment->slot.empty() || !IsAttached(registry, object)) { return std::nullopt; }

            const SkeletalMeshComponent* const parent = registry.TryGet<SkeletalMeshComponent>(attachment->parent);
            const AnimationAsset* const animation = parent != nullptr ? parent->GetActiveAnimation() : nullptr;
            if (animation == nullptr) { return std::nullopt; }

            // Played by a trigger: the trigger's keys decide (below), not the animation's slots.
            if (const AnimationTriggerComponent* const triggers = registry.TryGet<AnimationTriggerComponent>(attachment->parent);
                triggers != nullptr && triggers->GetPlaying(parent) != nullptr) {
                return std::nullopt;
            }
            return animation->clip.EvaluateVisibility(attachment->slot, parent->GetActiveAnimationTime());
        };

        registry.Each<MeshComponent>([&](const EntityId object, MeshComponent& mesh) { mesh.SetAnimationVisibility(animationSays(object)); });
        registry.Each<SkeletalMeshComponent>(
                [&](const EntityId object, SkeletalMeshComponent& mesh) { mesh.SetAnimationVisibility(animationSays(object)); });

        // The objects named by the keys of the triggers playing now.
        registry.Each<AnimationTriggerComponent>([&](const EntityId owner, const AnimationTriggerComponent& component) {
            const SkeletalMeshComponent* const mesh = registry.TryGet<SkeletalMeshComponent>(owner);
            const AnimationTrigger* const trigger = component.GetPlaying(mesh);
            if (trigger == nullptr) { return; }

            for (const TriggerVisibilityKey& key : trigger->visibilityKeys) {
                const std::optional<bool> visible = trigger->EvaluateVisibility(key.object, mesh->GetOneShotTime());
                if (!visible.has_value()) { continue; }  // Not yet: as it is set.
                if (MeshComponent* const target = registry.TryGet<MeshComponent>(key.object)) { target->SetAnimationVisibility(visible); }
                if (SkeletalMeshComponent* const target = registry.TryGet<SkeletalMeshComponent>(key.object)) {
                    target->SetAnimationVisibility(visible);
                }
            }
        });
    }

    std::vector<EntityId> GetAttachedObjects(const Registry& registry, const EntityId parent) {
        std::vector<EntityId> children;
        if (const ComponentPool<AttachmentComponent>* const pool = registry.FindPool<AttachmentComponent>()) {
            const std::span<const EntityId> entities = pool->GetEntities();
            const std::span<const AttachmentComponent> attachments = pool->GetComponents();
            for (std::size_t i = 0; i < entities.size(); ++i) {
                if (attachments[i].parent == parent && entities[i] != parent) { children.push_back(entities[i]); }
            }
        }
        return children;
    }

    bool CanAttach(const Registry& registry, const EntityId child, const EntityId parent) {
        EntityId ancestor = parent;
        for (std::size_t depth = 0; depth < kMaxAttachmentDepth; ++depth) {
            if (ancestor == child) { return false; }
            const AttachmentComponent* const attachment = registry.TryGet<AttachmentComponent>(ancestor);
            if (attachment == nullptr || attachment->parent == 0) { return true; }
            ancestor = attachment->parent;
        }
        return false;  // Too deep (or already a loop): refuse to make it worse.
    }

    std::vector<EntityId> CollectSubtree(const Registry& registry, const EntityId root) {
        std::vector<EntityId> objects{root};
        for (std::size_t i = 0; i < objects.size() && objects.size() < 1'000'000; ++i) {
            for (const EntityId child : GetAttachedObjects(registry, objects[i])) {
                if (std::find(objects.begin(), objects.end(), child) == objects.end()) { objects.push_back(child); }
            }
        }
        return objects;
    }

}  // namespace Frost
