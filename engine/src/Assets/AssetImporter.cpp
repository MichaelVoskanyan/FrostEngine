#include <Frost/Assets/AssetImporter.hpp>

#include <Frost/Assets/AssetFile.hpp>
#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Assets/AssetRegistry.hpp>
#include <Frost/Assets/GltfImporter.hpp>
#include <Frost/Assets/Image.hpp>
#include <Frost/Assets/Sound.hpp>
#include <Frost/Platform/Filesystem.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>

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

    // The textures the existing mesh (static or skeletal) at `meshPath`, if any, uses, by the
    // source item they were imported from, wherever they are now. Reimporting a model (even from a
    // different source file) updates these in place rather than creating new ones.
    [[nodiscard]]
    std::map<std::string, Frost::AssetReference> ExistingMeshTextures(const std::filesystem::path& meshPath,
                                                                      const Frost::AssetRegistry* const registry,
                                                                      const std::filesystem::path& projectDirectory) {
        std::map<std::string, Frost::AssetReference> textures;
        if (meshPath.empty()) { return textures; }

        const std::optional<Frost::AssetHeader> header = Frost::AssetFile::ReadHeader(meshPath, projectDirectory);
        if (!header.has_value() || !Frost::AssetFile::HasId(header->type, header->version)) { return textures; }

        std::vector<Frost::MaterialAssetData> materials;
        try {
            if (header->type == Frost::AssetType::Mesh) { materials = Frost::AssetFile::ReadMesh(meshPath, projectDirectory).materials; }
            else if (header->type == Frost::AssetType::SkeletalMesh) {
                materials = Frost::AssetFile::ReadSkeletalMesh(meshPath, projectDirectory).materials;
            }
        }
        catch (const std::exception&) {
            return textures;  // Unreadable: it is replaced as a whole.
        }

        for (const Frost::MaterialAssetData& material : materials) {
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

    // Writes the textures of one or more meshes imported from the same source as .ftex assets, each
    // once however many of the meshes use it.
    class TextureWriter final {
    public:
        TextureWriter(std::filesystem::path sourcePath, std::filesystem::path textureFolder,
                      const Frost::AssetRegistry* const registry, std::filesystem::path projectDirectory)
            : m_SourcePath(std::move(sourcePath)),
              m_TextureFolder(std::move(textureFolder)),
              m_Registry(registry),
              m_ProjectDirectory(std::move(projectDirectory)) {}

        // The textures the mesh about to be written at `meshPath` used before (updated in place).
        void AddExisting(const std::filesystem::path& meshPath) {
            for (auto& [item, reference] : ExistingMeshTextures(meshPath, m_Registry, m_ProjectDirectory)) {
                m_Existing.try_emplace(item, reference);
            }
        }

        // Writes (or reuses) each texture; the result is by index into `textures`. New files are
        // named after `meshPath`.
        [[nodiscard]]
        std::vector<Frost::AssetReference> Write(const std::vector<Frost::TextureData>& textures,
                                                 const std::filesystem::path& meshPath) {
            const std::string meshName = Frost::PathToUtf8(meshPath.stem());

            std::vector<Frost::AssetReference> references;
            for (std::size_t i = 0; i < textures.size(); ++i) {
                const Frost::TextureData& texture = textures[i];
                const std::string item = TextureItem(textures, i);

                if (const auto written = m_Written.find(item); written != m_Written.end()) {
                    references.push_back(written->second);
                    continue;
                }

                // The texture an earlier import of this model produced, if it still exists: updated
                // in place, keeping its id (and wherever it has been moved to). Preferably the one
                // an existing mesh uses; else one imported from the same source.
                const Frost::AssetRegistry::Entry* const previous =
                        m_Registry != nullptr && !m_SourcePath.empty()
                                ? m_Registry->FindBySource(m_SourcePath, item, Frost::AssetType::Texture)
                                : nullptr;

                Frost::AssetReference reference;
                if (const auto found = m_Existing.find(item); found != m_Existing.end()) { reference = found->second; }
                else if (previous != nullptr && previous->id.IsValid()) {
                    reference = Frost::AssetReference{.id = previous->id, .path = previous->path};
                }
                else {
                    const std::string name = SanitizeFileName(
                            meshName + "_" + (texture.name.empty() ? "Texture" + std::to_string(i) : texture.name));
                    std::filesystem::path path = m_TextureFolder / Frost::PathFromUtf8(name.empty() ? "Texture" : name);
                    path += Frost::AssetFile::kTextureExtension;
                    reference = Frost::AssetReference{.id = Frost::AssetId::Generate(),
                                                      .path = Frost::AssetManager::NormalizePath(UniquePath(path))};
                }

                Frost::AssetFile::WriteTexture(
                        reference.path,
                        Frost::TextureFile{.header = {.type = Frost::AssetType::Texture,
                                                      .version = Frost::AssetFile::kTextureVersion,
                                                      .id = reference.id,
                                                      .sourcePath = m_SourcePath,
                                                      .sourceItem = item},
                                           .image = texture.image,
                                           .settings = texture.settings},
                        m_ProjectDirectory);
                m_Written.emplace(item, reference);
                m_Order.push_back(reference);
                references.push_back(reference);
            }
            return references;
        }

        // Every texture written, in the order written.
        [[nodiscard]]
        const std::vector<Frost::AssetReference>& GetWritten() const noexcept {
            return m_Order;
        }

    private:
        std::filesystem::path m_SourcePath;
        std::filesystem::path m_TextureFolder;
        const Frost::AssetRegistry* m_Registry;
        std::filesystem::path m_ProjectDirectory;
        std::map<std::string, Frost::AssetReference> m_Existing;
        std::map<std::string, Frost::AssetReference> m_Written;
        std::vector<Frost::AssetReference> m_Order;
    };

    // A mesh's materials as asset data: the embedded texture indices become references.
    [[nodiscard]]
    std::vector<Frost::MaterialAssetData> ToMaterialAssets(const std::vector<Frost::MaterialData>& materials,
                                                           const std::vector<Frost::AssetReference>& textures) {
        std::vector<Frost::MaterialAssetData> result;
        for (const Frost::MaterialData& material : materials) {
            Frost::MaterialAssetData data{.name = material.name, .parameters = material.parameters, .textures = {}};
            const std::array<std::int32_t, Frost::MaterialTextures::kSlotCount> slots = {
                    material.baseColorTexture, material.metallicRoughnessTexture, material.normalTexture,
                    material.occlusionTexture, material.emissiveTexture};
            for (std::size_t slot = 0; slot < slots.size(); ++slot) {
                if (slots[slot] >= 0 && static_cast<std::size_t>(slots[slot]) < textures.size()) {
                    data.textures[slot] = textures[static_cast<std::size_t>(slots[slot])];
                }
            }
            result.push_back(std::move(data));
        }
        return result;
    }

    // Writes a model's textures as .ftex assets and the mesh as a .fmesh whose materials refer to
    // them. `mesh` is importer output (materials indexing embedded textures).
    Frost::AssetReference WriteMeshAsset(Frost::MeshData mesh, const std::filesystem::path& meshPath,
                                         const Frost::AssetId& meshId, const std::filesystem::path& sourcePath,
                                         TextureWriter& textureWriter, const std::filesystem::path& projectDirectory) {
        const std::filesystem::path normalizedMesh = Frost::AssetManager::NormalizePath(meshPath);

        // Textures first: the mesh refers to them.
        const std::vector<Frost::AssetReference> textures = textureWriter.Write(mesh.textures, normalizedMesh);

        Frost::MeshFile file;
        file.header = Frost::AssetHeader{.type = Frost::AssetType::Mesh,
                                         .version = Frost::AssetFile::kMeshVersion,
                                         .id = meshId,
                                         .sourcePath = sourcePath,
                                         .sourceItem = {}};
        file.materials = ToMaterialAssets(mesh.materials, textures);

        mesh.materials.clear();
        mesh.textures.clear();
        file.mesh = std::move(mesh);

        Frost::AssetFile::WriteMesh(normalizedMesh, file, projectDirectory);
        return Frost::AssetReference{.id = meshId, .path = normalizedMesh};
    }

    // The path of an asset a reference points at: through the registry if there is one, else its
    // path as recorded.
    [[nodiscard]]
    std::optional<std::filesystem::path> ResolveReference(const Frost::AssetReference& reference,
                                                          const Frost::AssetRegistry* const registry) {
        if (registry != nullptr) { return registry->Resolve(reference); }
        if (!reference.path.empty()) { return Frost::AssetManager::NormalizePath(reference.path); }
        return std::nullopt;
    }

    // The skeleton an existing skeletal mesh or animation at `path` is bound to, if it is one.
    [[nodiscard]]
    Frost::AssetReference ExistingSkeletonOf(const std::filesystem::path& path, const Frost::AssetType type,
                                             const std::filesystem::path& projectDirectory) {
        const std::optional<Frost::AssetHeader> header = Frost::AssetFile::ReadHeader(path, projectDirectory);
        if (!header.has_value() || header->type != type) { return {}; }

        try {
            if (type == Frost::AssetType::SkeletalMesh) { return Frost::AssetFile::ReadSkeletalMesh(path, projectDirectory).skeleton; }
            if (type == Frost::AssetType::Animation) { return Frost::AssetFile::ReadAnimation(path, projectDirectory).skeleton; }
        }
        catch (const Frost::ImportError&) {
            // Unreadable: as if there were none.
        }
        return {};
    }

    // The visibility keys authored (in the editor) on an existing animation at `path`, so a reimport
    // keeps them: they are not part of the source file.
    [[nodiscard]]
    std::vector<Frost::VisibilityKey> ExistingVisibilityKeys(const std::filesystem::path& path,
                                                             const std::filesystem::path& projectDirectory) {
        const std::optional<Frost::AssetHeader> header = Frost::AssetFile::ReadHeader(path, projectDirectory);
        if (!header.has_value() || header->type != Frost::AssetType::Animation) { return {}; }

        try {
            return Frost::AssetFile::ReadAnimation(path, projectDirectory).clip.visibilityKeys;
        }
        catch (const Frost::ImportError&) {
            return {};  // Unreadable: as if there were none.
        }
    }

    // The meshes an existing skeletal mesh at `path` comes with as separate attachments, by part
    // name, where they are now (those that still exist).
    [[nodiscard]]
    std::map<std::string, std::filesystem::path> ExistingPartMeshes(const std::filesystem::path& path,
                                                                    const Frost::AssetRegistry* const registry,
                                                                    const std::filesystem::path& projectDirectory) {
        std::map<std::string, std::filesystem::path> parts;
        if (path.empty()) { return parts; }

        const std::optional<Frost::AssetHeader> header = Frost::AssetFile::ReadHeader(path, projectDirectory);
        if (!header.has_value() || header->type != Frost::AssetType::SkeletalMesh || header->version < 3) { return parts; }

        std::vector<Frost::DefaultAttachment> attachments;
        try {
            attachments = Frost::AssetFile::ReadSkeletalMesh(path, projectDirectory).defaultAttachments;
        }
        catch (const Frost::ImportError&) {
            return parts;  // Unreadable: as if there were none.
        }

        for (const Frost::DefaultAttachment& attachment : attachments) {
            const std::optional<std::filesystem::path> mesh = ResolveReference(attachment.mesh, registry);
            std::error_code error;
            if (mesh.has_value() && std::filesystem::is_regular_file(*mesh, error)) { parts.try_emplace(attachment.name, *mesh); }
        }
        return parts;
    }

    // Adds to `skeleton` (freshly imported) the bones an existing skeleton at `path` had that were
    // added in the editor: each whose name the import does not have, and whose parent it has (or
    // that is a root), in their order. A reimport keeps them.
    void KeepBonesAddedInEditor(Frost::SkeletonData& skeleton, const std::filesystem::path& path,
                                const std::filesystem::path& projectDirectory) {
        const std::optional<Frost::AssetHeader> header = Frost::AssetFile::ReadHeader(path, projectDirectory);
        if (!header.has_value() || header->type != Frost::AssetType::Skeleton || header->version < 2) { return; }

        Frost::SkeletonData existing;
        try {
            existing = Frost::AssetFile::ReadSkeleton(path, projectDirectory).skeleton;
        }
        catch (const Frost::ImportError&) {
            return;  // Unreadable: as if there were none.
        }

        for (const Frost::Bone& bone : existing.bones) {
            if (!bone.addedInEditor || skeleton.FindBone(bone.name).has_value() || skeleton.bones.size() >= Frost::SkeletonData::kMaxBones) {
                continue;
            }
            std::int32_t parent = -1;
            if (bone.parent >= 0) {
                const std::optional<std::size_t> found = skeleton.FindBone(existing.bones[static_cast<std::size_t>(bone.parent)].name);
                if (!found.has_value()) { continue; }  // Its parent is gone from the model.
                parent = static_cast<std::int32_t>(*found);
            }
            Frost::Bone kept = bone;
            kept.parent = parent;
            skeleton.bones.push_back(std::move(kept));
        }
    }

    // The sockets of an existing skeletal mesh at `path` (authored in the editor), so a reimport
    // keeps them.
    [[nodiscard]]
    std::vector<Frost::SocketData> ExistingSockets(const std::filesystem::path& path, const std::filesystem::path& projectDirectory) {
        const std::optional<Frost::AssetHeader> header = Frost::AssetFile::ReadHeader(path, projectDirectory);
        if (!header.has_value() || header->type != Frost::AssetType::SkeletalMesh || header->version < 4) { return {}; }

        try {
            return Frost::AssetFile::ReadSkeletalMesh(path, projectDirectory).sockets;
        }
        catch (const Frost::ImportError&) {
            return {};
        }
    }

    // Of `all` (a model's materials), those `subMeshes` use, in order of first use; their material
    // indices are renumbered to match.
    [[nodiscard]]
    std::vector<Frost::MaterialAssetData> TakeUsedMaterials(std::vector<Frost::SubMesh>& subMeshes,
                                                            const std::vector<Frost::MaterialAssetData>& all) {
        std::vector<Frost::MaterialAssetData> used;
        std::map<std::int32_t, std::int32_t> renumbered;
        for (Frost::SubMesh& subMesh : subMeshes) {
            if (subMesh.materialIndex < 0 || static_cast<std::size_t>(subMesh.materialIndex) >= all.size()) {
                subMesh.materialIndex = -1;
                continue;
            }
            const auto [entry, added] = renumbered.try_emplace(subMesh.materialIndex, static_cast<std::int32_t>(used.size()));
            if (added) { used.push_back(all[static_cast<std::size_t>(subMesh.materialIndex)]); }
            subMesh.materialIndex = entry->second;
        }
        return used;
    }

    [[nodiscard]]
    std::filesystem::path ProjectDirectoryOf(const std::filesystem::path& projectDirectory,
                                             const Frost::AssetRegistry* const registry) {
        if (!projectDirectory.empty()) { return Frost::AssetManager::NormalizePath(projectDirectory); }
        return registry != nullptr ? registry->GetProjectDirectory() : std::filesystem::path{};
    }

    // ImportModel on an already opened model.
    Frost::AssetImporter::ModelResult ImportModelFrom(Frost::GltfModel& model, const std::filesystem::path& source,
                                                      const Frost::AssetImporter::ModelOptions& options) {
        using Frost::AssetType;
        using Frost::ImportError;

        const std::filesystem::path projectDirectory = ProjectDirectoryOf(options.projectDirectory, options.registry);
        const auto normalized = [](const std::filesystem::path& path) {
            return path.empty() ? path : Frost::AssetManager::NormalizePath(path);
        };

        const std::filesystem::path meshPath = normalized(options.mesh);
        const std::filesystem::path skeletalMeshPath = normalized(options.skeletalMesh);
        const std::filesystem::path skeletonPath = normalized(options.skeleton);

        // Every destination must be of its type, and distinct.
        std::set<std::filesystem::path> destinations;
        const auto checkDestination = [&](const std::filesystem::path& path, const AssetType type) {
            if (path.empty()) { return; }
            if (Frost::AssetManager::GetAssetType(path) != type) {
                throw ImportError("'" + Frost::PathToUtf8(path) + "' is not a " + Frost::AssetFile::GetExtension(type) +
                                  " file name.");
            }
            if (!destinations.insert(path).second) {
                throw ImportError("'" + Frost::PathToUtf8(path.filename()) + "' is the destination of two imported assets.");
            }
        };
        checkDestination(meshPath, AssetType::Mesh);
        checkDestination(skeletalMeshPath, AssetType::SkeletalMesh);
        checkDestination(skeletonPath, AssetType::Skeleton);
        for (const Frost::AssetImporter::ModelOptions::Animation& animation : options.animations) {
            if (animation.destination.empty()) { throw ImportError("An animation to import has no destination."); }
            checkDestination(normalized(animation.destination), AssetType::Animation);
        }
        if (destinations.empty()) { throw ImportError("Nothing to import: no asset was chosen."); }

        const Frost::GltfModelInfo& info = model.GetInfo();
        const bool needsSkeleton = !skeletalMeshPath.empty() || !options.animations.empty();

        Frost::AssetImporter::ModelResult result;

        // 1. Everything is imported into memory first.
        Frost::SkeletonData skeleton;
        if (!skeletonPath.empty()) {
            skeleton = model.ImportSkeleton();
            KeepBonesAddedInEditor(skeleton, skeletonPath, projectDirectory);
            result.skeleton = Frost::AssetReference{.id = IdFor(skeletonPath, AssetType::Skeleton), .path = skeletonPath};
        }
        else if (needsSkeleton) {
            if (!options.existingSkeleton.IsSet()) {
                throw ImportError("A skeletal mesh or animation needs a skeleton: import the model's, or choose an existing one.");
            }
            const std::optional<std::filesystem::path> path = ResolveReference(options.existingSkeleton, options.registry);
            if (!path.has_value()) {
                throw ImportError("The skeleton " +
                                  (options.existingSkeleton.path.empty() ? std::string("asset")
                                                                         : "'" + Frost::PathToUtf8(options.existingSkeleton.path.filename()) + "'") +
                                  " to bind to could not be found.");
            }
            Frost::SkeletonFile file = Frost::AssetFile::ReadSkeleton(*path, projectDirectory);
            skeleton = std::move(file.skeleton);
            result.skeleton = Frost::AssetReference{.id = file.header.id, .path = Frost::AssetManager::NormalizePath(*path)};
        }

        std::optional<Frost::SkeletalMeshData> skeletalMesh;
        std::vector<Frost::GltfSeparatePart> separateParts;
        if (!skeletalMeshPath.empty()) {
            const Frost::GltfAttachmentOptions attachments{.attachLooseParts = options.attachLooseParts,
                                                           .partBones = options.partBones,
                                                           .separateParts = options.separateParts};
            skeletalMesh = model.ImportSkeletalMesh(skeleton, result.warnings, attachments, &separateParts);
        }

        // Where the separate parts go: where an earlier import of each went (as the existing
        // skeletal mesh lists it, else as the registry knows it), else next to the skeletal mesh.
        std::vector<std::filesystem::path> partPaths;
        if (!separateParts.empty()) {
            const std::map<std::string, std::filesystem::path> existing =
                    ExistingPartMeshes(skeletalMeshPath, options.registry, projectDirectory);
            const std::string stem = Frost::PathToUtf8(skeletalMeshPath.stem());

            for (const Frost::GltfSeparatePart& part : separateParts) {
                const std::string item = Frost::AssetImporter::GetPartItem(part.name);
                // A file is this part's if an earlier import of it wrote it.
                const auto isPart = [&](const std::filesystem::path& path) {
                    const std::optional<Frost::AssetHeader> header = Frost::AssetFile::ReadHeader(path, projectDirectory);
                    return header.has_value() && header->type == AssetType::Mesh && header->sourceItem == item;
                };

                std::filesystem::path path;
                if (const auto found = existing.find(part.name); found != existing.end()) { path = normalized(found->second); }
                else if (options.registry != nullptr) {
                    if (const Frost::AssetRegistry::Entry* const entry = options.registry->FindBySource(source, item, AssetType::Mesh)) {
                        path = normalized(entry->path);
                    }
                }
                if (path.empty() || destinations.contains(path)) {
                    const std::string name = SanitizeFileName(stem + "_" + part.name);
                    const std::filesystem::path base = skeletalMeshPath.parent_path() / Frost::PathFromUtf8(name.empty() ? "Part" : name);
                    std::error_code error;
                    for (int suffix = 1;; ++suffix) {
                        path = base;
                        if (suffix > 1) { path += "_" + std::to_string(suffix); }
                        path += Frost::AssetFile::kMeshExtension;
                        path = normalized(path);
                        if (!destinations.contains(path) && (!std::filesystem::exists(path, error) || isPart(path))) { break; }
                    }
                }
                destinations.insert(path);
                partPaths.push_back(path);
            }
        }

        std::vector<Frost::AnimationClipData> clips;
        for (const Frost::AssetImporter::ModelOptions::Animation& animation : options.animations) {
            if (animation.index >= info.animations.size()) {
                throw ImportError("'" + Frost::PathToUtf8(source.filename()) + "' has no animation " +
                                  std::to_string(animation.index) + ".");
            }
            clips.push_back(model.ImportAnimation(animation.index, skeleton, result.warnings));
        }

        std::optional<Frost::MeshData> mesh;
        if (!meshPath.empty()) { mesh = model.ImportMesh(); }

        // What the files would refuse to hold (see AssetFile), refused before any of them is written.
        const auto refuse = [&](const std::string& what, const std::optional<std::string>& problem) {
            if (problem.has_value()) {
                throw ImportError("'" + Frost::PathToUtf8(source.filename()) + "' cannot be imported: " + what + " " + *problem + ".");
            }
        };
        if (!skeletonPath.empty()) { refuse("its skeleton:", skeleton.Validate()); }
        if (skeletalMesh.has_value()) { refuse("its skeletal mesh:", Frost::ValidateSkin(*skeletalMesh)); }
        for (std::size_t i = 0; i < clips.size(); ++i) {
            refuse("animation '" + info.animations[options.animations[i].index].name + "':", clips[i].Validate());
        }

        // 2. Then written: the skeleton first (the others refer to it), then the meshes (with their
        // textures, shared between the two), then the animations.
        if (!skeletonPath.empty()) {
            Frost::AssetFile::WriteSkeleton(skeletonPath,
                                            Frost::SkeletonFile{.header = {.type = AssetType::Skeleton,
                                                                           .version = Frost::AssetFile::kSkeletonVersion,
                                                                           .id = result.skeleton.id,
                                                                           .sourcePath = source,
                                                                           .sourceItem = {}},
                                                                .skeleton = skeleton},
                                            projectDirectory);
        }

        const std::filesystem::path textureFolder =
                !options.textureFolder.empty() ? normalized(options.textureFolder)
                : !skeletalMeshPath.empty()    ? Frost::AssetImporter::GetDefaultTextureFolder(skeletalMeshPath)
                : !meshPath.empty()            ? Frost::AssetImporter::GetDefaultTextureFolder(meshPath)
                                               : std::filesystem::path{};
        TextureWriter textureWriter(source, textureFolder, options.registry, projectDirectory);
        textureWriter.AddExisting(skeletalMeshPath);
        textureWriter.AddExisting(meshPath);
        for (const std::filesystem::path& path : partPaths) { textureWriter.AddExisting(path); }

        if (skeletalMesh.has_value()) {
            const Frost::AssetId id = IdFor(skeletalMeshPath, AssetType::SkeletalMesh);
            // The materials of the skeletal mesh and of its separate parts, each taking its own.
            const std::vector<Frost::AssetReference> textures = textureWriter.Write(skeletalMesh->mesh.textures, skeletalMeshPath);
            const std::vector<Frost::MaterialAssetData> materials = ToMaterialAssets(skeletalMesh->mesh.materials, textures);

            Frost::SkeletalMeshFile file;
            file.header = Frost::AssetHeader{.type = AssetType::SkeletalMesh,
                                             .version = Frost::AssetFile::kSkeletalMeshVersion,
                                             .id = id,
                                             .sourcePath = source,
                                             .sourceItem = {}};
            file.skeleton = result.skeleton;
            file.sockets = ExistingSockets(skeletalMeshPath, projectDirectory);

            // The separate parts first: the skeletal mesh refers to them.
            for (std::size_t i = 0; i < separateParts.size(); ++i) {
                Frost::GltfSeparatePart& part = separateParts[i];
                Frost::MeshFile partFile;
                partFile.header = Frost::AssetHeader{.type = AssetType::Mesh,
                                                     .version = Frost::AssetFile::kMeshVersion,
                                                     .id = IdFor(partPaths[i], AssetType::Mesh),
                                                     .sourcePath = source,
                                                     .sourceItem = Frost::AssetImporter::GetPartItem(part.name)};
                partFile.materials = TakeUsedMaterials(part.mesh.subMeshes, materials);
                partFile.mesh = std::move(part.mesh);
                Frost::AssetFile::WriteMesh(partPaths[i], partFile, projectDirectory);

                const Frost::AssetReference reference{.id = partFile.header.id, .path = partPaths[i]};
                file.defaultAttachments.push_back(
                        Frost::DefaultAttachment{.name = part.name, .slot = part.name, .bone = part.bone, .mesh = reference});
                result.parts.push_back(reference);
            }

            file.materials = TakeUsedMaterials(skeletalMesh->mesh.subMeshes, materials);
            skeletalMesh->mesh.materials.clear();
            skeletalMesh->mesh.textures.clear();
            file.mesh = std::move(*skeletalMesh);

            Frost::AssetFile::WriteSkeletalMesh(skeletalMeshPath, file, projectDirectory);
            result.skeletalMesh = Frost::AssetReference{.id = id, .path = skeletalMeshPath};
        }

        if (mesh.has_value()) {
            result.mesh = WriteMeshAsset(std::move(*mesh), meshPath, IdFor(meshPath, AssetType::Mesh), source, textureWriter,
                                         projectDirectory);
        }

        for (std::size_t i = 0; i < options.animations.size(); ++i) {
            const std::filesystem::path path = normalized(options.animations[i].destination);
            const Frost::AssetId id = IdFor(path, AssetType::Animation);
            clips[i].visibilityKeys = ExistingVisibilityKeys(path, projectDirectory);
            Frost::AssetFile::WriteAnimation(
                    path,
                    Frost::AnimationFile{.header = {.type = AssetType::Animation,
                                                    .version = Frost::AssetFile::kAnimationVersion,
                                                    .id = id,
                                                    .sourcePath = source,
                                                    .sourceItem = Frost::AssetImporter::GetAnimationItem(info, options.animations[i].index)},
                                         .skeleton = result.skeleton,
                                         .clip = std::move(clips[i])},
                    projectDirectory);
            result.animations.push_back(Frost::AssetReference{.id = id, .path = path});
        }

        result.textures = textureWriter.GetWritten();
        return result;
    }

    // Rebuilds a version-1 mesh (embedded textures) as a current one with texture assets.
    Frost::AssetImporter::Result UpgradeLegacyMesh(Frost::MeshData mesh, const std::filesystem::path& meshPath,
                                                   const std::filesystem::path& sourcePath,
                                                   const Frost::AssetRegistry* const registry,
                                                   const std::filesystem::path& projectDirectory) {
        TextureWriter textureWriter(sourcePath, Frost::AssetImporter::GetDefaultTextureFolder(meshPath), registry,
                                    projectDirectory);
        Frost::AssetImporter::Result result;
        result.asset = WriteMeshAsset(std::move(mesh), meshPath, Frost::AssetId::Generate(), sourcePath, textureWriter,
                                      projectDirectory);
        result.textures = textureWriter.GetWritten();
        return result;
    }

}  // namespace

namespace Frost {

    std::optional<AssetType> AssetImporter::GetSourceType(const std::filesystem::path& source) {
        const std::string extension = Lowercase(PathToUtf8(source.extension()));

        if (extension == ".gltf" || extension == ".glb") { return AssetType::Mesh; }
        if (extension == ".png" || extension == ".jpg" || extension == ".jpeg") { return AssetType::Texture; }
        if (extension == ".wav" || extension == ".mp3") { return AssetType::Sound; }
        return std::nullopt;
    }

    bool AssetImporter::IsModelSource(const std::filesystem::path& source) { return GetSourceType(source) == AssetType::Mesh; }

    bool AssetImporter::CanImportAs(const std::filesystem::path& source, const AssetType type) {
        const std::optional<AssetType> sourceType = GetSourceType(source);
        if (!sourceType.has_value()) { return false; }
        // An image or a sound becomes an asset of its own type only; a model, any model type.
        if (*sourceType != AssetType::Mesh) { return type == *sourceType; }
        return type != AssetType::Texture && type != AssetType::Sound;
    }

    const char* AssetImporter::GetAssetExtension(const AssetType type) noexcept { return AssetFile::GetExtension(type); }

    std::filesystem::path AssetImporter::GetDefaultTextureFolder(const std::filesystem::path& meshPath) {
        const std::filesystem::path folder = AssetManager::NormalizePath(meshPath).parent_path();
        const std::string folderName = PathToUtf8(folder.filename());

        if (Lowercase(folderName) == "meshes") {
            // Match the Meshes folder's capitalization.
            return folder.parent_path() / (folderName == "meshes" ? "textures" : "Textures");
        }
        return folder / "Textures";
    }

    std::filesystem::path AssetImporter::GetDefaultAnimationFolder(const std::filesystem::path& meshPath) {
        const std::filesystem::path folder = AssetManager::NormalizePath(meshPath).parent_path();
        const std::string folderName = PathToUtf8(folder.filename());

        if (Lowercase(folderName) == "meshes") {
            return folder.parent_path() / (folderName == "meshes" ? "animations" : "Animations");
        }
        return folder / "Animations";
    }

    std::string AssetImporter::GetAnimationItem(const GltfModelInfo& info, const std::size_t index) {
        if (index >= info.animations.size()) { return "animation#" + std::to_string(index); }

        const GltfAnimationInfo& animation = info.animations[index];
        const bool unique = animation.hasName &&
                            std::count_if(info.animations.begin(), info.animations.end(), [&](const GltfAnimationInfo& other) {
                                return other.hasName && other.name == animation.name;
                            }) == 1;
        return unique ? "animation:" + animation.name : "animation#" + std::to_string(index);
    }

    std::optional<std::size_t> AssetImporter::FindAnimation(const GltfModelInfo& info, const std::string_view item) {
        for (std::size_t i = 0; i < info.animations.size(); ++i) {
            if (GetAnimationItem(info, i) == item) { return i; }
        }
        return std::nullopt;
    }

    std::string AssetImporter::GetPartItem(const std::string_view partName) { return "part:" + std::string(partName); }

    std::optional<std::map<std::string, AssetImporter::PartAttachment>> AssetImporter::GetPartAttachments(
            const std::filesystem::path& skeletalMesh, const std::filesystem::path& projectDirectory) {
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(skeletalMesh, projectDirectory);
        if (!header.has_value() || header->type != AssetType::SkeletalMesh || header->version < 2) { return std::nullopt; }

        SkeletalMeshFile file;
        try {
            file = AssetFile::ReadSkeletalMesh(skeletalMesh, projectDirectory);
        }
        catch (const ImportError&) {
            return std::nullopt;
        }

        // A sub-mesh is a rigid part if it is attached, or if none of its vertices is skinned.
        const MeshData& mesh = file.mesh.mesh;
        std::map<std::string, PartAttachment> parts;
        for (const DefaultAttachment& attachment : file.defaultAttachments) {
            parts[attachment.name] = PartAttachment{.bone = attachment.bone, .separate = true};
        }
        for (std::size_t i = 0; i < mesh.subMeshes.size(); ++i) {
            const SubMesh& subMesh = mesh.subMeshes[i];
            const std::int32_t attachment = i < file.mesh.attachments.size() ? file.mesh.attachments[i] : -1;
            if (attachment >= 0) {
                parts[subMesh.name] = PartAttachment{.bone = file.mesh.bindBones[static_cast<std::size_t>(attachment)], .separate = false};
                continue;
            }

            bool skinned = false;
            for (std::uint32_t index = subMesh.firstIndex; index < subMesh.firstIndex + subMesh.indexCount && !skinned; ++index) {
                const SkinWeights& weights = file.mesh.skin[std::size_t{mesh.indices[index]} + static_cast<std::size_t>(subMesh.vertexOffset)];
                skinned = std::any_of(weights.weights.begin(), weights.weights.end(), [](const float weight) { return weight > 0.0f; });
            }
            if (!skinned) { parts.try_emplace(subMesh.name, PartAttachment{}); }
        }
        return parts;
    }

    AssetImporter::Result AssetImporter::Import(const std::filesystem::path& source, const Options& options) {
        const std::filesystem::path normalizedSource = AssetManager::NormalizePath(source);
        const std::filesystem::path destination = AssetManager::NormalizePath(options.destination);
        const std::optional<AssetType> sourceType = GetSourceType(normalizedSource);
        const std::optional<AssetType> type = AssetManager::GetAssetType(destination);
        const std::filesystem::path projectDirectory = ProjectDirectoryOf(options.projectDirectory, options.registry);

        if (!sourceType.has_value()) {
            throw ImportError("'" + PathToUtf8(normalizedSource) + "' is not a file that can be imported (expected " +
                              kImportableExtensions + ").");
        }
        if (!type.has_value() || !CanImportAs(normalizedSource, *type)) {
            throw ImportError(*sourceType == AssetType::Mesh
                                      ? "'" + PathToUtf8(destination) + "' is not a .fmesh, .fskel, .fskmesh or .fanim file name."
                                      : "'" + PathToUtf8(destination) + "' is not a " + GetAssetExtension(*sourceType) +
                                                " file name.");
        }

        if (*type == AssetType::Sound) {
            // The source file itself goes into the asset, byte for byte, with its properties. Read
            // (and validated) before anything else: an unsupported file leaves the destination as
            // it was. (Not inside SoundFile's initializer: GCC destroys members twice when an
            // aggregate initializer throws.)
            SoundData sound = SoundImporter::Import(normalizedSource);
            const AssetId id = IdFor(destination, AssetType::Sound);
            AssetFile::WriteSound(destination,
                                  SoundFile{.header = {.type = AssetType::Sound,
                                                       .version = AssetFile::kSoundVersion,
                                                       .id = id,
                                                       .sourcePath = normalizedSource,
                                                       .sourceItem = {}},
                                            .sound = std::move(sound)},
                                  projectDirectory);
            return Result{.asset = {.id = id, .path = destination}, .textures = {}, .warnings = {}, .parts = {}};
        }

        if (*type == AssetType::Texture) {
            // Sampled as sRGB color by default; a material slot that holds data (normals,
            // roughness, ...) samples it as linear instead (see AssetManager::LoadTexture).
            const AssetId id = IdFor(destination, AssetType::Texture);
            AssetFile::WriteTexture(destination, TextureFile{.header = {.type = AssetType::Texture,
                                                                        .version = AssetFile::kTextureVersion,
                                                                        .id = id,
                                                                        .sourcePath = normalizedSource,
                                                                        .sourceItem = {}},
                                                             .image = ImageImporter::Import(normalizedSource),
                                                             .settings = TextureSettings{}},
                                     projectDirectory);
            return Result{.asset = {.id = id, .path = destination}, .textures = {}, .warnings = {}, .parts = {}};
        }

        GltfModel model = GltfModel::Open(normalizedSource);

        ModelOptions modelOptions;
        modelOptions.textureFolder = options.textureFolder;
        modelOptions.registry = options.registry;
        modelOptions.projectDirectory = projectDirectory;

        // A skeletal mesh or animation is bound to the skeleton asked for, else the one it was bound
        // to before, else the skeleton an earlier import of the same source made.
        const auto skeletonFor = [&]() -> AssetReference {
            if (options.skeleton.IsSet()) { return options.skeleton; }
            if (AssetReference existing = ExistingSkeletonOf(destination, *type, projectDirectory); existing.IsSet()) {
                return existing;
            }
            if (options.registry != nullptr) {
                if (const AssetRegistry::Entry* const entry = options.registry->FindBySource(normalizedSource, "", AssetType::Skeleton)) {
                    return AssetReference{.id = entry->id, .path = entry->path};
                }
            }
            throw ImportError("'" + PathToUtf8(destination.filename()) +
                              "' needs a skeleton to bind to: import the model's skeleton too, or choose an existing one.");
        };

        switch (*type) {
            case AssetType::Mesh: modelOptions.mesh = destination; break;
            case AssetType::Skeleton: modelOptions.skeleton = destination; break;
            case AssetType::SkeletalMesh: {
                modelOptions.skeletalMesh = destination;
                modelOptions.existingSkeleton = skeletonFor();

                // The parts stay attached as they were, unless told otherwise.
                modelOptions.attachLooseParts = options.attachLooseParts;
                for (const auto& [part, attachment] :
                     GetPartAttachments(destination, projectDirectory).value_or(std::map<std::string, PartAttachment>{})) {
                    modelOptions.partBones[part] = attachment.bone;
                    if (attachment.separate) { modelOptions.separateParts.insert(part); }
                }
                for (const auto& [part, bone] : options.partBones) { modelOptions.partBones[part] = bone; }
                if (options.separateParts.has_value()) { modelOptions.separateParts = *options.separateParts; }
                break;
            }
            case AssetType::Animation: {
                std::string item = options.sourceItem;
                if (item.empty()) {
                    const std::optional<AssetHeader> existing = AssetFile::ReadHeader(destination, projectDirectory);
                    if (existing.has_value() && existing->type == AssetType::Animation) { item = existing->sourceItem; }
                }
                if (item.empty()) { throw ImportError("Choose which of the model's animations to import."); }

                const std::optional<std::size_t> index = FindAnimation(model.GetInfo(), item);
                if (!index.has_value()) {
                    throw ImportError("'" + PathToUtf8(normalizedSource.filename()) + "' has no animation '" +
                                      item.substr(item.find_first_of(":#") + 1) + "' (any more).");
                }
                modelOptions.animations.push_back(ModelOptions::Animation{.index = *index, .destination = destination});
                modelOptions.existingSkeleton = skeletonFor();
                break;
            }
            case AssetType::Texture:
            case AssetType::Sound: break;  // Handled above.
        }

        ModelResult imported = ImportModelFrom(model, normalizedSource, modelOptions);

        AssetReference asset;
        switch (*type) {
            case AssetType::Mesh: asset = imported.mesh; break;
            case AssetType::Skeleton: asset = imported.skeleton; break;
            case AssetType::SkeletalMesh: asset = imported.skeletalMesh; break;
            case AssetType::Animation: asset = imported.animations.front(); break;
            case AssetType::Texture:
            case AssetType::Sound: break;
        }
        return Result{.asset = std::move(asset),
                      .textures = std::move(imported.textures),
                      .warnings = std::move(imported.warnings),
                      .parts = std::move(imported.parts)};
    }

    AssetImporter::ModelResult AssetImporter::ImportModel(const std::filesystem::path& source, const ModelOptions& options) {
        const std::filesystem::path normalizedSource = AssetManager::NormalizePath(source);
        if (!IsModelSource(normalizedSource)) {
            throw ImportError("'" + PathToUtf8(normalizedSource) + "' is not a model (expected .gltf or .glb).");
        }

        GltfModel model = GltfModel::Open(normalizedSource);
        return ImportModelFrom(model, normalizedSource, options);
    }

    AssetImporter::Result AssetImporter::UpgradeLegacyAsset(const std::filesystem::path& asset,
                                                            const AssetRegistry* const registry) {
        const std::filesystem::path path = AssetManager::NormalizePath(asset);
        const std::filesystem::path projectDirectory = registry != nullptr ? registry->GetProjectDirectory() : std::filesystem::path{};
        const std::optional<AssetHeader> header = AssetFile::ReadHeader(path, projectDirectory);
        if (!header.has_value()) { throw ImportError("'" + PathToUtf8(path) + "' is not a Frost asset."); }

        if (AssetFile::HasId(header->type, header->version)) {
            return Result{.asset = {.id = header->id, .path = path}, .textures = {}, .warnings = {}, .parts = {}};
        }

        if (header->type == AssetType::Texture) {
            TextureFile file = AssetFile::ReadTexture(path, projectDirectory);
            file.header.id = AssetId::Generate();
            AssetFile::WriteTexture(path, file, projectDirectory);
            return Result{.asset = {.id = file.header.id, .path = path}, .textures = {}, .warnings = {}, .parts = {}};
        }

        MeshFile file = AssetFile::ReadMesh(path, projectDirectory);
        return UpgradeLegacyMesh(std::move(file.mesh), path, file.header.sourcePath, registry, projectDirectory);
    }

}  // namespace Frost
