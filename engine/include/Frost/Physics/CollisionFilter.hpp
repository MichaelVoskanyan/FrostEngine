#pragma once

#include <cstdint>

namespace Frost {

    // Collision layers: every collider is on exactly one of kCollisionLayerCount layers (numbered
    // from 0), and has a mask of the layers it collides with. Layers are how a game sorts what
    // collides with what (players, projectiles, debris, triggers...) and what a raycast can hit.
    //
    // Layers are numbered for now; naming them (per project) will come with project settings.
    // Layer 0 is the Default layer, which every collider starts on.

    // A set of layers: bit n is layer n.
    using CollisionLayerMask = std::uint32_t;

    inline constexpr int kCollisionLayerCount = 32;
    inline constexpr int kDefaultCollisionLayer = 0;

    inline constexpr CollisionLayerMask kNoCollisionLayers = 0;
    inline constexpr CollisionLayerMask kAllCollisionLayers = ~CollisionLayerMask{0};

    // The mask with only `layer` in it; empty for a layer out of range.
    [[nodiscard]]
    constexpr CollisionLayerMask CollisionLayerBit(const int layer) noexcept {
        return layer >= 0 && layer < kCollisionLayerCount ? CollisionLayerMask{1} << layer : kNoCollisionLayers;
    }

    // Which layer a collider is on, and which layers it collides with. Part of every collider
    // component (BoxColliderComponent, CapsuleColliderComponent).
    struct CollisionFilter {
        int layer = kDefaultCollisionLayer;        // 0 to kCollisionLayerCount - 1.
        CollisionLayerMask mask = kAllCollisionLayers;  // The layers it collides with.

        [[nodiscard]]
        constexpr bool IsOnAnyOf(const CollisionLayerMask layers) const noexcept {
            return (CollisionLayerBit(layer) & layers) != 0;
        }

        // Whether two colliders collide (PhysicsSystem): only if each one's mask has the other's
        // layer. Both must agree, so a collider never pushes one that passes through it.
        [[nodiscard]]
        constexpr bool CollidesWith(const CollisionFilter& other) const noexcept {
            return other.IsOnAnyOf(mask) && IsOnAnyOf(other.mask);
        }

        friend constexpr bool operator==(const CollisionFilter&, const CollisionFilter&) = default;
    };

}  // namespace Frost
