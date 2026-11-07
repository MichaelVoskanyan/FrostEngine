#pragma once

#include <cstdint>

namespace Frost {

    enum class AssetType : std::uint8_t {
        Mesh,          // A static model (.fmesh, imported from glTF).
        Texture,       // An image (.ftex, imported from PNG/JPEG, or extracted from a model).
        Skeleton,      // A bone hierarchy (.fskel, imported from glTF).
        SkeletalMesh,  // A model bound to a skeleton, which deforms it (.fskmesh, imported from glTF).
        Animation,     // How a skeleton's bones move over time (.fanim, one per glTF animation).
    };

    // Every asset type, in the order tools list them.
    inline constexpr AssetType kAssetTypes[] = {AssetType::Mesh, AssetType::SkeletalMesh, AssetType::Skeleton,
                                                AssetType::Animation, AssetType::Texture};

    // "mesh", "texture", "skeleton", "skeletal mesh", "animation" (for messages).
    [[nodiscard]]
    constexpr const char* GetAssetTypeName(const AssetType type) noexcept {
        switch (type) {
            case AssetType::Mesh: return "mesh";
            case AssetType::Texture: return "texture";
            case AssetType::Skeleton: return "skeleton";
            case AssetType::SkeletalMesh: return "skeletal mesh";
            case AssetType::Animation: return "animation";
        }
        return "asset";
    }

}  // namespace Frost
