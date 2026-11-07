#pragma once

#include <Frost/Scene/Registry.hpp>

#include <string>

namespace Frost {

    // Makes its object a child of another object of the level: it moves with its parent and, if a
    // bone is named, with that bone of the parent's skeletal mesh. This is how a weapon carries its
    // magazine, muzzle brake or suppressor: each is an object of its own (with a Mesh), attached to
    // the weapon, that can be removed or replaced.
    //
    // The object's transform is relative to the parent's model space, and a bone's motion applies
    // relative to the bone's rest pose:
    //   world = parent's world x bone(now) x bone(rest)^-1 x local
    // So a part modeled in the parent's space (with the same origin, as attachments for a weapon
    // usually are in Blender) sits exactly where it was modeled with an identity transform, and
    // follows the bone from there; no per-socket offsets are needed.
    //
    // Alternatively it is attached to a socket of the parent's skeletal mesh (see SocketData), and
    // its transform is relative to the socket, wherever the socket is now:
    //   world = parent's world x socket(now) x local
    // for parts modeled at their own origin (a suppressor snapped onto a muzzle socket). A socket
    // the parent's model does not have counts as none (the bone, if any, is used).
    //
    // The slot names the role the attachment fills ("Magazine", "Muzzle", ...). The parent's own
    // animation can refer to attachments by slot (see AnimationClipData::visibilityKeys), so a
    // replacement part in the same slot (an extended magazine) behaves like the one it replaces.
    // Animation triggers name the objects they show and hide instead (see
    // AnimationTriggerComponent), so several objects in one slot can be told apart.
    //
    // Attached objects are drawn where this puts them (see ComputeWorldMatrix), hidden with the
    // object they are attached to (see IsShownInHierarchy), and left out of physics. A parent that
    // is not in the level (or an attachment that would make a loop) counts as not attached; see
    // Frost/Scene/Hierarchy.hpp for the functions that work all this out.
    struct AttachmentComponent {
        static constexpr const char* kDisplayName = "Attachment";

        EntityId parent = 0;  // The parent object's id (0 = none: not attached).
        std::string bone;     // A bone of the parent's skeletal mesh; empty = the parent itself.
        std::string slot;     // The role it fills, for animations; may be empty.
        std::string socket;   // A socket of the parent's skeletal mesh; if set (and found), used instead of `bone`.
    };

}  // namespace Frost
