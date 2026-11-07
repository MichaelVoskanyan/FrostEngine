#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Scene/Bounds.hpp>
#include <Frost/Scene/Components/MaterialOverrides.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Frost {

    class AssetManager;
    class Material;
    struct AnimationAsset;
    struct MeshAsset;
    struct SkeletalMeshAsset;
    struct SkeletonData;

    // A model bound to a skeleton (a skeletal mesh asset), drawn with the object's transform in a
    // pose of its skeleton: the skeleton's rest pose, or an animation at a time. The mesh is
    // deformed on the GPU (see Renderer::SubmitSkinned), so the pose can change every frame: in the
    // game the animation plays (see AnimationSystem); in the editor the pose is the one at the
    // animation time set (the Inspector can preview it playing). Its sub-meshes' materials can be
    // overridden per object, as with a MeshComponent.
    //
    // Parts of the model attached to bones (a magazine, a muzzle brake; see
    // SkeletalMeshData::attachments) follow their bones by default; with that turned off they stay
    // where they were modeled while the rest of the model moves (for parts that gameplay code will
    // place itself).
    //
    // The model and the animation are referenced by asset (id + last known path) and kept even if
    // they could not be loaded (missing files), so saving the level does not lose them. The
    // animation plays on the mesh's skeleton by bone name (see AnimationClipData::BindTracks): any
    // animation made for a skeleton with the same bone names works.
    class SkeletalMeshComponent final {
    public:
        static constexpr const char* kDisplayName = "Skeletal Mesh";

        SkeletalMeshComponent() = default;

        // Showing a loaded model (see SetSkeletalMesh).
        explicit SkeletalMeshComponent(std::shared_ptr<const SkeletalMeshAsset> asset);

        // --- The model --------------------------------------------------------------------------

        // Assigns a loaded model (or none, with nullptr). Material overrides are kept for the
        // sub-mesh indices the new model also has; the animation is kept.
        void SetSkeletalMesh(std::shared_ptr<const SkeletalMeshAsset> asset);

        // Records a model reference that could not be loaded: nothing is drawn, but the reference
        // (and any overrides) survive a save.
        void SetUnresolvedSkeletalMesh(AssetReference reference);

        // The loaded model; null if there is none or it could not be loaded.
        [[nodiscard]]
        const std::shared_ptr<const SkeletalMeshAsset>& GetSkeletalMesh() const noexcept;

        // The referenced model, whether or not it is loaded; unset if no model is assigned.
        [[nodiscard]]
        const AssetReference& GetSkeletalMeshReference() const noexcept;

        // True when a model is referenced but could not be loaded.
        [[nodiscard]]
        bool IsMissing() const noexcept;

        // Whether the model is drawn (and can be clicked in the viewport); saved with the level. An
        // animation of the object it is attached to can override it while it plays (see
        // AnimationClipData::visibilityKeys).
        [[nodiscard]]
        bool IsVisible() const noexcept;

        void SetVisible(bool visible) noexcept;

        // The playing animation's say (run-time only, not saved): shown, hidden, or nullopt (no say:
        // IsVisible decides).
        [[nodiscard]]
        const std::optional<bool>& GetAnimationVisibility() const noexcept;

        void SetAnimationVisibility(std::optional<bool> visible) noexcept;

        // Whether it is drawn now: the animation's say, else IsVisible.
        [[nodiscard]]
        bool IsShown() const noexcept;

        // Whether the model's attached parts follow their bones (default), or stay where they were
        // modeled.
        [[nodiscard]]
        bool GetAttachmentsFollowBones() const noexcept;

        void SetAttachmentsFollowBones(bool follow) noexcept;

        // --- The animation ------------------------------------------------------------------------

        // The animation the model is posed with (or none, with nullptr: the rest pose).
        void SetAnimation(std::shared_ptr<const AnimationAsset> animation);

        // Records an animation reference that could not be loaded (the rest pose is shown).
        void SetUnresolvedAnimation(AssetReference reference);

        [[nodiscard]]
        const std::shared_ptr<const AnimationAsset>& GetAnimation() const noexcept;

        [[nodiscard]]
        const AssetReference& GetAnimationReference() const noexcept;

        // True when an animation is referenced but could not be loaded.
        [[nodiscard]]
        bool IsAnimationMissing() const noexcept;

        // Where in the animation the pose is taken from, in seconds (clamped to the animation when
        // sampled): the pose shown in the editor, and where playing starts. Negative and non-finite
        // times are stored as 0.
        [[nodiscard]]
        float GetAnimationTime() const noexcept;

        void SetAnimationTime(float seconds) noexcept;

        // How many of the animation's tracks move a bone of the model's skeleton (0 without a
        // loaded model, skeleton or animation). 0 with both loaded means the animation was made for
        // a skeleton with different bone names.
        [[nodiscard]]
        std::size_t GetBoundTrackCount() const;

        // --- Playback -----------------------------------------------------------------------------

        // Whether the animation starts playing when the game starts (AnimationSystem::Start). On
        // by default.
        [[nodiscard]]
        bool GetAutoPlay() const noexcept;

        void SetAutoPlay(bool autoPlay) noexcept;

        // Whether playing wraps around at the end (default), or stops there.
        [[nodiscard]]
        bool IsLooping() const noexcept;

        void SetLooping(bool looping) noexcept;

        // How fast the animation plays (1 = as authored; negative plays it backwards). Non-finite
        // values are stored as 1.
        [[nodiscard]]
        float GetPlayRate() const noexcept;

        void SetPlayRate(float rate) noexcept;

        // Playing is run-time state (not saved): Play starts advancing the animation time with
        // Advance, Stop freezes it where it is.
        void Play() noexcept;

        void Stop() noexcept;

        [[nodiscard]]
        bool IsPlaying() const noexcept;

        // Moves the animation time on by `seconds` (times the play rate) if playing: wrapping around
        // when looping, else stopping at the end (or the start, playing backwards). Does nothing
        // without an animation, or one of zero duration.
        void Advance(float seconds) noexcept;

        // --- One-shot animations (run-time only, not saved) ---------------------------------------

        // Plays `animation` once, from its start (or `startTime` seconds into it), in place of the component's own animation (a
        // reload, a shot), as authored (play rate 1): the pose, and the visibility of the
        // attachments (see UpdateAttachmentVisibility), follow it until it ends, and then
        // the component's own animation again (which does not advance meanwhile). Advanced by
        // Advance, whether or not the component's own animation is playing. Replaces a one-shot
        // already playing; nullptr stops it (as StopOnce).
        void PlayOnce(std::shared_ptr<const AnimationAsset> animation, float startTime = 0.0f);

        // Ends the one-shot animation now (the component's own animation takes over again).
        void StopOnce() noexcept;

        // A paused one-shot animation holds its pose (Advance leaves it where it is) until resumed
        // or stopped; e.g. to look at a moment of it in the editor. PlayOnce starts unpaused.
        void SetOneShotPaused(bool paused) noexcept;

        [[nodiscard]]
        bool IsOneShotPaused() const noexcept;

        // Whether a one-shot animation is playing.
        [[nodiscard]]
        bool IsPlayingOnce() const noexcept;

        // The one-shot animation playing (null if none), and how far into it, in seconds.
        [[nodiscard]]
        const std::shared_ptr<const AnimationAsset>& GetOneShotAnimation() const noexcept;

        [[nodiscard]]
        float GetOneShotTime() const noexcept;

        // The animation the pose (and attachment visibility) comes from now, and the time in it: the
        // one-shot's while one plays, else the component's own (null: the rest pose).
        [[nodiscard]]
        const AnimationAsset* GetActiveAnimation() const noexcept;

        [[nodiscard]]
        float GetActiveAnimationTime() const noexcept;

        // --- The pose -----------------------------------------------------------------------------

        // Brings the pose up to date after the model, animation, time or attachments setting
        // changed (cheap when nothing did): samples the animation and computes the joint palette
        // the model is drawn with. Level::Submit calls it before drawing.
        void UpdatePose();

        // What is drawn: the model's GPU-skinned mesh (see SkeletalMeshAsset::drawable); null if no
        // model is loaded.
        [[nodiscard]]
        const std::shared_ptr<const MeshAsset>& GetDrawable() const noexcept;

        // The joint palette of the current pose (see SkeletalMeshAsset::ComputePalette). Up to date
        // after UpdatePose.
        [[nodiscard]]
        const std::vector<glm::mat4>& GetPalette() const noexcept;

        // Every bone's transform in the object's space in the current pose, by skeleton bone index
        // (empty without a loaded model and skeleton). Up to date after UpdatePose.
        [[nodiscard]]
        const std::vector<glm::mat4>& GetBoneTransforms() const noexcept;

        // Where a socket of the model (see SkeletalMeshAsset::sockets) is in the current pose, in the
        // object's space: its offset on top of its bone's transform, without the bone's scale (see
        // ComputeSocketMatrix; on the model itself for a socket without a bone, or whose bone the
        // skeleton lacks). Nullopt if the model is not
        // loaded or has no socket of that name. Up to date after UpdatePose.
        [[nodiscard]]
        std::optional<glm::mat4> GetSocketTransform(std::string_view socketName) const;

        // The model's vertex positions in the current pose, in the object's space (skinned on the
        // CPU on first use after the pose changed; for picking and bounds). Empty without a model.
        [[nodiscard]]
        const std::vector<glm::vec3>& GetPosedPositions() const;

        // The posed model's box in the object's own space, or nullopt if none is loaded.
        [[nodiscard]]
        std::optional<Bounds> GetLocalBounds() const;

        // --- Materials (as MeshComponent) ---------------------------------------------------------

        [[nodiscard]]
        std::size_t GetMaterialSlotCount() const noexcept;

        [[nodiscard]]
        const std::optional<MaterialOverride>& GetMaterialOverride(std::size_t subMeshIndex) const;

        void SetMaterialOverride(std::size_t subMeshIndex, std::optional<MaterialOverride> materialOverride);

        void InvalidateMaterials() noexcept;

        [[nodiscard]]
        const std::vector<std::shared_ptr<Material>>& GetResolvedMaterials(AssetManager& assets);

    private:
        AssetReference m_MeshReference;
        std::shared_ptr<const SkeletalMeshAsset> m_Mesh;
        AssetReference m_AnimationReference;
        std::shared_ptr<const AnimationAsset> m_Animation;
        float m_AnimationTime = 0.0f;
        bool m_AttachmentsFollowBones = true;
        bool m_Visible = true;
        std::optional<bool> m_AnimationVisibility;
        bool m_AutoPlay = true;
        bool m_Looping = true;
        float m_PlayRate = 1.0f;
        bool m_Playing = false;
        std::shared_ptr<const AnimationAsset> m_OneShot;
        float m_OneShotTime = 0.0f;
        bool m_OneShotPaused = false;
        MaterialOverrides m_MaterialOverrides;

        // The current pose.
        std::vector<glm::mat4> m_BoneTransforms;
        std::vector<glm::mat4> m_Palette;
        bool m_PoseDirty = true;

        // Which skeleton bone each track of the animation moves, for the animation and skeleton it
        // was worked out for (by name, so it is kept rather than redone every frame).
        std::vector<std::int32_t> m_TrackBindings;
        const AnimationAsset* m_BoundAnimation = nullptr;
        const SkeletonData* m_BoundSkeleton = nullptr;

        // Posed positions and bounds, computed on demand.
        mutable std::vector<glm::vec3> m_PosedPositions;
        mutable std::optional<Bounds> m_PosedBounds;
        mutable bool m_PositionsDirty = true;
    };

}  // namespace Frost
