#include <Frost/Scene/Components/SkeletalMeshComponent.hpp>

#include <Frost/Animation/SkinnedMesh.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Renderer/Mesh.hpp>

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace Frost {

    SkeletalMeshComponent::SkeletalMeshComponent(std::shared_ptr<const SkeletalMeshAsset> asset) {
        SetSkeletalMesh(std::move(asset));
    }

    void SkeletalMeshComponent::SetSkeletalMesh(std::shared_ptr<const SkeletalMeshAsset> asset) {
        m_Mesh = std::move(asset);
        m_MeshReference = m_Mesh != nullptr ? AssetReference{.id = m_Mesh->id, .path = m_Mesh->path} : AssetReference{};

        m_MaterialOverrides.Resize(m_Mesh != nullptr && m_Mesh->drawable != nullptr ? m_Mesh->drawable->mesh->GetSubMeshes().size()
                                                                                   : 0);
        m_BoundSkeleton = nullptr;  // (The cache is keyed by address: never let a reloaded asset reuse it.)
        m_PoseDirty = true;
        UpdatePose();  // Cheap (no upload): the pose is valid right away.
    }

    void SkeletalMeshComponent::SetUnresolvedSkeletalMesh(AssetReference reference) {
        m_Mesh.reset();
        m_MeshReference = std::move(reference);
        m_MaterialOverrides.Invalidate();
        m_BoundSkeleton = nullptr;
        m_PoseDirty = true;
        UpdatePose();
    }

    const std::shared_ptr<const SkeletalMeshAsset>& SkeletalMeshComponent::GetSkeletalMesh() const noexcept { return m_Mesh; }

    const AssetReference& SkeletalMeshComponent::GetSkeletalMeshReference() const noexcept { return m_MeshReference; }

    bool SkeletalMeshComponent::IsMissing() const noexcept { return m_Mesh == nullptr && m_MeshReference.IsSet(); }

    bool SkeletalMeshComponent::IsVisible() const noexcept { return m_Visible; }

    void SkeletalMeshComponent::SetVisible(const bool visible) noexcept { m_Visible = visible; }

    const std::optional<bool>& SkeletalMeshComponent::GetAnimationVisibility() const noexcept { return m_AnimationVisibility; }

    void SkeletalMeshComponent::SetAnimationVisibility(std::optional<bool> visible) noexcept { m_AnimationVisibility = visible; }

    bool SkeletalMeshComponent::IsShown() const noexcept { return m_AnimationVisibility.value_or(m_Visible); }

    bool SkeletalMeshComponent::GetAttachmentsFollowBones() const noexcept { return m_AttachmentsFollowBones; }

    void SkeletalMeshComponent::SetAttachmentsFollowBones(const bool follow) noexcept {
        if (follow == m_AttachmentsFollowBones) { return; }
        m_AttachmentsFollowBones = follow;
        m_PoseDirty = true;
    }

    void SkeletalMeshComponent::SetAnimation(std::shared_ptr<const AnimationAsset> animation) {
        m_Animation = std::move(animation);
        m_AnimationReference =
                m_Animation != nullptr ? AssetReference{.id = m_Animation->id, .path = m_Animation->path} : AssetReference{};
        m_BoundAnimation = nullptr;
        m_PoseDirty = true;
    }

    void SkeletalMeshComponent::SetUnresolvedAnimation(AssetReference reference) {
        m_Animation.reset();
        m_AnimationReference = std::move(reference);
        m_BoundAnimation = nullptr;
        m_PoseDirty = true;
    }

    const std::shared_ptr<const AnimationAsset>& SkeletalMeshComponent::GetAnimation() const noexcept { return m_Animation; }

    const AssetReference& SkeletalMeshComponent::GetAnimationReference() const noexcept { return m_AnimationReference; }

    bool SkeletalMeshComponent::IsAnimationMissing() const noexcept {
        return m_Animation == nullptr && m_AnimationReference.IsSet();
    }

    float SkeletalMeshComponent::GetAnimationTime() const noexcept { return m_AnimationTime; }

    void SkeletalMeshComponent::SetAnimationTime(const float seconds) noexcept {
        const float time = std::isfinite(seconds) ? std::max(seconds, 0.0f) : 0.0f;
        if (time == m_AnimationTime) { return; }
        m_AnimationTime = time;
        m_PoseDirty = true;
    }

    std::size_t SkeletalMeshComponent::GetBoundTrackCount() const {
        if (m_Mesh == nullptr || m_Mesh->skeleton == nullptr || m_Animation == nullptr) { return 0; }

        const std::vector<std::int32_t> bindings = m_Animation->clip.BindTracks(m_Mesh->skeleton->skeleton);
        return static_cast<std::size_t>(std::count_if(bindings.begin(), bindings.end(), [](const std::int32_t bone) { return bone >= 0; }));
    }

    bool SkeletalMeshComponent::GetAutoPlay() const noexcept { return m_AutoPlay; }

    void SkeletalMeshComponent::SetAutoPlay(const bool autoPlay) noexcept { m_AutoPlay = autoPlay; }

    bool SkeletalMeshComponent::IsLooping() const noexcept { return m_Looping; }

    void SkeletalMeshComponent::SetLooping(const bool looping) noexcept { m_Looping = looping; }

    float SkeletalMeshComponent::GetPlayRate() const noexcept { return m_PlayRate; }

    void SkeletalMeshComponent::SetPlayRate(const float rate) noexcept { m_PlayRate = std::isfinite(rate) ? rate : 1.0f; }

    void SkeletalMeshComponent::Play() noexcept { m_Playing = true; }

    void SkeletalMeshComponent::Stop() noexcept { m_Playing = false; }

    bool SkeletalMeshComponent::IsPlaying() const noexcept { return m_Playing; }

    void SkeletalMeshComponent::Advance(const float seconds) noexcept {
        if (!std::isfinite(seconds)) { return; }

        if (m_OneShot != nullptr) {
            // The component's own animation waits for it.
            if (m_OneShotPaused) { return; }
            m_OneShotTime += std::max(seconds, 0.0f);
            if (m_OneShotTime >= m_OneShot->clip.duration) {
                m_OneShot.reset();
                m_BoundAnimation = nullptr;  // (Keyed by address: never let another asset reuse it.)
            }
            m_PoseDirty = true;
            return;
        }

        if (!m_Playing || m_Animation == nullptr) { return; }

        const float duration = m_Animation->clip.duration;
        if (!(duration > 0.0f)) { return; }

        float time = std::min(m_AnimationTime, duration) + seconds * m_PlayRate;
        if (m_Looping) {
            time = std::fmod(time, duration);
            if (time < 0.0f) { time += duration; }
        }
        else if (time >= duration || time <= 0.0f) {
            time = std::clamp(time, 0.0f, duration);
            m_Playing = false;  // Reached the end (or, backwards, the start).
        }

        m_AnimationTime = time;
        m_PoseDirty = true;
    }

    void SkeletalMeshComponent::PlayOnce(std::shared_ptr<const AnimationAsset> animation, const float startTime) {
        m_OneShot = std::move(animation);
        m_OneShotTime = std::isfinite(startTime) ? std::max(startTime, 0.0f) : 0.0f;
        m_OneShotPaused = false;
        m_BoundAnimation = nullptr;
        m_PoseDirty = true;
    }

    void SkeletalMeshComponent::StopOnce() noexcept {
        if (m_OneShot == nullptr) { return; }
        m_OneShot.reset();
        m_OneShotTime = 0.0f;
        m_BoundAnimation = nullptr;
        m_PoseDirty = true;
    }

    void SkeletalMeshComponent::SetOneShotPaused(const bool paused) noexcept { m_OneShotPaused = paused; }

    bool SkeletalMeshComponent::IsOneShotPaused() const noexcept { return m_OneShotPaused; }

    bool SkeletalMeshComponent::IsPlayingOnce() const noexcept { return m_OneShot != nullptr; }

    const std::shared_ptr<const AnimationAsset>& SkeletalMeshComponent::GetOneShotAnimation() const noexcept { return m_OneShot; }

    float SkeletalMeshComponent::GetOneShotTime() const noexcept { return m_OneShotTime; }

    const AnimationAsset* SkeletalMeshComponent::GetActiveAnimation() const noexcept {
        return m_OneShot != nullptr ? m_OneShot.get() : m_Animation.get();
    }

    float SkeletalMeshComponent::GetActiveAnimationTime() const noexcept {
        return m_OneShot != nullptr ? m_OneShotTime : m_AnimationTime;
    }

    void SkeletalMeshComponent::UpdatePose() {
        if (!m_PoseDirty) { return; }
        m_PoseDirty = false;
        m_PositionsDirty = true;

        m_BoneTransforms.clear();
        m_Palette.clear();
        if (m_Mesh == nullptr) { return; }

        if (m_Mesh->skeleton == nullptr) {
            m_Palette = m_Mesh->restPalette;  // The bind pose.
            return;
        }

        const SkeletonData& skeleton = m_Mesh->skeleton->skeleton;
        std::vector<BoneTransform> pose = skeleton.GetRestPose();

        if (const AnimationAsset* const animation = GetActiveAnimation(); animation != nullptr) {
            if (m_BoundAnimation != animation || m_BoundSkeleton != &skeleton) {
                m_TrackBindings = animation->clip.BindTracks(skeleton);
                m_BoundAnimation = animation;
                m_BoundSkeleton = &skeleton;
            }
            animation->clip.Sample(GetActiveAnimationTime(), m_TrackBindings, pose);
        }

        m_BoneTransforms = skeleton.ComputeModelTransforms(pose);
        m_Mesh->ComputePalette(m_BoneTransforms, m_AttachmentsFollowBones, m_Palette);
    }

    const std::shared_ptr<const MeshAsset>& SkeletalMeshComponent::GetDrawable() const noexcept {
        static const std::shared_ptr<const MeshAsset> kNone;
        return m_Mesh != nullptr ? m_Mesh->drawable : kNone;
    }

    const std::vector<glm::mat4>& SkeletalMeshComponent::GetPalette() const noexcept { return m_Palette; }

    const std::vector<glm::mat4>& SkeletalMeshComponent::GetBoneTransforms() const noexcept { return m_BoneTransforms; }

    std::optional<glm::mat4> SkeletalMeshComponent::GetSocketTransform(const std::string_view socketName) const {
        if (m_Mesh == nullptr) { return std::nullopt; }
        const SocketData* const socket = m_Mesh->FindSocket(socketName);
        if (socket == nullptr) { return std::nullopt; }

        glm::mat4 bone(1.0f);
        if (!socket->bone.empty() && m_Mesh->skeleton != nullptr) {
            const std::optional<std::size_t> index = m_Mesh->skeleton->skeleton.FindBone(socket->bone);
            if (index.has_value() && *index < m_BoneTransforms.size()) { bone = m_BoneTransforms[*index]; }
        }
        return ComputeSocketMatrix(bone, *socket);
    }

    const std::vector<glm::vec3>& SkeletalMeshComponent::GetPosedPositions() const {
        if (!m_PositionsDirty) { return m_PosedPositions; }
        m_PositionsDirty = false;

        m_PosedPositions.clear();
        m_PosedBounds.reset();
        if (m_Mesh == nullptr || m_Palette.size() != m_Mesh->paletteSize) { return m_PosedPositions; }

        SkinPositions(m_Mesh->data.mesh.vertices, m_Mesh->paletteSkin, m_Palette, m_PosedPositions);
        if (!m_PosedPositions.empty()) {
            Bounds bounds{.min = m_PosedPositions.front(), .max = m_PosedPositions.front()};
            for (const glm::vec3& position : m_PosedPositions) {
                bounds.min = glm::min(bounds.min, position);
                bounds.max = glm::max(bounds.max, position);
            }
            m_PosedBounds = bounds;
        }
        return m_PosedPositions;
    }

    std::optional<Bounds> SkeletalMeshComponent::GetLocalBounds() const {
        static_cast<void>(GetPosedPositions());
        return m_PosedBounds;
    }

    std::size_t SkeletalMeshComponent::GetMaterialSlotCount() const noexcept { return m_MaterialOverrides.GetSlotCount(); }

    const std::optional<MaterialOverride>& SkeletalMeshComponent::GetMaterialOverride(const std::size_t subMeshIndex) const {
        return m_MaterialOverrides.Get(subMeshIndex);
    }

    void SkeletalMeshComponent::SetMaterialOverride(const std::size_t subMeshIndex,
                                                    std::optional<MaterialOverride> materialOverride) {
        m_MaterialOverrides.Set(subMeshIndex, std::move(materialOverride));
    }

    void SkeletalMeshComponent::InvalidateMaterials() noexcept { m_MaterialOverrides.Invalidate(); }

    const std::vector<std::shared_ptr<Material>>& SkeletalMeshComponent::GetResolvedMaterials(AssetManager& assets) {
        return m_MaterialOverrides.Resolve(assets, GetDrawable().get(), m_Mesh != nullptr ? m_Mesh->name : std::string());
    }

}  // namespace Frost
