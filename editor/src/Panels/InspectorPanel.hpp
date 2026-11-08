#pragma once

#include <Frost/Animation/AnimationClip.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Scene/GameObject.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

class EditorContext;

// The "Inspector" window: everything editable about the selected object, which is its name and
// its components, one collapsible section each (in Frost::BuiltinComponents order):
//   - Transform (every object has one): position, rotation in degrees, scale with an optional
//     uniform lock. For an attached object, relative to its mount.
//   - Attachment: the object it is attached to (picking one puts it on its mount; "(none)"
//     detaches it where it is), the bone or socket of that object's skeletal mesh it moves with,
//     and the slot (role) animations show and hide it by.
//   - Mesh: its model, shown by name (drop a model from the Asset Library onto it to swap it;
//     an attached object keeps its bone and slot, e.g. an extended magazine replacing a magazine),
//     whether it is visible, and per sub-mesh, its material: either the model's own, or an override
//     with editable factors and texture slots. Drop an image from the Asset Library onto a slot to
//     use it; each slot can also inherit the model's texture or be left empty.
//   - Skeletal Mesh: its model (drop a skeletal mesh onto it) and skeleton, whether it is visible,
//     the objects attached to it (and adding those its model comes with), whether the parts
//     attached to bones follow them, the animation (drop an animation onto it, or pick one), where
//     in it the model is posed (a time slider, with Play to preview the animation playing), the
//     animation's visibility keys (when it shows and hides attachments, by slot), how the game
//     plays it (on start, looping, speed), and per-sub-mesh materials as for a Mesh.
//   - Animation Triggers: keys that play animations once on the skeletal mesh in the game (R: a
//     reload), each previewable (played, or posed at a moment of it), with its visibility keys:
//     which of the objects attached to it are shown or hidden, and from when.
//   - Light: type, color (or color temperature), intensity, radius/distance and spot cone angles.
//     Lights are aimed by rotating the object.
//   - Camera: field of view and clip distances, and how it is mounted on its object (offset and
//     rotation); "Align with View" moves it (with its object) to where the viewport is looking
//     from. The game looks through the level's camera.
//   - Audio Source: its sound (drop a sound from the Asset Library onto it, or pick one), a preview
//     (play/pause, stop, and a time slider to jump around in it; heard from the viewport camera, as
//     the game would play it, with every setting taking effect as it is changed; it stops when
//     another object is selected), volume, pitch,
//     looping, whether it plays when the game starts, and how it is heard in space (or not):
//     minimum and maximum distance, the fade between them, the Doppler effect and occlusion (with
//     what the preview hears of them now: walls in the way, the pitch shift).
//   - Audio Listener: whether it is the level's active listener (the one the game hears through;
//     making one active makes the others inactive), where it sits on its object (its offset, or
//     the camera's), and what the game hears from otherwise.
//   - Fly Controller: the speeds and mouse sensitivity of flying the object around in the game.
//   - Character Controller: walking and jumping speeds and mouse sensitivity, and what the
//     character still needs (a simulated rigid body, a collider, a camera).
//   - Capsule Collider: its center, radius and height, or fitted to the object's model.
//   - Box Collider: the box's center and size in the object's space, or fitted to its model.
//   - Rigid Body: whether it is simulated (falls and collides), and its mass.
// "Add Component" attaches any component the object does not have yet; the close button on a
// section's header (or its right-click menu) removes that component.
class InspectorPanel final {
public:
    static constexpr const char* kWindowName = "Inspector";

    void Draw(EditorContext& context);

private:
    // The section of component T (header and, if open, its properties). Sets `removeRequested`
    // if the user asked to remove the component (it is removed after all sections are drawn).
    // Returns whether it changed the object.
    template <typename T>
    bool DrawComponentSection(EditorContext& context, Frost::GameObject& object, bool& removeRequested);

    // The "Add Component" button and its menu. Returns whether a component was added.
    bool DrawAddComponentMenu(EditorContext& context, Frost::GameObject& object);

    // The properties of one component type; each returns whether it changed the object. Every type
    // in Frost::BuiltinComponents needs one (the build fails otherwise).
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::TransformComponent& transform);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::AttachmentComponent& attachment);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::AnimationTriggerComponent& component);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::MeshComponent& mesh);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::SkeletalMeshComponent& mesh);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::LightComponent& light);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::CameraComponent& camera);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::AudioSourceComponent& source);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::AudioListenerComponent& listener);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::FlyControllerComponent& controller);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object,
                       Frost::CharacterControllerComponent& controller);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::CapsuleColliderComponent& collider);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::BoxColliderComponent& collider);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::RigidBodyComponent& body);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::PhysicsQueryComponent& query);

    // The per-sub-mesh material overrides of a component that draws a model (MeshComponent or
    // SkeletalMeshComponent); `model` is what it draws, if loaded.
    template <typename Component>
    bool DrawMaterials(EditorContext& context, Component& mesh, const Frost::MeshAsset* model);

    // The audio source section's sound slot (a drop target that opens the library's sounds), and its
    // preview controls.
    bool DrawSoundSlot(EditorContext& context, Frost::AudioSourceComponent& source);
    void DrawSoundPreview(EditorContext& context, const Frost::GameObject& object, const Frost::AudioSourceComponent& source);

    // The skeletal mesh section's animation slot, time and preview controls.
    bool DrawAnimation(EditorContext& context, const Frost::GameObject& object, Frost::SkeletalMeshComponent& mesh);

    // The skeletal mesh section's list of the objects attached to it.
    void DrawAttachedObjects(EditorContext& context, const Frost::GameObject& object, const Frost::SkeletalMeshComponent& mesh);

    // The visibility keys of an animation trigger of `object`: which of the objects attached to it
    // are shown or hidden, and from when, while the trigger's animation plays (saved with the
    // level). "Add key" adds one at `time`. Returns whether they changed.
    bool DrawTriggerKeys(EditorContext& context, const Frost::GameObject& object, Frost::AnimationTrigger& trigger, float time);

    // The (slot-based) visibility keys of `animation` (the skeletal mesh's own animation, played on
    // `object`), editable: each edit is saved to the animation asset when done. "Add key" adds one
    // at `time`.
    void DrawVisibilityKeys(EditorContext& context, const Frost::GameObject& object,
                            const std::shared_ptr<const Frost::AnimationAsset>& animation, float time);
    bool DrawMaterialOverride(EditorContext& context, Frost::MaterialOverride& materialOverride);
    // One row of the texture slot table (label, then a button that is a drop target and opens a
    // menu).
    bool DrawTextureSlot(EditorContext& context, Frost::MaterialSlot slot, Frost::TextureOverride& texture);

    bool m_UniformScale = true;

    // The object whose skeletal mesh animation is being previewed (played), or 0.
    Frost::ObjectId m_PlayingObject = 0;

    // The object a one-shot animation was previewed on (stopped when it is no longer shown), or 0.
    Frost::ObjectId m_OneShotObject = 0;

    // The object whose sound was previewed (stopped when it is no longer shown, so no preview plays
    // on unnoticed), or 0.
    Frost::ObjectId m_SoundObject = 0;

    // Requests from the sections, carried out once every section is drawn (they add or remove
    // components): detach this object, add the default attachments of this one.
    Frost::ObjectId m_DetachRequest = 0;
    Frost::ObjectId m_AddDefaultAttachmentsRequest = 0;

    // Visibility keys being edited, per animation asset: a copy of the keys of the asset version it
    // was made from, saved to the asset when an edit is done.
    struct KeyDraft {
        std::weak_ptr<const Frost::AnimationAsset> source;
        std::vector<Frost::VisibilityKey> keys;
    };
    std::map<Frost::AssetId, KeyDraft> m_KeyDrafts;
};
