#pragma once

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Renderer/Light.hpp>

#include <imgui.h>

#include <glm/vec3.hpp>

#include <filesystem>
#include <optional>
#include <string>

// Small ImGui conveniences shared by the editor's panels.
namespace EditorUI {

    // InputText bound to a std::string (grows the string as needed).
    bool InputText(const char* label, std::string& text, ImGuiInputTextFlags flags = 0);

    // Color pickers show sRGB-encoded values (what people expect from a color picker), while
    // materials store linear ones; these convert between the two per channel.
    [[nodiscard]]
    glm::vec3 LinearToSrgb(const glm::vec3& linear);

    [[nodiscard]]
    glm::vec3 SrgbToLinear(const glm::vec3& srgb);

    // Drag and drop of assets (by path) between the Asset Library and the viewport/inspector. Each
    // asset type has its own payload type, so targets only react to what they can accept.

    // Call right after the item to drag from. Does nothing for unsupported files.
    void AssetDragSource(const std::filesystem::path& path);

    // Call right after the item to drop onto. Returns the dropped asset's path.
    [[nodiscard]]
    std::optional<std::filesystem::path> AcceptAssetDrop(Frost::AssetType type);

    // As AcceptAssetDrop, but inside a BeginDragDropTarget()/EndDragDropTarget() pair the caller
    // opened (for targets that accept several kinds of payload).
    [[nodiscard]]
    std::optional<std::filesystem::path> AcceptAssetPayload(Frost::AssetType type);

    // Drag a folder of the Asset Library (to move it). Call right after the item to drag from.
    void FolderDragSource(const std::filesystem::path& folder);

    // Inside a caller-opened drop target: an asset or folder dropped to be moved (any asset type
    // or a folder), by path.
    [[nodiscard]]
    std::optional<std::filesystem::path> AcceptMovePayload();

    // Drag a new light of `type` (from the Lights palette). Call right after the item to drag from.
    void LightDragSource(Frost::LightType type);

    // The type of light dropped, inside a caller-opened drop target.
    [[nodiscard]]
    std::optional<Frost::LightType> AcceptLightPayload();

    // "Directional Light", "Point Light", "Spot Light".
    [[nodiscard]]
    const char* LightDisplayName(Frost::LightType type);

    // "[Mesh]" / "[Texture]" style tag for lists.
    [[nodiscard]]
    const char* AssetTypeTag(Frost::AssetType type);

}  // namespace EditorUI
