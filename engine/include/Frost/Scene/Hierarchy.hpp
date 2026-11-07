#pragma once

#include <Frost/Scene/Registry.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace Frost {

    struct AttachmentComponent;

    // Objects attached to objects (see AttachmentComponent): where they are in the world, and who is
    // attached to whom. Everything is worked out from the registry (entity = object id), so any
    // code holding the level's registry can use it.

    // Parent chains longer than this are cut (which also stops a loop, should one be saved).
    inline constexpr std::size_t kMaxAttachmentDepth = 64;

    // How far a bone of an object's skeletal mesh has moved from its rest pose, in the object's
    // model space: bone(now) x bone(rest)^-1, from the mesh's current pose (see
    // SkeletalMeshComponent::UpdatePose). Identity if the object has no skeletal mesh, no skeleton,
    // or no bone of that name, or for an empty name.
    [[nodiscard]]
    glm::mat4 ComputeBoneMotion(const Registry& registry, EntityId object, std::string_view bone);

    // Where an attachment's mount is in its parent's model space now: the parent's socket (see
    // SkeletalMeshComponent::GetSocketTransform) if it names one the parent has, else the motion of
    // its bone (ComputeBoneMotion).
    [[nodiscard]]
    glm::mat4 ComputeMountMatrix(const Registry& registry, const AttachmentComponent& attachment);

    // The transform from an object's space to the world: its own transform on top of its parent's
    // world transform and the motion of the bone it is attached to, up the chain of parents. An
    // object whose parent is gone counts as not attached.
    [[nodiscard]]
    glm::mat4 ComputeWorldMatrix(const Registry& registry, EntityId object);

    // Where an object is in the world and which way it faces (its world transform without the
    // scale): for what is placed rather than drawn, such as lights and cameras. For an object that
    // is not attached, exactly its transform's position and rotation.
    struct WorldPlacement {
        glm::vec3 position{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    [[nodiscard]]
    WorldPlacement ComputeWorldPlacement(const Registry& registry, EntityId object);

    // Whether the object is attached to another object of the registry (one that exists).
    [[nodiscard]]
    bool IsAttached(const Registry& registry, EntityId object);

    // Whether the object's models are drawn now: its own mesh components say they are shown (see
    // MeshComponent::IsShown), and so do those of every object up its chain of parents (hiding a
    // weapon hides what is attached to it).
    [[nodiscard]]
    bool IsShownInHierarchy(const Registry& registry, EntityId object);

    // Lets the animations playing on skeletal meshes show and hide objects: each model's animation
    // visibility (see MeshComponent::SetAnimationVisibility) becomes
    //   - for an object named by the visibility keys of the animation trigger playing on a skeletal
    //     mesh (see AnimationTriggerComponent), what those keys say at the animation's current time
    //     once its first key has passed;
    //   - else, for an attached object with a slot, what the parent's own animation (not one a
    //     trigger plays) says for that slot at its current time (see
    //     AnimationClipData::EvaluateVisibility);
    //   - else no say.
    // Level calls it every time it is drawn.
    void UpdateAttachmentVisibility(Registry& registry);

    // The objects attached directly to `parent`, in storage order.
    [[nodiscard]]
    std::vector<EntityId> GetAttachedObjects(const Registry& registry, EntityId parent);

    // Whether attaching `child` to `parent` keeps the hierarchy a tree (not to itself, nor to one of
    // its own descendants).
    [[nodiscard]]
    bool CanAttach(const Registry& registry, EntityId child, EntityId parent);

    // `root` and everything attached below it, parents before their children.
    [[nodiscard]]
    std::vector<EntityId> CollectSubtree(const Registry& registry, EntityId root);

}  // namespace Frost
