#include <Frost/Assets/AssetManager.hpp>

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetImporter.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Renderer/MaterialSystem.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Renderer/Renderer.hpp>
#include <Frost/Renderer/Texture.hpp>

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

        auto asset = std::make_shared<MeshAsset>();
        asset->id = file.header.id;
        asset->path = path;
        asset->name = PathToUtf8(path.stem());

        // MeshData can still be rejected by the upload (e.g. an index range past the end); that is
        // a problem with the file, so report it as one. Version-1 files carry their materials and
        // textures in the MeshData, which CreateMesh uploads as they are. In version 2 the
        // sub-meshes' material indices refer to file.materials, which are built below, so the
        // geometry is uploaded without them.
        try {
            if (file.header.version >= 2) {
                MeshData geometry = file.mesh;
                for (SubMesh& subMesh : geometry.subMeshes) { subMesh.materialIndex = -1; }
                asset->mesh = m_Renderer.CreateMesh(geometry);
            }
            else {
                asset->mesh = m_Renderer.CreateMesh(file.mesh);
            }
        }
        catch (const std::invalid_argument& e) {
            throw ImportError("'" + PathToUtf8(path) + "' could not be uploaded: " + e.what());
        }
        catch (const std::runtime_error& e) {
            // CreateMesh reports inconsistent material/texture references this way.
            throw ImportError("'" + PathToUtf8(path) + "' is inconsistent: " + e.what());
        }

        // Version 2: materials refer to texture assets.
        std::vector<std::shared_ptr<Material>> materials;
        for (const MaterialAssetData& data : file.materials) {
            std::array<std::shared_ptr<Texture>, MaterialTextures::kSlotCount> slots{};
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                if (!data.textures[slot].IsSet()) { continue; }
                try {
                    slots[slot] = LoadTexture(data.textures[slot], kColorSlots[slot]);
                }
                catch (const ImportError& e) {
                    ++asset->missingTextures;
                    std::cerr << "[Frost::AssetManager] '" << asset->name << "', material '" << data.name
                              << "': a texture could not be loaded; using the slot's default. " << e.what() << '\n';
                }
            }

            materials.push_back(GetMaterialSystem().CreateMaterial(
                    data.parameters, MaterialTextures{.baseColor = slots[0],
                                                      .metallicRoughness = slots[1],
                                                      .normal = slots[2],
                                                      .occlusion = slots[3],
                                                      .emissive = slots[4]}));
        }

        // The file's sub-meshes, not the uploaded mesh's: version-2 geometry was uploaded with the
        // material indices cleared (see above).
        const std::vector<SubMesh>& subMeshes = file.mesh.subMeshes;
        for (std::size_t i = 0; i < subMeshes.size() && !materials.empty(); ++i) {
            const std::int32_t index = subMeshes[i].materialIndex;
            if (index >= 0 && static_cast<std::size_t>(index) < materials.size()) {
                asset->mesh->SetMaterial(i, materials[static_cast<std::size_t>(index)]);
            }
        }

        asset->positions.reserve(file.mesh.vertices.size());
        for (const Vertex& vertex : file.mesh.vertices) { asset->positions.push_back(vertex.position); }
        asset->indices = file.mesh.indices;

        if (!asset->positions.empty()) {
            asset->bounds.min = asset->bounds.max = asset->positions.front();
            for (const glm::vec3& position : asset->positions) {
                asset->bounds.min = glm::min(asset->bounds.min, position);
                asset->bounds.max = glm::max(asset->bounds.max, position);
            }
        }

        m_Meshes.emplace(key, asset);
        return asset;
    }

    std::shared_ptr<const MeshAsset> AssetManager::LoadMesh(const std::filesystem::path& path) {
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, m_Registry.GetProjectDirectory());
        return LoadMesh(AssetReference{.id = header.has_value() ? header->id : AssetId{}, .path = NormalizePath(path)});
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
        }
    }

    void AssetManager::UnloadTextures() { m_Textures.clear(); }

    void AssetManager::UnloadAll() {
        m_Meshes.clear();
        m_Textures.clear();
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

        if (extension == AssetFile::kMeshExtension) { return AssetType::Mesh; }
        if (extension == AssetFile::kTextureExtension) { return AssetType::Texture; }
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
            std::string message = std::string("The ") + (type == AssetType::Mesh ? "mesh" : "texture") + " asset ";
            message += reference.path.empty() ? "" : "'" + PathToUtf8(reference.path.filename()) + "' ";
            message += reference.id.IsValid() ? "(id " + reference.id.ToString() + ") " : "";
            message += "could not be found; it may have been deleted.";
            throw ImportError(message);
        }
        if (GetAssetType(*path) != type) { throw ImportError(WrongFileMessage(*path, type)); }
        return *path;
    }

}  // namespace Frost
