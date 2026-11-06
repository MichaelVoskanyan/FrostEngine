#pragma once

#include <Frost/Scene/GameObject.hpp>

#include <cstddef>

class EditorContext;

// The "Inspector" window: everything editable about the selected object, which is its name and
// its components, one collapsible section each (in Frost::BuiltinComponents order):
//   - Transform (every object has one): position, rotation in degrees, scale with an optional
//     uniform lock.
//   - Mesh: its model, shown by name (drop a model from the Asset Library onto it to swap it), and
//     per sub-mesh, its material: either the model's own, or an override with editable factors and
//     texture slots. Drop an image from the Asset Library onto a slot to use it; each slot can also
//     inherit the model's texture or be left empty.
//   - Light: type, color (or color temperature), intensity, radius/distance and spot cone angles.
//     Lights are aimed by rotating the object.
//   - Camera: field of view and clip distances, and how it is mounted on its object (offset and
//     rotation); "Align with View" moves it (with its object) to where the viewport is looking
//     from. The game looks through the level's camera.
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
    bool DrawAddComponentMenu(Frost::GameObject& object);

    // The properties of one component type; each returns whether it changed the object. Every type
    // in Frost::BuiltinComponents needs one (the build fails otherwise).
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::TransformComponent& transform);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::MeshComponent& mesh);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::LightComponent& light);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::CameraComponent& camera);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::FlyControllerComponent& controller);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object,
                       Frost::CharacterControllerComponent& controller);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::CapsuleColliderComponent& collider);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::BoxColliderComponent& collider);
    bool DrawComponent(EditorContext& context, Frost::GameObject& object, Frost::RigidBodyComponent& body);

    bool DrawMaterials(EditorContext& context, Frost::MeshComponent& mesh);
    bool DrawMaterialOverride(EditorContext& context, Frost::MaterialOverride& materialOverride);
    // One row of the texture slot table (label, then a button that is a drop target and opens a
    // menu).
    bool DrawTextureSlot(EditorContext& context, Frost::MaterialSlot slot, Frost::TextureOverride& texture);

    bool m_UniformScale = true;
};
