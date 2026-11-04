#include <Frost/Assets/AssetImporter.hpp>

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Assets/AssetRegistry.hpp>
#include <Frost/Assets/GltfImporter.hpp>
#include <Frost/Assets/Image.hpp>
#include <Frost/Platform/Filesystem.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <map>
#include <string>
#include <system_error>

namespace {

    [[nodiscard]]
    std::string Lowercase(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // A name usable as a file name on every platform.
    [[nodiscard]]
    std::string SanitizeFileName(const std::string& name) {
        std::string result;
        for (const char c : name) {
            const bool invalid = static_cast<unsigned char>(c) < 0x20 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos;
            result.push_back(invalid ? '_' : c);
        }
        while (!result.empty() && (result.back() == ' ' || result.back() == '.')) { result.pop_back(); }
        return result;
    }

    // `preferred`, or preferred_2, _3, ... if that file already exists.
    [[nodiscard]]
    std::filesystem::path UniquePath(const std::filesystem::path& preferred) {
        std::error_code error;
        if (!std::filesystem::exists(preferred, error)) { return preferred; }

        const std::filesystem::path base = preferred.parent_path() / preferred.stem();
        const std::filesystem::path extension = preferred.extension();
        for (int suffix = 2;; ++suffix) {
            std::filesystem::path candidate = base;
            candidate += "_" + std::to_string(suffix);
            candidate += extension;
            if (!std::filesystem::exists(candidate, error)) { return candidate; }
        }
    }

    // The id an asset written to `path` should have: the existing asset's (reimporting keeps its
    // identity), else a new one.
    [[nodiscard]]
    Frost::AssetId IdFor(const std::filesystem::path& path, const Frost::AssetType type) {
        const std::optional<Frost::AssetHeader> existing = Frost::AssetFile::ReadHeader(path, {});
        if (existing.has_value() && existing->type == type && existing->id.IsValid()) { return existing->id; }
        return Frost::AssetId::Generate();
    }

    // Identifies texture `index` of a model within the model's source, stably across reimports:
    // its name when it has a unique one, else its position.
    [[nodiscard]]
    std::string TextureItem(const std::vector<Frost::TextureData>& textures, const std::size_t index) {
        const std::string& name = textures[index].name;
        const bool unique = !name.empty() && std::count_if(textures.begin(), textures.end(), [&](const Frost::TextureData& texture) {
                                                 return texture.name == name;
                                             }) == 1;
        return unique ? "texture:" + name : "texture#" + std::to_string(index);
    }

    // The textures the existing mesh at `meshPath` (if any) uses, by the source item they were
    // imported from, wherever they are now. Reimporting a model (even from a different source file)
    // updates these in place rather than creating new ones.
    [[nodiscard]]
    std::map<std::string, Frost::AssetReference> ExistingMeshTextures(const std::filesystem::path& meshPath,
                                                                      const Frost::AssetRegistry* const registry,
                                                                      const std::filesystem::path& projectDirectory) {
        std::map<std::string, Frost::AssetReference> textures;
        const std::optional<Frost::AssetHeader> header = Frost::AssetFile::ReadHeader(meshPath, projectDirectory);
        if (!header.has_value() || header->type != Frost::AssetType::Mesh || header->version < 2) { return textures; }

        Frost::MeshFile file;
        try {
            file = Frost::AssetFile::ReadMesh(meshPath, projectDirectory);
        }
        catch (const std::exception&) {
            return textures;  // Unreadable: it is replaced as a whole.
        }

        for (const Frost::MaterialAssetData& material : file.materials) {
            for (const Frost::AssetReference& reference : material.textures) {
                if (!reference.id.IsValid() && reference.path.empty()) { continue; }

                std::optional<std::filesystem::path> path;
                if (registry != nullptr) { path = registry->Resolve(reference); }
                else if (!reference.path.empty()) { path = reference.path; }
                if (!path.has_value()) { continue; }

                const std::optional<Frost::AssetHeader> texture = Frost::AssetFile::ReadHeader(*path, projectDirectory);
                if (!texture.has_value() || texture->type != Frost::AssetType::Texture || !texture->id.IsValid() ||
                    texture->sourceItem.empty() || (reference.id.IsValid() && texture->id != reference.id)) {
                    continue;
                }
                textures.try_emplace(texture->sourceItem,
                                     Frost::AssetReference{.id = texture->id, .path = Frost::AssetManager::NormalizePath(*path)});
            }
        }
        return textures;
    }

    // Writes a model's textures as .ftex assets and the mesh as a .fmesh whose materials refer to
    // them. `mesh` is importer output (materials indexing embedded textures).
    Frost::AssetImporter::Result WriteMeshAssets(Frost::MeshData mesh, const std::filesystem::path& meshPath,
                                                 const Frost::AssetId& meshId, const std::filesystem::path& sourcePath,
                                                 const std::filesystem::path& textureFolder,
                                                 const Frost::AssetRegistry* const registry,
                                                 const std::filesystem::path& projectDirectory) {
        const std::map<std::string, Frost::AssetReference> existing = ExistingMeshTextures(meshPath, registry, projectDirectory);

        Frost::AssetImporter::Result result;
        const std::string meshName = Frost::PathToUtf8(meshPath.stem());

        // Textures first: the mesh refers to them.
        std::vector<Frost::AssetReference> textures;
        for (std::size_t i = 0; i < mesh.textures.size(); ++i) {
            const Frost::TextureData& texture = mesh.textures[i];
            const std::string item = TextureItem(mesh.textures, i);

            // The texture an earlier import of this model produced, if it still exists: updated in
            // place, keeping its id (and wherever it has been moved to). Preferably the one the
            // existing mesh uses; else one imported from the same source.
            const Frost::AssetRegistry::Entry* const previous =
                    registry != nullptr && !sourcePath.empty()
                            ? registry->FindBySource(sourcePath, item, Frost::AssetType::Texture)
                            : nullptr;

            Frost::AssetReference reference;
            if (const auto found = existing.find(item); found != existing.end()) { reference = found->second; }
            else if (previous != nullptr && previous->id.IsValid()) {
                reference = Frost::AssetReference{.id = previous->id, .path = previous->path};
            }
            else {
                const std::string name = SanitizeFileName(
                        meshName + "_" + (texture.name.empty() ? "Texture" + std::to_string(i) : texture.name));
                std::filesystem::path path = textureFolder / Frost::PathFromUtf8(name.empty() ? "Texture" : name);
                path += Frost::AssetFile::kTextureExtension;
                reference = Frost::AssetReference{.id = Frost::AssetId::Generate(),
                                                  .path = Frost::AssetManager::NormalizePath(UniquePath(path))};
            }

            Frost::AssetFile::WriteTexture(
                    reference.path,
                    Frost::TextureFile{.header = {.type = Frost::AssetType::Texture,
                                                  .version = Frost::AssetFile::kTextureVersion,
                                                  .id = reference.id,
                                                  .sourcePath = sourcePath,
                                                  .sourceItem = item},
                                       .image = texture.image,
                                       .settings = texture.settings},
                    projectDirectory);
            textures.push_back(reference);
        }

        // Materials: the embedded texture indices become references.
        Frost::MeshFile file;
        file.header = Frost::AssetHeader{.type = Frost::AssetType::Mesh,
                                         .version = Frost::AssetFile::kMeshVersion,
                                         .id = meshId,
                                         .sourcePath = sourcePath,
                                         .sourceItem = {}};
        for (const Frost::MaterialData& material : mesh.materials) {
            Frost::MaterialAssetData data{.name = material.name, .parameters = material.parameters, .textures = {}};
            const std::array<std::int32_t, Frost::MaterialTextures::kSlotCount> slots = {
                    material.baseColorTexture, material.metallicRoughnessTexture, material.normalTexture,
                    material.occlusionTexture, material.emissiveTexture};
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                if (slots[slot] >= 0 && static_cast<std::size_t>(slots[slot]) < textures.size()) {
                    data.textures[slot] = textures[static_cast<std::size_t>(slots[slot])];
                }
            }
            file.materials.push_back(std::move(data));
        }

        mesh.materials.clear();
        mesh.textures.clear();
        file.mesh = std::move(mesh);

        const std::filesystem::path normalizedMesh = Frost::AssetManager::NormalizePath(meshPath);
        Frost::AssetFile::WriteMesh(normalizedMesh, file, projectDirectory);

        result.asset = Frost::AssetReference{.id = meshId, .path = normalizedMesh};
        result.textures = std::move(textures);
        return result;
    }

}  // namespace

namespace Frost {

    std::optional<AssetType> AssetImporter::GetSourceType(const std::filesystem::path& source) {
        const std::string extension = Lowercase(PathToUtf8(source.extension()));

        if (extension == ".gltf" || extension == ".glb") { return AssetType::Mesh; }
        if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") { return AssetType::Texture; }
        return std::nullopt;
    }

    const char* AssetImporter::GetAssetExtension(const AssetType type) noexcept {
        return type == AssetType::Mesh ? AssetFile::kMeshExtension : AssetFile::kTextureExtension;
    }

    std::filesystem::path AssetImporter::GetDefaultTextureFolder(const std::filesystem::path& meshPath) {
        const std::filesystem::path folder = AssetManager::NormalizePath(meshPath).parent_path();
        const std::string folderName = PathToUtf8(folder.filename());

        if (Lowercase(folderName) == "meshes") {
            // Match the Meshes folder's capitalization.
            return folder.parent_path() / (folderName == "meshes" ? "textures" : "Textures");
        }
        return folder / "Textures";
    }

    AssetImporter::Result AssetImporter::Import(const std::filesystem::path& source, const Options& options) {
        const std::filesystem::path normalizedSource = AssetManager::NormalizePath(source);
        const std::filesystem::path destination = AssetManager::NormalizePath(options.destination);
        const std::optional<AssetType> type = GetSourceType(normalizedSource);
        const std::filesystem::path projectDirectory =
                !options.projectDirectory.empty() ? AssetManager::NormalizePath(options.projectDirectory)
                : options.registry != nullptr      ? options.registry->GetProjectDirectory()
                                                   : std::filesystem::path{};

        if (!type.has_value()) {
            throw ImportError("'" + PathToUtf8(normalizedSource) +
                              "' is not a file that can be imported (expected .gltf, .glb, .png, .jpg or .jpeg).");
        }
        if (AssetManager::GetAssetType(destination) != type) {
            throw ImportError("'" + PathToUtf8(destination) + "' is not a " + GetAssetExtension(*type) + " file name.");
        }

        if (*type == AssetType::Mesh) {
            const std::filesystem::path textureFolder =
                    options.textureFolder.empty() ? GetDefaultTextureFolder(destination)
                                                  : AssetManager::NormalizePath(options.textureFolder);
            return WriteMeshAssets(GltfImporter::Import(normalizedSource), destination, IdFor(destination, AssetType::Mesh),
                                   normalizedSource, textureFolder, options.registry, projectDirectory);
        }

        // Sampled as sRGB color by default; a material slot that holds data (normals, roughness,
        // ...) samples it as linear instead (see AssetManager::LoadTexture).
        const AssetId id = IdFor(destination, AssetType::Texture);
        AssetFile::WriteTexture(destination, TextureFile{.header = {.type = AssetType::Texture,
                                                                    .version = AssetFile::kTextureVersion,
                                                                    .id = id,
                                                                    .sourcePath = normalizedSource,
                                                                    .sourceItem = {}},
                                                         .image = ImageImporter::Import(normalizedSource),
                                                         .settings = TextureSettings{}},
                                 projectDirectory);
        return Result{.asset = {.id = id, .path = destination}, .textures = {}};
    }

    AssetImporter::Result AssetImporter::UpgradeLegacyAsset(const std::filesystem::path& asset,
                                                            const AssetRegistry* const registry) {
        const std::filesystem::path path = AssetManager::NormalizePath(asset);
        const std::filesystem::path projectDirectory = registry != nullptr ? registry->GetProjectDirectory() : std::filesystem::path{};
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, projectDirectory);
        if (!header.has_value()) { throw ImportError("'" + PathToUtf8(path) + "' is not a Frost asset."); }

        if (header->version >= 2) { return Result{.asset = {.id = header->id, .path = path}, .textures = {}}; }

        if (header->type == AssetType::Texture) {
            TextureFile file = AssetFile::ReadTexture(path, projectDirectory);
            file.header.id = AssetId::Generate();
            AssetFile::WriteTexture(path, file, projectDirectory);
            return Result{.asset = {.id = file.header.id, .path = path}, .textures = {}};
        }

        MeshFile file = AssetFile::ReadMesh(path, projectDirectory);
        return WriteMeshAssets(std::move(file.mesh), path, AssetId::Generate(), file.header.sourcePath,
                               GetDefaultTextureFolder(path), registry, projectDirectory);
    }

}  // namespace Frost
