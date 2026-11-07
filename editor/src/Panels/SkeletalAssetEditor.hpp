#pragma once

#include <Frost/Animation/Skeleton.hpp>
#include <Frost/Animation/Socket.hpp>
#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetManager.hpp>

#include <imgui.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class EditorContext;

// The editor of a skeleton or skeletal mesh asset, in a window of its own (opened by
// double-clicking the asset in the Asset Library), after Unreal's skeletal mesh editor, kept
// minimal:
//   - Skeleton tree (left): the skeleton's bones, with the skeletal mesh's sockets under the bones
//     they follow (and those on the mesh itself under the mesh). Right-click to add a socket or a
//     bone there, or to delete one.
//   - Preview (center): the model (for a skeleton, a skeletal mesh bound to it, if there is one),
//     flat shaded, with its skeleton and sockets drawn over it, posed at a moment of an animation
//     if one is chosen. Drag to orbit, wheel to zoom; click a joint or socket to select it.
//   - Details (right): the selected bone's or socket's name, parent and offset.
// Sockets belong to the skeletal mesh (saved in its .fskmesh); bones to the skeleton (its .fskel,
// shared by every skeletal mesh and animation bound to it). Bones imported from a model are shown
// as they are (reimport to change them); bones added here can be renamed, moved and deleted. Edits
// are made to a working copy and written by Save (the level then shows them); closing with unsaved
// changes asks first.
class SkeletalAssetEditor final {
public:
    // Opens the skeleton or skeletal mesh at `path`. Throws Frost::ImportError if it (or a skeletal
    // mesh's skeleton) cannot be read.
    SkeletalAssetEditor(EditorContext& context, std::filesystem::path path);

    [[nodiscard]]
    const std::filesystem::path& GetPath() const noexcept;

    // Whether there are unsaved changes.
    [[nodiscard]]
    bool IsDirty() const noexcept;

    // Brings the window to the front (next time it is drawn).
    void Focus() noexcept;

    // Closes the window, asking first if there are unsaved changes.
    void RequestClose() noexcept;

    // Draws the window. Returns false once it has been closed.
    bool Draw(EditorContext& context);

    // Writes the changes (the skeleton and/or the skeletal mesh's sockets) and reloads what uses
    // them. Returns an error message, empty on success.
    [[nodiscard]]
    std::string Save(EditorContext& context);

    // Drops the changes: back to what the files hold.
    void Revert(EditorContext& context);

    // --- Editing (also what the window's controls do) ------------------------------------------------

    // Adds a socket on `bone` (empty = the mesh itself), with a fresh name; returns its index.
    // Skeletal meshes only.
    std::size_t AddSocket(const std::string& bone);

    // Adds a bone under `parent` (-1 = a new root), at its joint, with a fresh name; returns its
    // index.
    std::size_t AddBone(std::int32_t parent);

    // Deletes a socket.
    void DeleteSocket(std::size_t index);

    // Deletes a bone added in the editor. Refused (false, with `why` set) for imported bones, and
    // bones with child bones or sockets.
    bool DeleteBone(std::size_t index, std::string& why);

    // Renames a bone added in the editor (its sockets follow). Refused (false) for imported bones
    // and names that are empty or taken by a bone or socket.
    bool RenameBone(std::size_t index, const std::string& name);

    // Renames a socket. Refused (false) for names that are empty or taken by a bone or socket.
    bool RenameSocket(std::size_t index, const std::string& name);

    [[nodiscard]]
    const Frost::SkeletonData& GetSkeleton() const noexcept;

    [[nodiscard]]
    std::vector<Frost::SocketData>& GetSockets() noexcept;

    [[nodiscard]]
    bool IsSkeletalMesh() const noexcept;

private:
    struct Selection {
        enum class Kind { None, Mesh, Bone, Socket };
        Kind kind = Kind::None;
        std::size_t index = 0;
    };

    // Loads the files into the working copy (and the preview model).
    void Load(EditorContext& context);

    void DrawToolbar(EditorContext& context);
    void DrawTree();
    void DrawBoneNode(std::size_t bone, const std::vector<std::vector<std::size_t>>& children);
    void DrawSocketNode(std::size_t socket);
    void DrawPreview(EditorContext& context);
    void DrawDetails();
    void DrawClosePrompt(EditorContext& context);

    // Every bone's model-space transform in the preview pose (the working skeleton's rest pose, or
    // the preview animation at its time).
    [[nodiscard]]
    std::vector<glm::mat4> ComputePreviewPose() const;

    // Where a socket is in the preview pose, in model space.
    [[nodiscard]]
    glm::mat4 ComputeSocketMatrix(const Frost::SocketData& socket, const std::vector<glm::mat4>& pose) const;

    [[nodiscard]]
    bool IsNameTaken(const std::string& name) const;

    [[nodiscard]]
    std::string MakeUniqueName(const std::string& base) const;

    void MarkSkeletonDirty() noexcept;
    void MarkSocketsDirty() noexcept;

    std::filesystem::path m_Path;
    Frost::AssetType m_Type = Frost::AssetType::Skeleton;
    std::string m_Name;
    std::string m_WindowId;

    // The working copy.
    std::filesystem::path m_SkeletonPath;
    Frost::SkeletonFile m_Skeleton;
    std::vector<Frost::SocketData> m_Sockets;
    bool m_SkeletonDirty = false;
    bool m_SocketsDirty = false;
    std::string m_Error;  // Why the files could not be read (shown instead of the editor).

    // The preview: the model, an animation and where in it.
    std::shared_ptr<const Frost::SkeletalMeshAsset> m_PreviewMesh;
    // The parts it comes with (see Frost::DefaultAttachment), and the bones they follow.
    std::vector<std::pair<std::shared_ptr<const Frost::MeshAsset>, std::string>> m_PreviewParts;
    std::shared_ptr<const Frost::AnimationAsset> m_PreviewAnimation;
    float m_PreviewTime = 0.0f;
    bool m_PreviewPlaying = false;

    // The orbit camera.
    glm::vec3 m_Target{0.0f};
    float m_Distance = 100.0f;
    float m_Yaw = 30.0f;
    float m_Pitch = -15.0f;
    float m_Radius = 50.0f;  // About the size of the model, for markers and framing.
    bool m_CameraFramed = false;

    Selection m_Selection;
    bool m_Open = true;
    bool m_Focus = true;
    bool m_ClosePrompt = false;

    // The name being edited in the details panel, and whose it is.
    std::string m_NameBuffer;
    Selection m_NameBufferFor;
    std::string m_NameProblem;
};
