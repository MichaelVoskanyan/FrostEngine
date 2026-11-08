#include <Frost/Assets/AssetManager.hpp>

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetImporter.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Renderer/MaterialSystem.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Renderer/Renderer.hpp>
#include <Frost/Renderer/Texture.hpp>
#include <Frost/Renderer/Vertex.hpp>

#include <glm/common.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

    // Why `path` cannot be loaded as an asset of `type`, phrased for the common mistake of
    // pointing at a source file that has not been imported.
    [[nodiscard]]
    std::string WrongFileMessage(const std::filesystem::path& path, const Frost::AssetType type) {
        const std::string expected = Frost::AssetImporter::GetAssetExtension(type);
        if (Frost::AssetImporter::GetSourceType(path).has_value()) {
            return "'" + Frost::PathToUtf8(path) + "' is a source file, not a Frost asset; import it into the project " +
                   "first (it becomes a " + expected + ").";
        }
        return "'" + Frost::PathToUtf8(path) + "' is not a " + expected + " asset.";
    }

    // Base color and emissive hold color (sampled as sRGB); the other slots hold data.
    constexpr std::array<bool, Frost::MaterialTextures::kSlotCount> kColorSlots = {true, false, false, false, true};

    // Uploads geometry. MeshData can still be rejected by the upload (e.g. an index range past the
    // end); that is a problem with the file, so it is reported as one.
    [[nodiscard]]
    std::shared_ptr<Frost::Mesh> UploadOrThrow(const Frost::Renderer& renderer, const Frost::MeshData& geometry,
                                               const std::filesystem::path& path) {
        try {
            return renderer.CreateMesh(geometry);
        }
        catch (const std::invalid_argument& e) {
            throw Frost::ImportError("'" + Frost::PathToUtf8(path) + "' could not be uploaded: " + e.what());
        }
        catch (const std::runtime_error& e) {
            // CreateMesh reports inconsistent material/texture references this way.
            throw Frost::ImportError("'" + Frost::PathToUtf8(path) + "' is inconsistent: " + e.what());
        }
    }

    // The CPU copy of a mesh asset's geometry: positions, indices and bounds.
    void FillCpuGeometry(Frost::MeshAsset& asset, const Frost::MeshData& geometry) {
        asset.positions.clear();
        asset.positions.reserve(geometry.vertices.size());
        for (const Frost::Vertex& vertex : geometry.vertices) { asset.positions.push_back(vertex.position); }
        asset.indices = geometry.indices;

        asset.bounds = Frost::Bounds{};
        if (!asset.positions.empty()) {
            asset.bounds.min = asset.bounds.max = asset.positions.front();
            for (const glm::vec3& position : asset.positions) {
                asset.bounds.min = glm::min(asset.bounds.min, position);
                asset.bounds.max = glm::max(asset.bounds.max, position);
            }
        }
    }

    // Per sub-mesh, the material its materialIndex names (null for none or an invalid index).
    [[nodiscard]]
    std::vector<std::shared_ptr<Frost::Material>> MaterialsBySubMesh(
            const std::vector<Frost::SubMesh>& subMeshes, const std::vector<std::shared_ptr<Frost::Material>>& materials) {
        std::vector<std::shared_ptr<Frost::Material>> result(subMeshes.size());
        for (std::size_t i = 0; i < subMeshes.size(); ++i) {
            const std::int32_t index = subMeshes[i].materialIndex;
            if (index >= 0 && static_cast<std::size_t>(index) < materials.size()) {
                result[i] = materials[static_cast<std::size_t>(index)];
            }
        }
        return result;
    }

}  // namespace

namespace Frost {

    AssetManager::AssetManager(Renderer& renderer, const std::filesystem::path& contentDirectory)
        : m_Renderer(renderer), m_Registry(contentDirectory) {}

    AssetManager::~AssetManager() = default;

    AssetRegistry& AssetManager::GetRegistry() noexcept { return m_Registry; }

    const AssetRegistry& AssetManager::GetRegistry() const noexcept { return m_Registry; }

    std::shared_ptr<const MeshAsset> AssetManager::LoadMesh(const AssetReference& reference) {
        if (const auto cached = m_Meshes.find(CacheKey(reference.id, reference.path)); cached != m_Meshes.end()) {
            return cached->second;
        }

        const std::filesystem::path path = ResolveOrThrow(reference, AssetType::Mesh);
        const MeshFile file = AssetFile::ReadMesh(path, m_Registry.GetProjectDirectory());

        // Resolved through a path: the file's own id is the cache key.
        const std::string key = CacheKey(file.header.id, path);
        if (const auto cached = m_Meshes.find(key); cached != m_Meshes.end()) { return cached->second; }

        std::shared_ptr<MeshAsset> asset;
        if (file.header.version >= 2) {
            // Materials refer to texture assets; the sub-meshes' material indices to the materials.
            int missingTextures = 0;
            const std::vector<std::shared_ptr<Material>> materials =
                    CreateMaterials(file.materials, PathToUtf8(path.stem()), missingTextures);
            asset = CreateMeshAsset(file.header.id, path, file.mesh, MaterialsBySubMesh(file.mesh.subMeshes, materials));
            asset->missingTextures = missingTextures;
        }
        else {
            // Version-1 files carry their materials and textures in the MeshData, which CreateMesh
            // uploads as they are.
            asset = std::make_shared<MeshAsset>();
            asset->id = file.header.id;
            asset->path = path;
            asset->name = PathToUtf8(path.stem());
            asset->mesh = UploadOrThrow(m_Renderer, file.mesh, path);
            FillCpuGeometry(*asset, file.mesh);
        }

        m_Meshes.emplace(key, asset);
        return asset;
    }

    std::shared_ptr<const MeshAsset> AssetManager::LoadMesh(const std::filesystem::path& path) {
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, m_Registry.GetProjectDirectory());
        return LoadMesh(AssetReference{.id = header.has_value() ? header->id : AssetId{}, .path = NormalizePath(path)});
    }

    std::shared_ptr<const SkeletonAsset> AssetManager::LoadSkeleton(const AssetReference& reference) {
        if (const auto cached = m_Skeletons.find(CacheKey(reference.id, reference.path)); cached != m_Skeletons.end()) {
            return cached->second;
        }

        const std::filesystem::path path = ResolveOrThrow(reference, AssetType::Skeleton);
        SkeletonFile file = AssetFile::ReadSkeleton(path, m_Registry.GetProjectDirectory());

        const std::string key = CacheKey(file.header.id, path);
        if (const auto cached = m_Skeletons.find(key); cached != m_Skeletons.end()) { return cached->second; }

        auto asset = std::make_shared<SkeletonAsset>();
        asset->id = file.header.id;
        asset->path = path;
        asset->name = PathToUtf8(path.stem());
        asset->skeleton = std::move(file.skeleton);
        asset->restTransforms = asset->skeleton.ComputeModelTransforms(asset->skeleton.GetRestPose());

        m_Skeletons.emplace(key, asset);
        return asset;
    }

    std::shared_ptr<const SkeletonAsset> AssetManager::LoadSkeleton(const std::filesystem::path& path) {
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, m_Registry.GetProjectDirectory());
        return LoadSkeleton(AssetReference{.id = header.has_value() ? header->id : AssetId{}, .path = NormalizePath(path)});
    }

    std::shared_ptr<const AnimationAsset> AssetManager::LoadAnimation(const AssetReference& reference) {
        if (const auto cached = m_Animations.find(CacheKey(reference.id, reference.path)); cached != m_Animations.end()) {
            return cached->second;
        }

        const std::filesystem::path path = ResolveOrThrow(reference, AssetType::Animation);
        AnimationFile file = AssetFile::ReadAnimation(path, m_Registry.GetProjectDirectory());

        const std::string key = CacheKey(file.header.id, path);
        if (const auto cached = m_Animations.find(key); cached != m_Animations.end()) { return cached->second; }

        auto asset = std::make_shared<AnimationAsset>();
        asset->id = file.header.id;
        asset->path = path;
        asset->name = PathToUtf8(path.stem());
        asset->skeleton = std::move(file.skeleton);
        asset->clip = std::move(file.clip);

        m_Animations.emplace(key, asset);
        return asset;
    }

    std::shared_ptr<const AnimationAsset> AssetManager::LoadAnimation(const std::filesystem::path& path) {
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, m_Registry.GetProjectDirectory());
        return LoadAnimation(AssetReference{.id = header.has_value() ? header->id : AssetId{}, .path = NormalizePath(path)});
    }

    std::shared_ptr<const SkeletalMeshAsset> AssetManager::LoadSkeletalMesh(const AssetReference& reference) {
        if (const auto cached = m_SkeletalMeshes.find(CacheKey(reference.id, reference.path)); cached != m_SkeletalMeshes.end()) {
            return cached->second;
        }

        const std::filesystem::path path = ResolveOrThrow(reference, AssetType::SkeletalMesh);
        SkeletalMeshFile file = AssetFile::ReadSkeletalMesh(path, m_Registry.GetProjectDirectory());

        const std::string key = CacheKey(file.header.id, path);
        if (const auto cached = m_SkeletalMeshes.find(key); cached != m_SkeletalMeshes.end()) { return cached->second; }

        auto asset = std::make_shared<SkeletalMeshAsset>();
        asset->id = file.header.id;
        asset->path = path;
        asset->name = PathToUtf8(path.stem());
        asset->skeletonReference = file.skeleton;
        asset->defaultAttachments = std::move(file.defaultAttachments);
        asset->sockets = std::move(file.sockets);

        // Without its skeleton the mesh can still be shown, as modeled (the bind pose).
        if (file.skeleton.IsSet()) {
            try {
                asset->skeleton = LoadSkeleton(file.skeleton);
            }
            catch (const ImportError& e) {
                std::cerr << "[Frost::AssetManager] '" << asset->name << "': its skeleton could not be loaded; showing it "
                          << "as modeled, unposed. " << e.what() << '\n';
            }
        }

        int missingTextures = 0;
        const std::vector<std::shared_ptr<Material>> materials = CreateMaterials(file.materials, asset->name, missingTextures);
        asset->materials = MaterialsBySubMesh(file.mesh.mesh.subMeshes, materials);
        asset->missingTextures = missingTextures;

        asset->data = std::move(file.mesh);
        MeshData& mesh = asset->data.mesh;
        for (SubMesh& subMesh : mesh.subMeshes) { subMesh.materialIndex = -1; }

        // Bones by name (see BindSkin).
        if (asset->skeleton != nullptr) {
            asset->bindings = BindSkin(asset->data.bindBones, asset->skeleton->skeleton);
            asset->missingBones = static_cast<int>(std::count(asset->bindings.begin(), asset->bindings.end(), -1));
            if (asset->missingBones > 0) {
                std::cerr << "[Frost::AssetManager] '" << asset->name << "': " << asset->missingBones << " bone(s) it is "
                          << "bound to are not in skeleton '" << asset->skeleton->name << "'; what they move stays "
                          << "as modeled.\n";
            }
        }
        else {
            asset->bindings.assign(asset->data.bindBones.size(), -1);
        }

        // The palette: the bind table, then a slot per attached sub-mesh, whose vertices are fully
        // weighted to it (they have no skin weights of their own).
        asset->paletteSize = asset->data.bindBones.size();
        asset->paletteSkin = asset->data.skin;
        const std::size_t subMeshCount = mesh.subMeshes.size();
        asset->attachmentSlots.assign(subMeshCount, -1);
        for (std::size_t i = 0; i < subMeshCount && i < asset->data.attachments.size(); ++i) {
            if (asset->data.attachments[i] < 0) { continue; }
            if (asset->paletteSize >= std::size_t{65536}) {
                throw ImportError("'" + PathToUtf8(path) + "' has too many bones and attached parts to draw.");
            }

            const auto slot = static_cast<std::uint16_t>(asset->paletteSize++);
            asset->attachmentSlots[i] = slot;

            const SubMesh& subMesh = mesh.subMeshes[i];
            for (std::uint32_t index = subMesh.firstIndex; index < subMesh.firstIndex + subMesh.indexCount; ++index) {
                SkinWeights& weights = asset->paletteSkin[std::size_t{mesh.indices[index]} + static_cast<std::size_t>(subMesh.vertexOffset)];
                weights = SkinWeights{};
                weights.bones[0] = slot;
                weights.weights[0] = 1.0f;
            }
        }

        asset->ComputePalette({}, true, asset->restPalette);

        // The drawable: bind-pose geometry plus the skin stream, skinned on the GPU; its CPU
        // positions (picking, bounds) are the rest pose's.
        std::vector<SkinnedVertex> skin(asset->paletteSkin.size());
        for (std::size_t i = 0; i < skin.size(); ++i) {
            skin[i].joints = asset->paletteSkin[i].bones;
            skin[i].weights = asset->paletteSkin[i].weights;
        }
        std::vector<glm::vec3> restPositions;
        SkinPositions(mesh.vertices, asset->paletteSkin, asset->restPalette, restPositions);
        asset->drawable = CreateMeshAsset(asset->id, asset->path, mesh, asset->materials, skin, restPositions);

        m_SkeletalMeshes.emplace(key, asset);
        return asset;
    }

    std::shared_ptr<const SkeletalMeshAsset> AssetManager::LoadSkeletalMesh(const std::filesystem::path& path) {
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, m_Registry.GetProjectDirectory());
        return LoadSkeletalMesh(AssetReference{.id = header.has_value() ? header->id : AssetId{}, .path = NormalizePath(path)});
    }

    const SocketData* SkeletalMeshAsset::FindSocket(const std::string_view socketName) const noexcept {
        for (const SocketData& socket : sockets) {
            if (socket.name == socketName) { return &socket; }
        }
        return nullptr;
    }

    void SkeletalMeshAsset::ComputePalette(const std::span<const glm::mat4> boneTransforms, const bool attachmentsFollow,
                                           std::vector<glm::mat4>& palette) const {
        palette.assign(paletteSize, glm::mat4(1.0f));
        if (skeleton == nullptr) { return; }  // The bind pose: identity throughout.

        const SkeletonData& bones = skeleton->skeleton;
        if (!boneTransforms.empty() && boneTransforms.size() != bones.bones.size()) {
            throw std::invalid_argument("SkeletalMeshAsset::ComputePalette: one transform per bone of the skeleton is required.");
        }

        const std::vector<glm::mat4>& rest = skeleton->restTransforms;
        const std::span<const glm::mat4> pose = boneTransforms.empty() ? std::span<const glm::mat4>(rest) : boneTransforms;

        const std::vector<glm::mat4> skin = ComputeSkinMatrices(bindings, data.inverseBindMatrices, pose);
        std::copy(skin.begin(), skin.end(), palette.begin());

        // Attached parts: their bone's skinning matrix, in the pose or (not following) at rest.
        const std::vector<glm::mat4> restSkin =
                attachmentsFollow ? std::vector<glm::mat4>{} : ComputeSkinMatrices(bindings, data.inverseBindMatrices, rest);
        for (std::size_t i = 0; i < attachmentSlots.size() && i < data.attachments.size(); ++i) {
            if (attachmentSlots[i] < 0) { continue; }
            const auto entry = static_cast<std::size_t>(data.attachments[i]);
            palette[static_cast<std::size_t>(attachmentSlots[i])] = attachmentsFollow ? skin[entry] : restSkin[entry];
        }
    }

    std::vector<std::shared_ptr<Material>> AssetManager::CreateMaterials(const std::vector<MaterialAssetData>& materials,
                                                                         const std::string& owner, int& missingTextures) {
        std::vector<std::shared_ptr<Material>> result;
        for (const MaterialAssetData& data : materials) {
            std::array<std::shared_ptr<Texture>, MaterialTextures::kSlotCount> slots{};
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                if (!data.textures[slot].IsSet()) { continue; }
                try {
                    slots[slot] = LoadTexture(data.textures[slot], kColorSlots[slot]);
                }
                catch (const ImportError& e) {
                    ++missingTextures;
                    std::cerr << "[Frost::AssetManager] '" << owner << "', material '" << data.name
                              << "': a texture could not be loaded; using the slot's default. " << e.what() << '\n';
                }
            }

            result.push_back(GetMaterialSystem().CreateMaterial(data.parameters, MaterialTextures{.baseColor = slots[0],
                                                                                                  .metallicRoughness = slots[1],
                                                                                                  .normal = slots[2],
                                                                                                  .occlusion = slots[3],
                                                                                                  .emissive = slots[4]}));
        }
        return result;
    }

    std::shared_ptr<MeshAsset> AssetManager::CreateMeshAsset(const AssetId& id, const std::filesystem::path& path,
                                                             const MeshData& geometry,
                                                             const std::vector<std::shared_ptr<Material>>& materials,
                                                             const std::span<const SkinnedVertex> skin,
                                                             const std::span<const glm::vec3> positions) {
        auto asset = std::make_shared<MeshAsset>();
        asset->id = id;
        asset->path = path;
        asset->name = PathToUtf8(path.stem());

        // Uploaded without materials (the sub-meshes' indices refer to the file's list, not to
        // anything the upload knows); they are attached per sub-mesh below.
        MeshData upload;
        upload.vertices = geometry.vertices;
        upload.indices = geometry.indices;
        upload.subMeshes = geometry.subMeshes;
        for (SubMesh& subMesh : upload.subMeshes) { subMesh.materialIndex = -1; }
        if (skin.empty()) { asset->mesh = UploadOrThrow(m_Renderer, upload, path); }
        else {
            try {
                asset->mesh = m_Renderer.CreateMesh(upload, skin);
            }
            catch (const std::invalid_argument& e) {
                throw ImportError("'" + PathToUtf8(path) + "' could not be uploaded: " + e.what());
            }
        }

        const std::size_t subMeshCount = asset->mesh->GetSubMeshes().size();
        for (std::size_t i = 0; i < subMeshCount && i < materials.size(); ++i) {
            if (materials[i] != nullptr) { asset->mesh->SetMaterial(i, materials[i]); }
        }

        FillCpuGeometry(*asset, geometry);
        if (!positions.empty()) {
            asset->positions.assign(positions.begin(), positions.end());
            asset->bounds.min = asset->bounds.max = asset->positions.front();
            for (const glm::vec3& position : asset->positions) {
                asset->bounds.min = glm::min(asset->bounds.min, position);
                asset->bounds.max = glm::max(asset->bounds.max, position);
            }
        }
        return asset;
    }

    std::shared_ptr<const SoundAsset> AssetManager::LoadSound(const AssetReference& reference) {
        if (const auto cached = m_Sounds.find(CacheKey(reference.id, reference.path)); cached != m_Sounds.end()) {
            return cached->second;
        }

        const std::filesystem::path path = ResolveOrThrow(reference, AssetType::Sound);
        SoundFile file = AssetFile::ReadSound(path, m_Registry.GetProjectDirectory());

        const std::string key = CacheKey(file.header.id, path);
        if (const auto cached = m_Sounds.find(key); cached != m_Sounds.end()) { return cached->second; }

        auto asset = std::make_shared<SoundAsset>();
        asset->id = file.header.id;
        asset->path = path;
        asset->name = PathToUtf8(path.stem());
        asset->sound = std::move(file.sound);

        m_Sounds.emplace(key, asset);
        return asset;
    }

    std::shared_ptr<const SoundAsset> AssetManager::LoadSound(const std::filesystem::path& path) {
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, m_Registry.GetProjectDirectory());
        return LoadSound(AssetReference{.id = header.has_value() ? header->id : AssetId{}, .path = NormalizePath(path)});
    }

    std::shared_ptr<Texture> AssetManager::LoadTexture(const AssetReference& reference, const bool srgb) {
        if (const auto cached = m_Textures.find({CacheKey(reference.id, reference.path), srgb}); cached != m_Textures.end()) {
            return cached->second;
        }

        const std::filesystem::path path = ResolveOrThrow(reference, AssetType::Texture);
        TextureFile file = AssetFile::ReadTexture(path, m_Registry.GetProjectDirectory());

        const auto key = std::make_pair(CacheKey(file.header.id, path), srgb);
        if (const auto cached = m_Textures.find(key); cached != m_Textures.end()) { return cached->second; }

        file.settings.srgb = srgb;  // Decided by the slot it is used in, not by the file.
        std::shared_ptr<Texture> texture = m_Renderer.GetMaterialSystem().CreateTexture(file.image, file.settings);

        m_Textures.emplace(key, texture);
        return texture;
    }

    std::shared_ptr<Texture> AssetManager::LoadTexture(const std::filesystem::path& path, const bool srgb) {
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, m_Registry.GetProjectDirectory());
        return LoadTexture(AssetReference{.id = header.has_value() ? header->id : AssetId{}, .path = NormalizePath(path)},
                           srgb);
    }

    void AssetManager::Unload(const AssetReference& reference) {
        // Both the id and the path key, whichever the asset was cached under.
        for (const std::string& key : {CacheKey(reference.id, reference.path), CacheKey({}, reference.path)}) {
            m_Meshes.erase(key);
            m_Textures.erase({key, true});
            m_Textures.erase({key, false});
            m_Skeletons.erase(key);
            m_SkeletalMeshes.erase(key);
            m_Animations.erase(key);
            m_Sounds.erase(key);
        }
    }

    void AssetManager::UnloadTextures() { m_Textures.clear(); }

    void AssetManager::UnloadAll() {
        m_Meshes.clear();
        m_Textures.clear();
        m_Skeletons.clear();
        m_SkeletalMeshes.clear();
        m_Animations.clear();
        m_Sounds.clear();
    }

    const MaterialSystem& AssetManager::GetMaterialSystem() const noexcept { return m_Renderer.GetMaterialSystem(); }

    std::filesystem::path AssetManager::NormalizePath(const std::filesystem::path& path) {
        if (path.empty()) { return {}; }

        std::error_code error;
        std::filesystem::path absolute = std::filesystem::absolute(path, error);
        if (error) { absolute = path; }

        // weakly_canonical resolves what exists and normalizes the rest lexically.
        std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
        if (error) { canonical = absolute.lexically_normal(); }

        return canonical;
    }

    std::optional<AssetType> AssetManager::GetAssetType(const std::filesystem::path& path) {
        std::string extension = PathToUtf8(path.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });

        for (const AssetType type : kAssetTypes) {
            if (extension == AssetFile::GetExtension(type)) { return type; }
        }
        return std::nullopt;
    }

    std::string AssetManager::CacheKey(const AssetId& id, const std::filesystem::path& path) {
        return id.IsValid() ? "id:" + id.ToString() : "path:" + PathToUtf8(NormalizePath(path));
    }

    std::filesystem::path AssetManager::ResolveOrThrow(const AssetReference& reference, const AssetType type) const {
        if (!reference.path.empty() && GetAssetType(reference.path) != type && !reference.id.IsValid()) {
            throw ImportError(WrongFileMessage(reference.path, type));
        }

        const std::optional<std::filesystem::path> path = m_Registry.Resolve(reference);
        if (!path.has_value() && !reference.path.empty() && !m_Registry.GetRoot().empty() &&
            !m_Registry.Contains(reference.path)) {
            throw ImportError("'" + PathToUtf8(reference.path) + "' is not part of this project (it is outside its " +
                              "content directory, '" + PathToUtf8(m_Registry.GetRoot()) + "'). A project only " +
                              "uses its own assets: copy the asset into the content directory to use it here.");
        }
        if (!path.has_value()) {
            std::string message = std::string("The ") + GetAssetTypeName(type) + " asset ";
            message += reference.path.empty() ? "" : "'" + PathToUtf8(reference.path.filename()) + "' ";
            message += reference.id.IsValid() ? "(id " + reference.id.ToString() + ") " : "";
            message += "could not be found; it may have been deleted.";
            throw ImportError(message);
        }
        if (GetAssetType(*path) != type) { throw ImportError(WrongFileMessage(*path, type)); }
        return *path;
    }

}  // namespace Frost
