#pragma once

#include <Frost/Scene/GameObject.hpp>

#include <cstddef>

class EditorContext;

// The "Inspector" window: everything editable about the selected object.
//   - Name and transform (position, rotation in degrees, scale with an optional uniform lock).
//   - Its model: shown by name; drop a model from the Asset Library onto it to swap it.
//   - Per sub-mesh, its material: either the model's own, or an override with editable factors
//     and texture slots. Drop an image from the Asset Library onto a slot to use it; each slot can
//     also inherit the model's texture or be left empty.
//   - Its light, if any: type, color (or color temperature), intensity, radius/distance and spot
//     cone angles. Lights are aimed by rotating the object. Any object can gain or lose a light.
class InspectorPanel final {
public:
    static constexpr const char* kWindowName = "Inspector";

    void Draw(EditorContext& context);

private:
    // Each returns whether it changed the object.
    bool DrawTransform(Frost::GameObject& object);
    bool DrawMesh(EditorContext& context, Frost::GameObject& object);
    bool DrawLight(Frost::GameObject& object);
    bool DrawMaterials(EditorContext& context, Frost::GameObject& object);
    bool DrawMaterialOverride(EditorContext& context, Frost::MaterialOverride& materialOverride);
    // One row of the texture slot table (label, then a button that is a drop target and opens a
    // menu).
    bool DrawTextureSlot(EditorContext& context, Frost::MaterialSlot slot, Frost::TextureOverride& texture);

    bool m_UniformScale = true;
};
