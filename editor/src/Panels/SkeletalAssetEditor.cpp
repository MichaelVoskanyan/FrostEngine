#include "SkeletalAssetEditor.hpp"

#include "../EditorContext.hpp"
#include "../Widgets/ImGuiHelpers.hpp"

#include <Frost/Animation/SkinnedMesh.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Scene/Components/TransformComponent.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>

namespace {

    constexpr ImVec4 kErrorColor(1.0f, 0.45f, 0.4f, 1.0f);
    // Colors of the preview's overlays (sRGB; see EditorUI::DisplayColor).
    [[nodiscard]] ImU32 BoneColor() { return EditorUI::DisplayColor(230, 200, 80); }
    [[nodiscard]] ImU32 AddedBoneColor() { return EditorUI::DisplayColor(90, 210, 230); }
    [[nodiscard]] ImU32 SelectedColor() { return EditorUI::DisplayColor(255, 140, 0); }
    [[nodiscard]] ImU32 SocketColor() { return EditorUI::DisplayColor(120, 255, 140); }
    constexpr float kFieldOfView = 45.0f;     // Degrees, vertical.
    constexpr std::size_t kMaxPreviewTriangles = 60000;
    constexpr float kPickRadius = 8.0f;       // Pixels.

    // A perspective view onto a rectangle of the window.
    struct PreviewProjection {
        glm::mat4 view{1.0f};
        glm::mat4 projection{1.0f};
        ImVec2 origin;
        ImVec2 size;

        [[nodiscard]]
        std::optional<ImVec2> ToScreen(const glm::vec3& point) const {
            const glm::vec4 clip = projection * view * glm::vec4(point, 1.0f);
            if (clip.w <= 1.0e-4f) { return std::nullopt; }
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            return ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - ndc.y * 0.5f) * size.y);
        }
    };

    [[nodiscard]]
    glm::vec3 ToEulerDegrees(const glm::quat& rotation) {
        Frost::TransformComponent transform;
        transform.SetFromMatrix(glm::mat4_cast(rotation));
        return transform.rotation + glm::vec3(0.0f);  // (+0: no "-0.0" on display.)
    }

    [[nodiscard]]
    glm::quat FromEulerDegrees(const glm::vec3& degrees) {
        return Frost::TransformComponent{.position = glm::vec3(0.0f), .rotation = degrees, .scale = glm::vec3(1.0f)}.GetRotation();
    }

    [[nodiscard]]
    float Distance(const ImVec2& a, const ImVec2& b) {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

}  // namespace

SkeletalAssetEditor::SkeletalAssetEditor(EditorContext& context, std::filesystem::path path)
    : m_Path(Frost::AssetManager::NormalizePath(path)),
      m_Type(Frost::AssetManager::GetAssetType(m_Path).value_or(Frost::AssetType::Skeleton)),
      m_Name(Frost::PathToUtf8(m_Path.stem())),
      m_WindowId(Frost::PathToUtf8(m_Path)) {
    if (m_Type != Frost::AssetType::Skeleton && m_Type != Frost::AssetType::SkeletalMesh) {
        throw Frost::ImportError("'" + Frost::PathToUtf8(m_Path.filename()) + "' is not a skeleton or skeletal mesh.");
    }
    Load(context);
    if (!m_Error.empty()) { throw Frost::ImportError(m_Error); }
}

const std::filesystem::path& SkeletalAssetEditor::GetPath() const noexcept { return m_Path; }

bool SkeletalAssetEditor::IsDirty() const noexcept { return m_SkeletonDirty || m_SocketsDirty; }

void SkeletalAssetEditor::Focus() noexcept { m_Focus = true; }

void SkeletalAssetEditor::RequestClose() noexcept {
    if (IsDirty()) {
        m_ClosePrompt = true;
        m_Focus = true;
    }
    else {
        m_Open = false;
    }
}

bool SkeletalAssetEditor::IsSkeletalMesh() const noexcept { return m_Type == Frost::AssetType::SkeletalMesh; }

const Frost::SkeletonData& SkeletalAssetEditor::GetSkeleton() const noexcept { return m_Skeleton.skeleton; }

std::vector<Frost::SocketData>& SkeletalAssetEditor::GetSockets() noexcept { return m_Sockets; }

void SkeletalAssetEditor::MarkSkeletonDirty() noexcept { m_SkeletonDirty = true; }

void SkeletalAssetEditor::MarkSocketsDirty() noexcept { m_SocketsDirty = true; }

void SkeletalAssetEditor::Load(EditorContext& context) {
    m_Error.clear();
    m_SkeletonDirty = false;
    m_SocketsDirty = false;
    m_Selection = {};
    m_NameBufferFor = {};

    const std::filesystem::path project = context.GetLibrary().GetRegistry().GetProjectDirectory();
    Frost::AssetManager& assets = context.GetAssets();
    try {
        if (IsSkeletalMesh()) {
            const Frost::SkeletalMeshFile file = Frost::AssetFile::ReadSkeletalMesh(m_Path, project);
            m_Sockets = file.sockets;

            const std::optional<std::filesystem::path> skeleton = context.GetLibrary().GetRegistry().Resolve(file.skeleton);
            if (!skeleton.has_value()) {
                m_Error = "The skeleton '" + Frost::PathToUtf8(file.skeleton.path.filename()) + "' that '" + m_Name +
                          "' is bound to could not be found.";
                return;
            }
            m_SkeletonPath = *skeleton;
            m_PreviewMesh = assets.LoadSkeletalMesh(m_Path);
        }
        else {
            m_SkeletonPath = m_Path;
            m_Sockets.clear();

            // The preview: a skeletal mesh bound to this skeleton, if the project has one.
            m_PreviewMesh.reset();
            const Frost::AssetId id = Frost::AssetFile::ReadSkeleton(m_Path, project).header.id;
            for (const AssetLibrary::Entry& entry : context.GetLibrary().GetEntries()) {
                if (entry.type != Frost::AssetType::SkeletalMesh || entry.duplicate) { continue; }
                try {
                    std::shared_ptr<const Frost::SkeletalMeshAsset> mesh = assets.LoadSkeletalMesh(entry.path);
                    if (mesh->skeletonReference.id == id) {
                        m_PreviewMesh = std::move(mesh);
                        break;
                    }
                }
                catch (const Frost::ImportError&) {
                    // Not a usable preview.
                }
            }
        }
        m_Skeleton = Frost::AssetFile::ReadSkeleton(m_SkeletonPath, project);
    }
    catch (const Frost::ImportError& e) {
        m_Error = e.what();
    }

    // The parts the model comes with, for the preview (those that cannot be loaded are left out).
    m_PreviewParts.clear();
    if (m_PreviewMesh != nullptr) {
        for (const Frost::DefaultAttachment& attachment : m_PreviewMesh->defaultAttachments) {
            try {
                m_PreviewParts.emplace_back(assets.LoadMesh(attachment.mesh), attachment.bone);
            }
            catch (const Frost::ImportError&) {
                // Missing: not shown.
            }
        }
    }
}

std::string SkeletalAssetEditor::Save(EditorContext& context) {
    const std::filesystem::path project = context.GetLibrary().GetRegistry().GetProjectDirectory();
    try {
        if (m_SkeletonDirty) {
            if (const std::optional<std::string> problem = m_Skeleton.skeleton.Validate()) {
                return "The skeleton cannot be saved: " + *problem + ".";
            }
            Frost::SkeletonFile file = m_Skeleton;
            file.header.version = Frost::AssetFile::kSkeletonVersion;
            Frost::AssetFile::WriteSkeleton(m_SkeletonPath, file, project);
        }
        if (m_SocketsDirty && IsSkeletalMesh()) {
            Frost::SkeletalMeshFile file = Frost::AssetFile::ReadSkeletalMesh(m_Path, project);
            file.header.version = Frost::AssetFile::kSkeletalMeshVersion;
            file.sockets = m_Sockets;
            Frost::AssetFile::WriteSkeletalMesh(m_Path, file, project);
        }
    }
    catch (const Frost::ImportError& e) {
        return std::string("Could not save: ") + e.what();
    }

    // Everything that uses them (the level, this preview) shows the saved versions.
    const Selection selection = m_Selection;
    context.ReloadAllAssets();
    Load(context);
    m_Selection = selection;
    return {};
}

void SkeletalAssetEditor::Revert(EditorContext& context) { Load(context); }

bool SkeletalAssetEditor::IsNameTaken(const std::string& name) const {
    return m_Skeleton.skeleton.FindBone(name).has_value() ||
           std::any_of(m_Sockets.begin(), m_Sockets.end(), [&](const Frost::SocketData& socket) { return socket.name == name; });
}

std::string SkeletalAssetEditor::MakeUniqueName(const std::string& base) const {
    if (!IsNameTaken(base)) { return base; }
    for (int suffix = 2;; ++suffix) {
        std::string candidate = base + "_" + std::to_string(suffix);
        if (!IsNameTaken(candidate)) { return candidate; }
    }
}

std::size_t SkeletalAssetEditor::AddSocket(const std::string& bone) {
    m_Sockets.push_back(Frost::SocketData{.name = MakeUniqueName(bone.empty() ? "Socket" : bone + "Socket"),
                                          .bone = bone,
                                          .position = glm::vec3(0.0f),
                                          .rotation = glm::vec3(0.0f),
                                          .scale = glm::vec3(1.0f)});
    MarkSocketsDirty();
    m_Selection = {.kind = Selection::Kind::Socket, .index = m_Sockets.size() - 1};
    return m_Sockets.size() - 1;
}

std::size_t SkeletalAssetEditor::AddBone(const std::int32_t parent) {
    std::vector<Frost::Bone>& bones = m_Skeleton.skeleton.bones;
    bones.push_back(Frost::Bone{.name = MakeUniqueName("NewBone"),
                                .parent = parent >= 0 && static_cast<std::size_t>(parent) < bones.size() ? parent : -1,
                                .restPose = {},
                                .addedInEditor = true});
    MarkSkeletonDirty();
    m_Selection = {.kind = Selection::Kind::Bone, .index = bones.size() - 1};
    return bones.size() - 1;
}

void SkeletalAssetEditor::DeleteSocket(const std::size_t index) {
    if (index >= m_Sockets.size()) { return; }
    m_Sockets.erase(m_Sockets.begin() + static_cast<std::ptrdiff_t>(index));
    MarkSocketsDirty();
    m_Selection = {};
}

bool SkeletalAssetEditor::DeleteBone(const std::size_t index, std::string& why) {
    std::vector<Frost::Bone>& bones = m_Skeleton.skeleton.bones;
    if (index >= bones.size()) { return false; }
    const Frost::Bone& bone = bones[index];
    if (!bone.addedInEditor) {
        why = "'" + bone.name + "' was imported with the model; reimport the model without it to remove it.";
        return false;
    }
    if (std::any_of(bones.begin(), bones.end(), [&](const Frost::Bone& other) { return other.parent == static_cast<std::int32_t>(index); })) {
        why = "'" + bone.name + "' has bones under it; delete those first.";
        return false;
    }
    if (std::any_of(m_Sockets.begin(), m_Sockets.end(), [&](const Frost::SocketData& socket) { return socket.bone == bone.name; })) {
        why = "'" + bone.name + "' has sockets on it; delete or move those first.";
        return false;
    }

    bones.erase(bones.begin() + static_cast<std::ptrdiff_t>(index));
    for (Frost::Bone& other : bones) {
        if (other.parent > static_cast<std::int32_t>(index)) { --other.parent; }
    }
    MarkSkeletonDirty();
    m_Selection = {};
    return true;
}

bool SkeletalAssetEditor::RenameBone(const std::size_t index, const std::string& name) {
    std::vector<Frost::Bone>& bones = m_Skeleton.skeleton.bones;
    if (index >= bones.size() || !bones[index].addedInEditor || name.empty()) { return false; }
    if (name == bones[index].name) { return true; }
    if (IsNameTaken(name)) { return false; }

    for (Frost::SocketData& socket : m_Sockets) {
        if (socket.bone == bones[index].name) {
            socket.bone = name;
            MarkSocketsDirty();
        }
    }
    bones[index].name = name;
    MarkSkeletonDirty();
    return true;
}

bool SkeletalAssetEditor::RenameSocket(const std::size_t index, const std::string& name) {
    if (index >= m_Sockets.size() || name.empty()) { return false; }
    if (name == m_Sockets[index].name) { return true; }
    if (IsNameTaken(name)) { return false; }
    m_Sockets[index].name = name;
    MarkSocketsDirty();
    return true;
}

std::vector<glm::mat4> SkeletalAssetEditor::ComputePreviewPose() const {
    const Frost::SkeletonData& skeleton = m_Skeleton.skeleton;
    std::vector<Frost::BoneTransform> pose = skeleton.GetRestPose();
    if (m_PreviewAnimation != nullptr) {
        m_PreviewAnimation->clip.Sample(m_PreviewTime, m_PreviewAnimation->clip.BindTracks(skeleton), pose);
    }
    return skeleton.ComputeModelTransforms(pose);
}

glm::mat4 SkeletalAssetEditor::ComputeSocketMatrix(const Frost::SocketData& socket, const std::vector<glm::mat4>& pose) const {
    glm::mat4 bone(1.0f);
    if (const std::optional<std::size_t> index = m_Skeleton.skeleton.FindBone(socket.bone); index.has_value() && *index < pose.size()) {
        bone = pose[*index];
    }
    return Frost::ComputeSocketMatrix(bone, socket);
}

bool SkeletalAssetEditor::Draw(EditorContext& context) {
    const std::string title = m_Name + (IsSkeletalMesh() ? "  (Skeletal Mesh)" : "  (Skeleton)") + (IsDirty() ? " *" : "") +
                              "###AssetEditor" + m_WindowId;
    ImGui::SetNextWindowSize(ImVec2(1100.0f, 650.0f), ImGuiCond_FirstUseEver);
    if (m_Focus) {
        ImGui::SetNextWindowFocus();
        m_Focus = false;
    }

    bool open = true;
    const bool visible = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse);
    if (!open) {
        if (IsDirty()) { m_ClosePrompt = true; }
        else { m_Open = false; }
    }

    if (visible) {
        if (!m_Error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
            ImGui::TextWrapped("%s", m_Error.c_str());
            ImGui::PopStyleColor();
            if (ImGui::Button("Retry")) { Load(context); }
        }
        else {
            DrawToolbar(context);
            ImGui::Separator();

            // Skeleton tree | preview | details.
            const float treeWidth = std::max(ImGui::GetFontSize() * 16.0f, 200.0f);
            const float detailsWidth = std::max(ImGui::GetFontSize() * 20.0f, 260.0f);
            if (ImGui::BeginChild("##Tree", ImVec2(treeWidth, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) { DrawTree(); }
            ImGui::EndChild();
            ImGui::SameLine();
            const float previewWidth = std::max(ImGui::GetContentRegionAvail().x - detailsWidth - ImGui::GetStyle().ItemSpacing.x, 50.0f);
            if (ImGui::BeginChild("##Preview", ImVec2(previewWidth, 0.0f), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                DrawPreview(context);
            }
            ImGui::EndChild();
            ImGui::SameLine();
            if (ImGui::BeginChild("##Details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) { DrawDetails(); }
            ImGui::EndChild();
        }
    }
    DrawClosePrompt(context);
    ImGui::End();
    return m_Open;
}

void SkeletalAssetEditor::DrawToolbar(EditorContext& context) {
    ImGui::BeginDisabled(!IsDirty());
    if (ImGui::Button("Save")) {
        if (const std::string error = Save(context); !error.empty()) { context.ReportError(error); }
    }
    ImGui::SetItemTooltip("Write the changes%s.", IsSkeletalMesh() ? " (sockets to the skeletal mesh, bones to its skeleton)" : "");
    ImGui::SameLine();
    if (ImGui::Button("Revert")) { Revert(context); }
    ImGui::SetItemTooltip("Drop the changes.");
    ImGui::EndDisabled();

    // Adding: at the selected bone (or the mesh).
    const bool boneSelected = m_Selection.kind == Selection::Kind::Bone && m_Selection.index < m_Skeleton.skeleton.bones.size();
    const std::string selectedBone = boneSelected ? m_Skeleton.skeleton.bones[m_Selection.index].name : std::string();
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    if (IsSkeletalMesh()) {
        ImGui::SameLine();
        if (ImGui::Button("Add Socket")) { static_cast<void>(AddSocket(selectedBone)); }
        ImGui::SetItemTooltip(boneSelected ? "Add a socket on the selected bone." : "Add a socket on the mesh itself (select a bone to add one on it).");
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Bone")) { static_cast<void>(AddBone(boneSelected ? static_cast<std::int32_t>(m_Selection.index) : -1)); }
    ImGui::SetItemTooltip("Add a bone under the selected bone (or a new root bone), to the skeleton '%s'%s.",
                          Frost::PathToUtf8(m_SkeletonPath.stem()).c_str(),
                          IsSkeletalMesh() ? ", shared by every skeletal mesh and animation bound to it" : "");

    // The preview's animation and moment.
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    const std::string animationLabel = m_PreviewAnimation != nullptr ? m_PreviewAnimation->name : std::string("(rest pose)");
    if (ImGui::BeginCombo("##PreviewAnimation", animationLabel.c_str())) {
        if (ImGui::Selectable("(rest pose)", m_PreviewAnimation == nullptr)) {
            m_PreviewAnimation.reset();
            m_PreviewPlaying = false;
        }
        for (const AssetLibrary::Entry& entry : context.GetLibrary().GetEntries()) {
            if (entry.type != Frost::AssetType::Animation || entry.duplicate) { continue; }
            const std::string name = Frost::PathToUtf8(entry.path.stem());
            if (ImGui::Selectable(name.c_str(), m_PreviewAnimation != nullptr && m_PreviewAnimation->id == entry.id)) {
                try {
                    m_PreviewAnimation = context.GetAssets().LoadAnimation(entry.path);
                    m_PreviewTime = 0.0f;
                }
                catch (const Frost::ImportError& e) {
                    context.ReportError(std::string("Could not load the animation: ") + e.what());
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Pose the preview with an animation, to see the sockets follow their bones.");
    if (m_PreviewAnimation != nullptr) {
        const float duration = m_PreviewAnimation->clip.duration;
        if (m_PreviewPlaying && duration > 0.0f) { m_PreviewTime = std::fmod(m_PreviewTime + ImGui::GetIO().DeltaTime, duration); }
        ImGui::SameLine();
        if (ImGui::Button(m_PreviewPlaying ? "Pause" : "Play")) { m_PreviewPlaying = !m_PreviewPlaying; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::SliderFloat("##PreviewTime", &m_PreviewTime, 0.0f, duration, "%.2f s", ImGuiSliderFlags_AlwaysClamp)) {
            m_PreviewPlaying = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Frame")) { m_CameraFramed = false; }
}

void SkeletalAssetEditor::DrawTree() {
    const std::vector<Frost::Bone>& bones = m_Skeleton.skeleton.bones;
    std::vector<std::vector<std::size_t>> children(bones.size());
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].parent >= 0 && static_cast<std::size_t>(bones[i].parent) < bones.size()) {
            children[static_cast<std::size_t>(bones[i].parent)].push_back(i);
        }
    }

    // The mesh itself, with the sockets on it (and those whose bone is missing).
    if (IsSkeletalMesh()) {
        std::vector<std::size_t> onMesh;
        for (std::size_t i = 0; i < m_Sockets.size(); ++i) {
            if (!m_Skeleton.skeleton.FindBone(m_Sockets[i].bone).has_value()) { onMesh.push_back(i); }
        }
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (onMesh.empty()) { flags |= ImGuiTreeNodeFlags_Leaf; }
        if (m_Selection.kind == Selection::Kind::Mesh) { flags |= ImGuiTreeNodeFlags_Selected; }
        const bool open = ImGui::TreeNodeEx("##Mesh", flags, "%s (mesh)", m_Name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { m_Selection = {.kind = Selection::Kind::Mesh, .index = 0}; }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Add Socket")) { static_cast<void>(AddSocket({})); }
            ImGui::EndPopup();
        }
        if (open) {
            for (const std::size_t socket : onMesh) { DrawSocketNode(socket); }
            ImGui::TreePop();
        }
    }

    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].parent < 0 || static_cast<std::size_t>(bones[i].parent) >= bones.size()) { DrawBoneNode(i, children); }
    }
}

void SkeletalAssetEditor::DrawBoneNode(const std::size_t bone, const std::vector<std::vector<std::size_t>>& children) {
    const Frost::Bone& data = m_Skeleton.skeleton.bones[bone];
    std::vector<std::size_t> sockets;
    for (std::size_t i = 0; i < m_Sockets.size(); ++i) {
        if (m_Sockets[i].bone == data.name) { sockets.push_back(i); }
    }

    ImGui::PushID(static_cast<int>(bone));
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (children[bone].empty() && sockets.empty()) { flags |= ImGuiTreeNodeFlags_Leaf; }
    if (m_Selection.kind == Selection::Kind::Bone && m_Selection.index == bone) { flags |= ImGuiTreeNodeFlags_Selected; }
    if (data.addedInEditor) { ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(AddedBoneColor())); }
    const bool open = ImGui::TreeNodeEx("##Bone", flags, "%s", data.name.c_str());
    if (data.addedInEditor) { ImGui::PopStyleColor(); }
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) { m_Selection = {.kind = Selection::Kind::Bone, .index = bone}; }
    if (data.addedInEditor) { ImGui::SetItemTooltip("Added in the editor."); }

    // Deferred: the tree is being walked.
    bool addSocket = false;
    bool addBone = false;
    bool deleteBone = false;
    if (ImGui::BeginPopupContextItem()) {
        m_Selection = {.kind = Selection::Kind::Bone, .index = bone};
        if (IsSkeletalMesh() && ImGui::MenuItem("Add Socket")) { addSocket = true; }
        if (ImGui::MenuItem("Add Bone")) { addBone = true; }
        if (ImGui::MenuItem("Delete Bone", nullptr, false, data.addedInEditor)) { deleteBone = true; }
        ImGui::EndPopup();
    }
    const std::string name = data.name;

    if (open) {
        for (const std::size_t socket : sockets) { DrawSocketNode(socket); }
        for (const std::size_t child : children[bone]) { DrawBoneNode(child, children); }
        ImGui::TreePop();
    }
    ImGui::PopID();

    if (addSocket) { static_cast<void>(AddSocket(name)); }
    if (addBone) { static_cast<void>(AddBone(static_cast<std::int32_t>(bone))); }
    if (deleteBone) {
        std::string why;
        if (!DeleteBone(bone, why)) { m_NameProblem = why; }
    }
}

void SkeletalAssetEditor::DrawSocketNode(const std::size_t socket) {
    ImGui::PushID(static_cast<int>(socket) + 1'000'000);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (m_Selection.kind == Selection::Kind::Socket && m_Selection.index == socket) { flags |= ImGuiTreeNodeFlags_Selected; }
    const bool missingBone = !m_Sockets[socket].bone.empty() && !m_Skeleton.skeleton.FindBone(m_Sockets[socket].bone).has_value();
    ImGui::PushStyleColor(ImGuiCol_Text, missingBone ? kErrorColor : ImGui::ColorConvertU32ToFloat4(SocketColor()));
    ImGui::TreeNodeEx("##Socket", flags, "[socket] %s", m_Sockets[socket].name.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemClicked()) { m_Selection = {.kind = Selection::Kind::Socket, .index = socket}; }
    if (missingBone) {
        ImGui::SetItemTooltip("Its bone '%s' is not in the skeleton: it sits on the mesh itself.", m_Sockets[socket].bone.c_str());
    }
    bool remove = false;
    if (ImGui::BeginPopupContextItem()) {
        m_Selection = {.kind = Selection::Kind::Socket, .index = socket};
        if (ImGui::MenuItem("Delete Socket")) { remove = true; }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    if (remove) { DeleteSocket(socket); }
}

void SkeletalAssetEditor::DrawPreview(EditorContext& /*context*/) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 1.0f), std::max(ImGui::GetContentRegionAvail().y, 1.0f));
    ImDrawList* const drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), EditorUI::DisplayColor(38, 38, 52));

    // The pose, and the model in it: the skeletal mesh, and the parts it comes with (see
    // Frost::DefaultAttachment) where they are attached.
    const std::vector<glm::mat4> pose = ComputePreviewPose();
    struct PreviewModel {
        const Frost::MeshAsset* mesh = nullptr;
        std::vector<glm::vec3> positions;  // Model space, posed.
    };
    std::vector<PreviewModel> models;
    const Frost::MeshAsset* const drawable = m_PreviewMesh != nullptr ? m_PreviewMesh->drawable.get() : nullptr;
    if (drawable != nullptr) {
        // The model's palette comes from its own (loaded) skeleton's bones; take their poses by name.
        std::vector<glm::mat4> modelPose;
        if (m_PreviewMesh->skeleton != nullptr) {
            const Frost::SkeletonData& loaded = m_PreviewMesh->skeleton->skeleton;
            modelPose.resize(loaded.bones.size());
            for (std::size_t i = 0; i < loaded.bones.size(); ++i) {
                const std::optional<std::size_t> index = m_Skeleton.skeleton.FindBone(loaded.bones[i].name);
                modelPose[i] = index.has_value() && *index < pose.size() ? pose[*index] : m_PreviewMesh->skeleton->restTransforms[i];
            }
        }
        std::vector<glm::mat4> palette;
        m_PreviewMesh->ComputePalette(modelPose, true, palette);
        PreviewModel body{.mesh = drawable, .positions = {}};
        Frost::SkinPositions(m_PreviewMesh->data.mesh.vertices, m_PreviewMesh->paletteSkin, palette, body.positions);
        models.push_back(std::move(body));

        const std::vector<glm::mat4> rest = m_Skeleton.skeleton.ComputeModelTransforms(m_Skeleton.skeleton.GetRestPose());
        for (const auto& [part, bone] : m_PreviewParts) {
            glm::mat4 motion(1.0f);
            if (const std::optional<std::size_t> index = m_Skeleton.skeleton.FindBone(bone); index.has_value() && *index < pose.size()) {
                motion = pose[*index] * glm::inverse(rest[*index]);
            }
            PreviewModel model{.mesh = part.get(), .positions = {}};
            model.positions.reserve(part->positions.size());
            for (const glm::vec3& position : part->positions) { model.positions.push_back(glm::vec3(motion * glm::vec4(position, 1.0f))); }
            models.push_back(std::move(model));
        }
    }

    // Framing: around the model (or the skeleton) at rest.
    if (!m_CameraFramed) {
        glm::vec3 low(std::numeric_limits<float>::max());
        glm::vec3 high(std::numeric_limits<float>::lowest());
        const auto include = [&](const glm::vec3& point) {
            low = glm::min(low, point);
            high = glm::max(high, point);
        };
        for (const PreviewModel& model : models) {
            for (const glm::vec3& position : model.positions) { include(position); }
        }
        for (const glm::mat4& bone : pose) { include(glm::vec3(bone[3])); }
        if (low.x > high.x) {
            low = glm::vec3(-50.0f);
            high = glm::vec3(50.0f);
        }
        m_Target = (low + high) * 0.5f;
        m_Radius = std::max(glm::length(high - low) * 0.5f, 1.0f);
        m_Distance = m_Radius / std::tan(glm::radians(kFieldOfView) * 0.5f) * 1.2f;
        m_CameraFramed = true;
    }

    // Orbit by dragging, zoom with the wheel; a click (no drag) selects the joint or socket under it.
    ImGui::InvisibleButton("##PreviewCanvas", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        m_Yaw -= delta.x * 0.4f;
        m_Pitch = std::clamp(m_Pitch - delta.y * 0.4f, -89.0f, 89.0f);
    }
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
        m_Distance = std::clamp(m_Distance * std::pow(0.88f, ImGui::GetIO().MouseWheel), m_Radius * 0.05f, m_Radius * 50.0f);
    }
    const bool clicked = ImGui::IsItemDeactivated() && Distance(ImGui::GetIO().MouseClickedPos[0], ImGui::GetIO().MousePos) < 3.0f;

    const float pitch = glm::radians(m_Pitch);
    const float yaw = glm::radians(m_Yaw);
    const glm::vec3 forward(-std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw));
    const glm::vec3 eye = m_Target - forward * m_Distance;
    const float nearPlane = std::max(m_Distance * 0.01f, 0.01f);
    PreviewProjection projection{.view = glm::lookAt(eye, m_Target, glm::vec3(0.0f, 1.0f, 0.0f)),
                                 .projection = glm::perspective(glm::radians(kFieldOfView), size.x / size.y, nearPlane,
                                                                m_Distance + m_Radius * 10.0f),
                                 .origin = origin,
                                 .size = size};

    drawList->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    // The models: flat-shaded triangles, far to near.
    if (!models.empty()) {
        struct Triangle {
            float depth;
            ImVec2 a, b, c;
            ImU32 color;
        };
        std::vector<Triangle> triangles;

        std::size_t total = 0;
        for (const PreviewModel& model : models) {
            for (const Frost::SubMesh& subMesh : model.mesh->mesh->GetSubMeshes()) { total += subMesh.indexCount / 3; }
        }
        const std::size_t stride = std::max<std::size_t>(1, (total + kMaxPreviewTriangles - 1) / kMaxPreviewTriangles);

        std::array<ImU32, 64> shades{};
        for (std::size_t i = 0; i < shades.size(); ++i) {
            const float shade = static_cast<float>(i) / static_cast<float>(shades.size() - 1);
            shades[i] = EditorUI::DisplayColor(static_cast<int>(175.0f * shade), static_cast<int>(178.0f * shade),
                                               static_cast<int>(190.0f * shade));
        }

        const glm::vec3 light = glm::normalize(glm::vec3(-0.4f, 0.6f, 1.0f));  // In view space, towards the light.
        std::size_t counter = 0;
        for (const PreviewModel& model : models) {
            const std::vector<glm::vec3>& positions = model.positions;
            if (positions.size() != model.mesh->positions.size()) { continue; }
            std::vector<glm::vec3> viewPositions(positions.size());
            for (std::size_t i = 0; i < positions.size(); ++i) { viewPositions[i] = glm::vec3(projection.view * glm::vec4(positions[i], 1.0f)); }

            for (const Frost::SubMesh& subMesh : model.mesh->mesh->GetSubMeshes()) {
                for (std::uint32_t i = 0; i + 2 < subMesh.indexCount; i += 3) {
                    if (counter++ % stride != 0) { continue; }
                    std::size_t corner[3];
                    for (std::uint32_t k = 0; k < 3; ++k) {
                        corner[k] = std::size_t{model.mesh->indices[subMesh.firstIndex + i + k]} + static_cast<std::size_t>(subMesh.vertexOffset);
                    }
                    if (corner[0] >= viewPositions.size() || corner[1] >= viewPositions.size() || corner[2] >= viewPositions.size()) {
                        continue;
                    }
                    const glm::vec3& a = viewPositions[corner[0]];
                    const glm::vec3& b = viewPositions[corner[1]];
                    const glm::vec3& c = viewPositions[corner[2]];
                    if (a.z > -nearPlane || b.z > -nearPlane || c.z > -nearPlane) { continue; }

                    const glm::vec3 normal = glm::cross(b - a, c - a);
                    const glm::vec3 centroid = (a + b + c) / 3.0f;
                    const float normalLength = glm::length(normal);
                    if (!(normalLength > 0.0f)) { continue; }
                    // Lit from over the viewer's left shoulder (both faces, whichever way the mesh winds).
                    const float facing = std::abs(glm::dot(normal / normalLength, light));
                    const float shade = 0.25f + 0.75f * facing;

                    const std::optional<ImVec2> sa = projection.ToScreen(positions[corner[0]]);
                    const std::optional<ImVec2> sb = projection.ToScreen(positions[corner[1]]);
                    const std::optional<ImVec2> sc = projection.ToScreen(positions[corner[2]]);
                    if (!sa || !sb || !sc) { continue; }
                    // (Shaded in steps, so colors can be converted once.)
                    const auto step = static_cast<std::size_t>(std::clamp(shade, 0.0f, 1.0f) * static_cast<float>(shades.size() - 1) + 0.5f);
                    triangles.push_back(Triangle{.depth = centroid.z, .a = *sa, .b = *sb, .c = *sc, .color = shades[step]});
                }
            }
        }
        std::sort(triangles.begin(), triangles.end(), [](const Triangle& x, const Triangle& y) { return x.depth < y.depth; });

        const ImDrawListFlags flags = drawList->Flags;
        drawList->Flags &= ~ImDrawListFlags_AntiAliasedFill;  // No fringes between neighboring triangles.
        for (const Triangle& triangle : triangles) { drawList->AddTriangleFilled(triangle.a, triangle.b, triangle.c, triangle.color); }
        drawList->Flags = flags;
    }
    else if (m_PreviewMesh == nullptr) {
        drawList->AddText(ImVec2(origin.x + 8.0f, origin.y + 8.0f), EditorUI::DisplayColor(170, 170, 170),
                          "No skeletal mesh bound to this skeleton to preview: showing its bones.");
    }

    // The skeleton over it, then the sockets.
    std::optional<std::pair<Selection, float>> picked;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto consider = [&](const Selection& selection, const ImVec2& point) {
        const float distance = Distance(point, mouse);
        if (distance <= kPickRadius && (!picked.has_value() || distance < picked->second)) { picked = std::make_pair(selection, distance); }
    };

    const std::vector<Frost::Bone>& bones = m_Skeleton.skeleton.bones;
    for (std::size_t i = 0; i < bones.size() && i < pose.size(); ++i) {
        const bool selected = m_Selection.kind == Selection::Kind::Bone && m_Selection.index == i;
        const ImU32 color = selected ? SelectedColor() : bones[i].addedInEditor ? AddedBoneColor() : BoneColor();
        const std::optional<ImVec2> joint = projection.ToScreen(glm::vec3(pose[i][3]));
        if (bones[i].parent >= 0 && static_cast<std::size_t>(bones[i].parent) < pose.size()) {
            const std::optional<ImVec2> parent = projection.ToScreen(glm::vec3(pose[static_cast<std::size_t>(bones[i].parent)][3]));
            if (joint && parent) { drawList->AddLine(*parent, *joint, color, selected ? 3.0f : 1.5f); }
        }
        if (joint) {
            drawList->AddCircleFilled(*joint, selected ? 5.0f : 3.5f, color);
            consider({.kind = Selection::Kind::Bone, .index = i}, *joint);
            if (selected) { drawList->AddText(ImVec2(joint->x + 7.0f, joint->y - 7.0f), color, bones[i].name.c_str()); }
        }
    }

    const float axisLength = m_Radius * 0.08f;
    for (std::size_t i = 0; i < m_Sockets.size(); ++i) {
        const bool selected = m_Selection.kind == Selection::Kind::Socket && m_Selection.index == i;
        const glm::mat4 socket = ComputeSocketMatrix(m_Sockets[i], pose);
        const glm::vec3 position(socket[3]);
        const std::optional<ImVec2> center = projection.ToScreen(position);
        if (!center) { continue; }
        const ImU32 axes[3] = {EditorUI::DisplayColor(240, 70, 70), EditorUI::DisplayColor(70, 230, 70), EditorUI::DisplayColor(80, 120, 255)};
        for (int axis = 0; axis < 3; ++axis) {
            const glm::vec3 direction = glm::vec3(socket[axis]);
            const float length = glm::length(direction);
            if (!(length > 0.0f)) { continue; }
            if (const std::optional<ImVec2> tip = projection.ToScreen(position + direction / length * axisLength)) {
                drawList->AddLine(*center, *tip, axes[axis], selected ? 3.0f : 2.0f);
            }
        }
        const float r = selected ? 6.0f : 4.5f;
        drawList->AddQuadFilled(ImVec2(center->x, center->y - r), ImVec2(center->x + r, center->y), ImVec2(center->x, center->y + r),
                                ImVec2(center->x - r, center->y), selected ? SelectedColor() : SocketColor());
        drawList->AddText(ImVec2(center->x + 8.0f, center->y + 2.0f), selected ? SelectedColor() : SocketColor(), m_Sockets[i].name.c_str());
        consider({.kind = Selection::Kind::Socket, .index = i}, *center);
    }
    drawList->PopClipRect();

    if (clicked && hovered) { m_Selection = picked.has_value() ? picked->first : Selection{}; }

    ImGui::SetCursorScreenPos(ImVec2(origin.x + 6.0f, origin.y + size.y - ImGui::GetTextLineHeightWithSpacing()));
    ImGui::TextDisabled("Drag to orbit, wheel to zoom, click a joint or socket to select it.");
}

void SkeletalAssetEditor::DrawDetails() {
    if (!m_NameProblem.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
        ImGui::TextWrapped("%s", m_NameProblem.c_str());
        ImGui::PopStyleColor();
    }

    // The name field: edited in a buffer, applied when done.
    const auto nameField = [&](const std::string& current, const bool editable, const auto& rename) {
        if (m_NameBufferFor.kind != m_Selection.kind || m_NameBufferFor.index != m_Selection.index) {
            m_NameBuffer = current;
            m_NameBufferFor = m_Selection;
        }
        ImGui::BeginDisabled(!editable);
        EditorUI::InputText("Name", m_NameBuffer);
        ImGui::EndDisabled();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (rename(m_NameBuffer)) { m_NameProblem.clear(); }
            else {
                m_NameProblem = "'" + m_NameBuffer + "' cannot be used: names must be unique among bones and sockets, and not empty.";
                m_NameBuffer = current;
            }
        }
    };

    switch (m_Selection.kind) {
        case Selection::Kind::None:
            ImGui::TextWrapped("Select a bone or socket (in the tree, or click it in the preview).");
            ImGui::Spacing();
            ImGui::TextDisabled("%zu bones, %zu sockets", m_Skeleton.skeleton.bones.size(), m_Sockets.size());
            if (!IsSkeletalMesh()) {
                ImGui::Spacing();
                ImGui::TextWrapped("Sockets belong to skeletal meshes: open one (double-click it in the Asset Library) to add them.");
            }
            return;

        case Selection::Kind::Mesh:
            ImGui::TextUnformatted(m_Name.c_str());
            ImGui::TextWrapped("Sockets on the mesh itself move with the object (not with a bone).");
            if (ImGui::Button("Add Socket")) { static_cast<void>(AddSocket({})); }
            return;

        case Selection::Kind::Bone: {
            std::vector<Frost::Bone>& bones = m_Skeleton.skeleton.bones;
            if (m_Selection.index >= bones.size()) {
                m_Selection = {};
                return;
            }
            const std::size_t index = m_Selection.index;
            ImGui::SeparatorText(bones[index].addedInEditor ? "Bone (added)" : "Bone");
            nameField(bones[index].name, bones[index].addedInEditor, [&](const std::string& name) { return RenameBone(index, name); });
            const std::int32_t parent = bones[index].parent;
            ImGui::TextDisabled("Parent: %s", parent >= 0 ? bones[static_cast<std::size_t>(parent)].name.c_str() : "(none: a root)");

            ImGui::SeparatorText("Rest pose (in its parent's space)");
            Frost::BoneTransform& rest = bones[index].restPose;
            if (bones[index].addedInEditor) {
                if (ImGui::DragFloat3("Position", &rest.translation.x, 0.1f, 0.0f, 0.0f, "%.2f cm")) { MarkSkeletonDirty(); }
                glm::vec3 euler = ToEulerDegrees(rest.rotation);
                if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f, 0.0f, 0.0f, "%.1f deg")) {
                    rest.rotation = FromEulerDegrees(euler);
                    MarkSkeletonDirty();
                }
                if (ImGui::DragFloat3("Scale", &rest.scale.x, 0.01f, 0.001f, 1000.0f, "%.3f")) { MarkSkeletonDirty(); }
            }
            else {
                const glm::vec3 euler = ToEulerDegrees(rest.rotation);
                ImGui::TextDisabled("Position  %.2f, %.2f, %.2f cm", static_cast<double>(rest.translation.x),
                                    static_cast<double>(rest.translation.y), static_cast<double>(rest.translation.z));
                ImGui::TextDisabled("Rotation  %.1f, %.1f, %.1f deg", static_cast<double>(euler.x), static_cast<double>(euler.y),
                                    static_cast<double>(euler.z));
                ImGui::TextDisabled("Scale     %.3f, %.3f, %.3f", static_cast<double>(rest.scale.x), static_cast<double>(rest.scale.y),
                                    static_cast<double>(rest.scale.z));
                ImGui::TextWrapped("Imported with the model: the mesh is skinned to it as it is. Reimport the model to change it.");
            }
            if (parent >= 0) {
                ImGui::TextDisabled("(Its parent's scale applies: a rig scaled in its armature has its bones' offsets scaled too.)");
            }

            ImGui::Separator();
            const std::string name = bones[index].name;
            if (IsSkeletalMesh() && ImGui::Button("Add Socket Here")) { static_cast<void>(AddSocket(name)); }
            if (IsSkeletalMesh()) { ImGui::SameLine(); }
            if (ImGui::Button("Add Child Bone")) { static_cast<void>(AddBone(static_cast<std::int32_t>(index))); }
            if (bones.size() > index && bones[index].addedInEditor) {
                if (ImGui::Button("Delete Bone")) {
                    std::string why;
                    if (!DeleteBone(index, why)) { m_NameProblem = why; }
                }
            }
            return;
        }

        case Selection::Kind::Socket: {
            if (m_Selection.index >= m_Sockets.size()) {
                m_Selection = {};
                return;
            }
            const std::size_t index = m_Selection.index;
            ImGui::SeparatorText("Socket");
            nameField(m_Sockets[index].name, true, [&](const std::string& name) { return RenameSocket(index, name); });

            Frost::SocketData& socket = m_Sockets[index];
            const std::string boneLabel = socket.bone.empty() ? std::string("(the mesh itself)") : socket.bone;
            if (ImGui::BeginCombo("Bone", boneLabel.c_str(), ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("(the mesh itself)", socket.bone.empty())) {
                    socket.bone.clear();
                    MarkSocketsDirty();
                }
                for (const Frost::Bone& bone : m_Skeleton.skeleton.bones) {
                    if (ImGui::Selectable(bone.name.c_str(), bone.name == socket.bone)) {
                        socket.bone = bone.name;
                        MarkSocketsDirty();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("What it moves with: a bone, or the mesh itself.");

            ImGui::SeparatorText(socket.bone.empty() ? "Offset (from the mesh's origin)" : "Offset (from the bone)");
            if (ImGui::DragFloat3("Position", &socket.position.x, 0.1f, 0.0f, 0.0f, "%.2f cm")) { MarkSocketsDirty(); }
            if (ImGui::DragFloat3("Rotation", &socket.rotation.x, 0.5f, 0.0f, 0.0f, "%.1f deg")) { MarkSocketsDirty(); }
            if (ImGui::DragFloat3("Scale", &socket.scale.x, 0.01f, 0.001f, 1000.0f, "%.3f")) { MarkSocketsDirty(); }
            if (ImGui::SmallButton("Reset")) {
                socket.position = glm::vec3(0.0f);
                socket.rotation = glm::vec3(0.0f);
                socket.scale = glm::vec3(1.0f);
                MarkSocketsDirty();
            }
            ImGui::Separator();
            ImGui::TextWrapped("Objects attached to this socket (Attachment > Bone in the Inspector) sit at it, their transform "
                               "relative to it.");
            if (ImGui::Button("Delete Socket")) { DeleteSocket(index); }
            return;
        }
    }
}

void SkeletalAssetEditor::DrawClosePrompt(EditorContext& context) {
    if (m_ClosePrompt) {
        ImGui::OpenPopup("Unsaved Changes##AssetEditor");
        m_ClosePrompt = false;
    }
    if (ImGui::BeginPopupModal("Unsaved Changes##AssetEditor", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save the changes to '%s' before closing?", m_Name.c_str());
        if (ImGui::Button("Save")) {
            if (const std::string error = Save(context); !error.empty()) { context.ReportError(error); }
            else { m_Open = false; }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save")) {
            m_Open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}
