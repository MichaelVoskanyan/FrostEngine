#include "ImGuiHelpers.hpp"

#include <Frost/Platform/Filesystem.hpp>

#include <cmath>
#include <cstring>

namespace {

    constexpr const char* kMeshPayload = "FROST_MESH_ASSET";
    constexpr const char* kTexturePayload = "FROST_TEXTURE_ASSET";
    constexpr const char* kLightPayload = "FROST_NEW_LIGHT";
    constexpr const char* kFolderPayload = "FROST_FOLDER";

    [[nodiscard]]
    const char* PayloadType(const Frost::AssetType type) {
        return type == Frost::AssetType::Mesh ? kMeshPayload : kTexturePayload;
    }

    int ResizeCallback(ImGuiInputTextCallbackData* const data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* const text = static_cast<std::string*>(data->UserData);
            text->resize(static_cast<std::size_t>(data->BufTextLen));
            data->Buf = text->data();
        }
        return 0;
    }

    [[nodiscard]]
    float LinearToSrgbChannel(const float value) {
        if (value <= 0.0f) { return 0.0f; }
        return value <= 0.0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
    }

    [[nodiscard]]
    float SrgbToLinearChannel(const float value) {
        if (value <= 0.0f) { return 0.0f; }
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    }

}  // namespace

namespace EditorUI {

    bool InputText(const char* const label, std::string& text, const ImGuiInputTextFlags flags) {
        // ImGui edits the string's own buffer in place (capacity + 1 for the terminator, which
        // std::string guarantees), resizing it through the callback as the text grows.
        return ImGui::InputText(label, text.data(), text.capacity() + 1, flags | ImGuiInputTextFlags_CallbackResize,
                                ResizeCallback, &text);
    }

    glm::vec3 LinearToSrgb(const glm::vec3& linear) {
        return {LinearToSrgbChannel(linear.x), LinearToSrgbChannel(linear.y), LinearToSrgbChannel(linear.z)};
    }

    glm::vec3 SrgbToLinear(const glm::vec3& srgb) {
        return {SrgbToLinearChannel(srgb.x), SrgbToLinearChannel(srgb.y), SrgbToLinearChannel(srgb.z)};
    }

    void AssetDragSource(const std::filesystem::path& path) {
        const std::optional<Frost::AssetType> type = Frost::AssetManager::GetAssetType(path);
        if (!type.has_value()) { return; }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            // The payload is the UTF-8 path (ImGui copies it).
            const std::string text = Frost::PathToUtf8(path);
            ImGui::SetDragDropPayload(PayloadType(*type), text.data(), text.size());

            ImGui::Text("%s %s", AssetTypeTag(*type), Frost::PathToUtf8(path.filename()).c_str());
            ImGui::EndDragDropSource();
        }
    }

    std::optional<std::filesystem::path> AcceptAssetDrop(const Frost::AssetType type) {
        std::optional<std::filesystem::path> result;

        if (ImGui::BeginDragDropTarget()) {
            result = AcceptAssetPayload(type);
            ImGui::EndDragDropTarget();
        }

        return result;
    }

    std::optional<std::filesystem::path> AcceptAssetPayload(const Frost::AssetType type) {
        const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload(PayloadType(type));
        if (payload == nullptr) { return std::nullopt; }

        return Frost::PathFromUtf8(
                std::string_view(static_cast<const char*>(payload->Data), static_cast<std::size_t>(payload->DataSize)));
    }

    void FolderDragSource(const std::filesystem::path& folder) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            const std::string text = Frost::PathToUtf8(folder);
            ImGui::SetDragDropPayload(kFolderPayload, text.data(), text.size());
            ImGui::Text("[Folder] %s", Frost::PathToUtf8(folder.filename()).c_str());
            ImGui::EndDragDropSource();
        }
    }

    std::optional<std::filesystem::path> AcceptMovePayload() {
        for (const char* const type : {kMeshPayload, kTexturePayload, kFolderPayload}) {
            if (const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload(type)) {
                return Frost::PathFromUtf8(
                        std::string_view(static_cast<const char*>(payload->Data), static_cast<std::size_t>(payload->DataSize)));
            }
        }
        return std::nullopt;
    }

    void LightDragSource(const Frost::LightType type) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            ImGui::SetDragDropPayload(kLightPayload, &type, sizeof(type));
            ImGui::Text("[Light] %s", LightDisplayName(type));
            ImGui::EndDragDropSource();
        }
    }

    std::optional<Frost::LightType> AcceptLightPayload() {
        const ImGuiPayload* const payload = ImGui::AcceptDragDropPayload(kLightPayload);
        if (payload == nullptr || payload->DataSize != static_cast<int>(sizeof(Frost::LightType))) { return std::nullopt; }

        Frost::LightType type{};
        std::memcpy(&type, payload->Data, sizeof(type));
        return type;
    }

    const char* LightDisplayName(const Frost::LightType type) {
        switch (type) {
            case Frost::LightType::Directional: return "Directional Light";
            case Frost::LightType::Point: return "Point Light";
            case Frost::LightType::Spot: return "Spot Light";
        }
        return "Light";
    }

    const char* AssetTypeTag(const Frost::AssetType type) {
        return type == Frost::AssetType::Mesh ? "[Mesh]" : "[Texture]";
    }

}  // namespace EditorUI
