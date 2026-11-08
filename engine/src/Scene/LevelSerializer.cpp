#include <Frost/Scene/LevelSerializer.hpp>

#include "../Assets/Json.hpp"

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Assets/AssetRegistry.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Platform/KeyNames.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Scene/Hierarchy.hpp>
#include <Frost/Scene/Level.hpp>
#include <Frost/Units.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

    using Frost::Json::Array;
    using Frost::Json::Object;
    using Frost::Json::Value;

    constexpr const char* kFormatName = "FrostLevel";
    // 15: audio source and audio listener components, including spatial settings, Doppler, and occlusion.
    // 14: physics query components.
    // 13: colliders' collision layers and masks.
    // 12: attachments to sockets.
    // 11: animation triggers' visibility keys.
    // 10: attachments (objects attached to objects), animation triggers, mesh visibility.
    // 9: skeletal meshes' playback settings and attachments toggle.
    // 8: skeletal mesh components.
    // 7: lengths (positions, sizes, distances) and speeds in centimeters, the world unit (see
    // Frost/Units.hpp); before, in meters (converted when read, see kMetersFormatVersion).
    // 6: capsule collider and character controller components; cameras have an offset and rotation.
    // 5: fly controller components. 4: camera components. 3: box collider and rigid body
    // components; an object can have a mesh component with no model yet ("mesh": {}). 2: asset
    // references are {"id", "path"} objects. 1: plain path strings. Older versions are still read.
    constexpr int kFormatVersion = 15;

    // The last version that stored lengths in meters.
    constexpr int kMetersFormatVersion = 6;


    // --- Writing -----------------------------------------------------------------------------------

    // A float as the double with the same shortest decimal form, so files read "0.1" rather than
    // "0.10000000149011612" (the exact double value of the float nearest 0.1).
    [[nodiscard]]
    Value ToJson(const float value) {
        char buffer[32];
        const std::to_chars_result written = std::to_chars(buffer, buffer + sizeof(buffer), value);

        double result = static_cast<double>(value);
        static_cast<void>(std::from_chars(buffer, written.ptr, result));
        return Value(result);
    }

    [[nodiscard]]
    Value ToJson(const glm::vec3& v) {
        return Value(Array{ToJson(v.x), ToJson(v.y), ToJson(v.z)});
    }

    [[nodiscard]]
    Value ToJson(const glm::vec4& v) {
        return Value(Array{ToJson(v.x), ToJson(v.y), ToJson(v.z), ToJson(v.w)});
    }

    // Relative to the level's directory. Always, when the level and the asset are both in the
    // project's content directory (they move together, however far apart they are); else only
    // when close to it, and absolute otherwise (see MakePortablePath).
    [[nodiscard]]
    Value StorePath(const std::filesystem::path& path, const std::filesystem::path& levelDirectory,
                    const Frost::AssetRegistry* const registry) {
        const bool sameProject = registry != nullptr && registry->Contains(path) &&
                                 (registry->Contains(levelDirectory) || levelDirectory == registry->GetRoot());
        const int maxParentSteps = sameProject ? std::numeric_limits<int>::max() : Frost::kMaxPortableParentSteps;
        return Value(Frost::GenericPathToUtf8(Frost::MakePortablePath(path, levelDirectory, maxParentSteps)));
    }

    // {"id": "...", "path": "..."}: the id is what the reference means; the path (where the asset
    // is now, if the registry knows, else where it was last seen) keeps the file readable and is
    // the fallback for assets the registry cannot see.
    [[nodiscard]]
    Value ReferenceToJson(const Frost::AssetReference& reference, const std::filesystem::path& levelDirectory,
                          const Frost::AssetRegistry* const registry) {
        std::filesystem::path path = reference.path;
        if (registry != nullptr) {
            if (const Frost::AssetRegistry::Entry* const entry = registry->Find(reference.id); entry != nullptr) {
                path = entry->path;
            }
        }

        Object json;
        if (reference.id.IsValid()) { json.emplace_back("id", Value(reference.id.ToString())); }
        if (!path.empty()) { json.emplace_back("path", StorePath(path, levelDirectory, registry)); }
        return Value(std::move(json));
    }

    // `model` is the drawn model the override applies to, if loaded (for its sub-mesh names).
    [[nodiscard]]
    Value MaterialOverrideToJson(const std::size_t subMeshIndex, const Frost::MeshAsset* const model,
                                 const Frost::MaterialOverride& materialOverride,
                                 const std::filesystem::path& levelDirectory, const Frost::AssetRegistry* const registry) {
        Object json;
        json.emplace_back("subMesh", Value(static_cast<double>(subMeshIndex)));

        // The name lets the override find its sub-mesh again if the model is re-exported with its
        // parts in a different order.
        if (model != nullptr) {
            const std::vector<Frost::SubMesh>& subMeshes = model->mesh->GetSubMeshes();
            if (subMeshIndex < subMeshes.size() && !subMeshes[subMeshIndex].name.empty()) {
                json.emplace_back("subMeshName", Value(subMeshes[subMeshIndex].name));
            }
        }

        const Frost::MaterialParameters& parameters = materialOverride.parameters;
        json.emplace_back("baseColorFactor", ToJson(parameters.baseColorFactor));
        json.emplace_back("metallic", ToJson(parameters.metallicFactor));
        json.emplace_back("roughness", ToJson(parameters.roughnessFactor));
        json.emplace_back("normalScale", ToJson(parameters.normalScale));
        json.emplace_back("occlusionStrength", ToJson(parameters.occlusionStrength));
        json.emplace_back("emissiveFactor", ToJson(parameters.emissiveFactor));

        Object textures;
        for (std::size_t slot = 0; slot < Frost::kMaterialSlotCount; ++slot) {
            const Frost::TextureOverride& texture = materialOverride.textures[slot];
            const char* const slotName = Frost::GetMaterialSlotName(static_cast<Frost::MaterialSlot>(slot));

            switch (texture.source) {
                case Frost::TextureOverride::Source::Inherit: break;  // Absent = inherit.
                case Frost::TextureOverride::Source::None: textures.emplace_back(slotName, Value()); break;
                case Frost::TextureOverride::Source::Asset:
                    textures.emplace_back(slotName, ReferenceToJson(texture.texture, levelDirectory, registry));
                    break;
            }
        }
        json.emplace_back("textures", Value(std::move(textures)));

        return Value(std::move(json));
    }

    [[nodiscard]]
    Value LightToJson(const Frost::LightComponent& light) {
        return Value(Object{{"type", Value(std::string(Frost::GetLightTypeName(light.type)))},
                            {"enabled", Value(light.enabled)},
                            {"color", ToJson(light.color)},
                            {"useTemperature", Value(light.useTemperature)},
                            {"temperature", ToJson(light.temperature)},
                            {"intensity", ToJson(light.intensity)},
                            {"range", ToJson(light.range)},
                            {"innerConeAngle", ToJson(light.innerConeAngle)},
                            {"outerConeAngle", ToJson(light.outerConeAngle)}});
    }

    // A collider's "layer" (0 to 31) and "mask" (the layers it collides with, as a 32-bit number:
    // bit n for layer n), added to its JSON.
    void AppendCollisionFilter(const Frost::CollisionFilter& filter, Object& json) {
        json.emplace_back("layer", Value(static_cast<double>(filter.layer)));
        json.emplace_back("mask", Value(static_cast<double>(filter.mask)));
    }

    // --- Reading -----------------------------------------------------------------------------------

    [[nodiscard]]
    std::optional<double> ReadNumber(const Value* const value) {
        if (value == nullptr || !value->IsNumber() || !std::isfinite(value->AsNumber())) { return std::nullopt; }
        return value->AsNumber();
    }

    [[nodiscard]]
    float ReadFloat(const Value& object, const std::string_view key, const float fallback) {
        const std::optional<double> number = ReadNumber(object.Find(key));
        return number.has_value() ? static_cast<float>(*number) : fallback;
    }

    // Reads `N` numbers of an array into a vector type, keeping `fallback` if the value is missing
    // or malformed.
    template <typename Vector, std::size_t N>
    [[nodiscard]]
    Vector ReadVector(const Value* const value, const Vector& fallback) {
        if (value == nullptr || !value->IsArray() || value->AsArray().size() != N) { return fallback; }

        Vector result = fallback;
        for (std::size_t i = 0; i < N; ++i) {
            const std::optional<double> number = ReadNumber(value->At(i));
            if (!number.has_value()) { return fallback; }
            result[static_cast<typename Vector::length_type>(i)] = static_cast<float>(*number);
        }
        return result;
    }

    [[nodiscard]]
    bool ReadBool(const Value& object, const std::string_view key, const bool fallback) {
        const Value* const value = object.Find(key);
        return value != nullptr && value->IsBoolean() ? value->AsBoolean() : fallback;
    }

    // Lengths and speeds are stored in centimeters from version 7, and were in meters before:
    // `scale` converts what the file says (1, or kMeter for older files). A missing or malformed
    // value keeps `fallback`, which is already in centimeters.
    [[nodiscard]]
    float ReadLength(const Value& object, const std::string_view key, const float fallback, const float scale) {
        const std::optional<double> number = ReadNumber(object.Find(key));
        return number.has_value() ? static_cast<float>(*number) * scale : fallback;
    }

    [[nodiscard]]
    glm::vec3 ReadLengthVector(const Value* const value, const glm::vec3& fallback, const float scale) {
        if (value == nullptr || !value->IsArray() || value->AsArray().size() != 3) { return fallback; }

        glm::vec3 result(0.0f);
        for (std::size_t i = 0; i < 3; ++i) {
            const std::optional<double> number = ReadNumber(value->At(i));
            if (!number.has_value()) { return fallback; }
            result[static_cast<glm::length_t>(i)] = static_cast<float>(*number) * scale;
        }
        return result;
    }

    // Nullopt (with a warning) for an unknown light type.
    [[nodiscard]]
    std::optional<Frost::LightComponent> ReadLight(const Value& json, const std::string& objectName,
                                                   const float lengthScale, std::vector<std::string>& warnings) {
        const Value* const typeName = json.Find("type");
        std::optional<Frost::LightType> type;
        for (const Frost::LightType candidate :
             {Frost::LightType::Directional, Frost::LightType::Point, Frost::LightType::Spot}) {
            if (typeName != nullptr && typeName->IsString() && typeName->AsString() == Frost::GetLightTypeName(candidate)) {
                type = candidate;
            }
        }
        if (!type.has_value()) {
            warnings.push_back("'" + objectName + "': dropped a light of unknown type.");
            return std::nullopt;
        }

        // Missing fields take the type's defaults.
        Frost::LightComponent light = Frost::LightComponent::CreateDefault(*type);
        light.enabled = ReadBool(json, "enabled", light.enabled);
        light.color = ReadVector<glm::vec3, 3>(json.Find("color"), light.color);
        light.useTemperature = ReadBool(json, "useTemperature", light.useTemperature);
        light.temperature = ReadFloat(json, "temperature", light.temperature);
        light.intensity = ReadFloat(json, "intensity", light.intensity);
        light.range = ReadLength(json, "range", light.range, lengthScale);
        light.innerConeAngle = ReadFloat(json, "innerConeAngle", light.innerConeAngle);
        light.outerConeAngle = ReadFloat(json, "outerConeAngle", light.outerConeAngle);
        return light;
    }

    // A collider's layer and mask (see AppendCollisionFilter). Missing (levels before version 13):
    // the defaults, the Default layer colliding with everything. Out of range: the default, with a
    // warning.
    [[nodiscard]]
    Frost::CollisionFilter ReadCollisionFilter(const Value& json, const std::string& objectName,
                                               std::vector<std::string>& warnings) {
        Frost::CollisionFilter filter;
        const auto readWhole = [&](const char* const key, const double max) -> std::optional<double> {
            const Value* const value = json.Find(key);
            if (value == nullptr) { return std::nullopt; }
            const std::optional<double> number = ReadNumber(value);
            if (!number.has_value() || *number < 0.0 || *number > max || std::floor(*number) != *number) {
                warnings.push_back("'" + objectName + "': a collider's " + key + " is not valid; using the default.");
                return std::nullopt;
            }
            return number;
        };

        if (const std::optional<double> layer = readWhole("layer", Frost::kCollisionLayerCount - 1)) {
            filter.layer = static_cast<int>(*layer);
        }
        if (const std::optional<double> mask = readWhole("mask", static_cast<double>(Frost::kAllCollisionLayers))) {
            filter.mask = static_cast<Frost::CollisionLayerMask>(*mask);
        }
        return filter;
    }

    [[nodiscard]]
    std::filesystem::path ResolvePath(const std::string& stored, const std::filesystem::path& levelDirectory) {
        return Frost::AssetManager::NormalizePath(Frost::ResolvePortablePath(Frost::PathFromUtf8(stored), levelDirectory));
    }

    // A reference in either format: a path string (version 1) or {"id", "path"}. Nullopt if the
    // value is neither (or names nothing).
    [[nodiscard]]
    std::optional<Frost::AssetReference> ReadReference(const Value& json, const std::filesystem::path& levelDirectory) {
        Frost::AssetReference reference;

        if (json.IsString()) {
            if (json.AsString().empty()) { return std::nullopt; }
            reference.path = ResolvePath(json.AsString(), levelDirectory);
            return reference;
        }
        if (!json.IsObject()) { return std::nullopt; }

        if (const Value* const id = json.Find("id"); id != nullptr && id->IsString()) {
            reference.id = Frost::AssetId::Parse(id->AsString()).value_or(Frost::AssetId{});
        }
        if (const Value* const path = json.Find("path"); path != nullptr && path->IsString() && !path->AsString().empty()) {
            reference.path = ResolvePath(path->AsString(), levelDirectory);
        }

        if (!reference.IsSet()) { return std::nullopt; }
        return reference;
    }

    [[nodiscard]]
    Frost::MaterialOverride ReadMaterialOverride(const Value& json, const std::filesystem::path& levelDirectory) {
        Frost::MaterialOverride result;
        Frost::MaterialParameters& parameters = result.parameters;

        parameters.baseColorFactor = ReadVector<glm::vec4, 4>(json.Find("baseColorFactor"), parameters.baseColorFactor);
        parameters.metallicFactor = ReadFloat(json, "metallic", parameters.metallicFactor);
        parameters.roughnessFactor = ReadFloat(json, "roughness", parameters.roughnessFactor);
        parameters.normalScale = ReadFloat(json, "normalScale", parameters.normalScale);
        parameters.occlusionStrength = ReadFloat(json, "occlusionStrength", parameters.occlusionStrength);
        parameters.emissiveFactor = ReadVector<glm::vec3, 3>(json.Find("emissiveFactor"), parameters.emissiveFactor);

        if (const Value* const textures = json.Find("textures"); textures != nullptr && textures->IsObject()) {
            for (std::size_t slot = 0; slot < Frost::kMaterialSlotCount; ++slot) {
                const Value* const texture =
                        textures->Find(Frost::GetMaterialSlotName(static_cast<Frost::MaterialSlot>(slot)));
                if (texture == nullptr) { continue; }  // Inherit.

                if (const std::optional<Frost::AssetReference> reference = ReadReference(*texture, levelDirectory)) {
                    result.textures[slot] =
                            Frost::TextureOverride{.source = Frost::TextureOverride::Source::Asset, .texture = *reference};
                }
                else {
                    result.textures[slot].source = Frost::TextureOverride::Source::None;
                }
            }
        }

        return result;
    }

    [[nodiscard]]
    std::string ReadFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) { throw Frost::LevelError("Could not open level '" + Frost::PathToUtf8(path) + "'."); }

        std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (file.bad()) { throw Frost::LevelError("Could not read level '" + Frost::PathToUtf8(path) + "'."); }
        return text;
    }

    // --- Components ----------------------------------------------------------------------------------

    struct WriteContext {
        const std::filesystem::path& levelDirectory;
        const Frost::AssetRegistry* registry;
    };

    struct ReadContext {
        const std::filesystem::path& levelDirectory;
        Frost::AssetManager& assets;
        std::vector<std::string>& warnings;
        // What the file's lengths and speeds are multiplied by to be in centimeters: 1, or kMeter
        // for a level from before version 7 (see ReadLength).
        float lengthScale;
    };

    // How one component type is stored in an object's JSON. Each specialization has:
    //   static void Write(const T& component, const Frost::GameObject& object, Object& json, const WriteContext&);
    //       Adds the component's key(s) to the object's JSON. Called only if the object has one.
    //   static void Read(const Value& json, Frost::GameObject& object, const ReadContext&);
    //       Attaches the component if the object's JSON has it (missing fields keep their
    //       defaults). Called for every object.
    // Every type in Frost::BuiltinComponents needs one (the build fails otherwise).
    template <typename T>
    struct ComponentSerializer;

    // The sub-mesh material overrides of a component that draws a model (MeshComponent,
    // SkeletalMeshComponent), as a "materials" array; `model` is what it draws, if loaded.
    template <typename Component>
    [[nodiscard]]
    Array MaterialOverridesToJson(const Component& component, const Frost::MeshAsset* const model, const WriteContext& context) {
        Array materials;
        for (std::size_t i = 0; i < component.GetMaterialSlotCount(); ++i) {
            const std::optional<Frost::MaterialOverride>& materialOverride = component.GetMaterialOverride(i);
            if (materialOverride.has_value()) {
                materials.push_back(MaterialOverrideToJson(i, model, *materialOverride, context.levelDirectory, context.registry));
            }
        }
        return materials;
    }

    // Reads a "materials" array (may be null) into a component's overrides, matching each to its
    // sub-mesh of `model` (if loaded) by name when it has one of that name, else by index.
    template <typename Component>
    void ReadMaterialOverrides(const Value* const materials, Component& component, const Frost::MeshAsset* const model,
                               const Frost::GameObject& object, const ReadContext& context) {
        if (materials == nullptr || !materials->IsArray()) { return; }

        for (const Value& materialJson : materials->AsArray()) {
            std::optional<std::size_t> index;
            const Value* const subMeshName = materialJson.Find("subMeshName");
            if (model != nullptr && subMeshName != nullptr && subMeshName->IsString()) {
                index = model->mesh->FindSubMesh(subMeshName->AsString());
            }
            if (!index.has_value()) {
                const std::optional<double> subMesh = ReadNumber(materialJson.Find("subMesh"));
                if (subMesh.has_value() && *subMesh >= 0.0 && std::floor(*subMesh) == *subMesh && *subMesh < 65536.0) {
                    index = static_cast<std::size_t>(*subMesh);
                }
            }

            const bool outOfRange = model != nullptr && index.has_value() && *index >= model->mesh->GetSubMeshes().size();
            if (!index.has_value() || outOfRange) {
                context.warnings.push_back("'" + object.GetName() +
                                           "': dropped a material override for a sub-mesh the model does not have.");
                continue;
            }

            component.SetMaterialOverride(*index, ReadMaterialOverride(materialJson, context.levelDirectory));
        }
    }

    // "transform": {"position": [x, y, z], "rotation": [pitch, yaw, roll] (degrees), "scale": [x, y, z]}
    template <>
    struct ComponentSerializer<Frost::TransformComponent> {
        static void Write(const Frost::TransformComponent& transform, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& /*context*/) {
            json.emplace_back("transform", Value(Object{{"position", ToJson(transform.position)},
                                                        {"rotation", ToJson(transform.rotation)},
                                                        {"scale", ToJson(transform.scale)}}));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const transform = json.Find("transform");
            if (transform == nullptr) { return; }  // Every object has one already; keep the default.

            Frost::TransformComponent& target = object.GetTransform();
            target.position = ReadLengthVector(transform->Find("position"), target.position, context.lengthScale);
            target.rotation = ReadVector<glm::vec3, 3>(transform->Find("rotation"), target.rotation);
            target.scale = ReadVector<glm::vec3, 3>(transform->Find("scale"), target.scale);
        }
    };

    // "mesh": {"id", "path"} (or {} for no model yet), and "materials": [...] for the sub-mesh
    // overrides (a separate key, as in versions 1 and 2), and "meshVisible": false for a hidden
    // model (absent = shown).
    template <>
    struct ComponentSerializer<Frost::MeshComponent> {
        static void Write(const Frost::MeshComponent& mesh, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& context) {
            json.emplace_back("mesh", mesh.GetMeshReference().IsSet()
                                              ? ReferenceToJson(mesh.GetMeshReference(), context.levelDirectory, context.registry)
                                              : Value(Object{}));

            Array materials = MaterialOverridesToJson(mesh, mesh.GetMesh().get(), context);
            if (!materials.empty()) { json.emplace_back("materials", Value(std::move(materials))); }
            if (!mesh.IsVisible()) { json.emplace_back("meshVisible", Value(false)); }
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const meshJson = json.Find("mesh");
            const Value* const materials = json.Find("materials");

            if (meshJson == nullptr) {
                if (materials != nullptr && materials->IsArray() && !materials->AsArray().empty()) {
                    context.warnings.push_back("'" + object.GetName() +
                                               "': dropped material overrides of an object that has no model.");
                }
                return;
            }

            Frost::MeshComponent& mesh = object.AddComponent<Frost::MeshComponent>();
            if (const std::optional<Frost::AssetReference> reference = ReadReference(*meshJson, context.levelDirectory)) {
                try {
                    mesh.SetMesh(context.assets.LoadMesh(*reference));
                }
                catch (const Frost::ImportError& e) {
                    mesh.SetUnresolvedMesh(*reference);
                    context.warnings.push_back("'" + object.GetName() + "': " + e.what());
                }
            }

            ReadMaterialOverrides(materials, mesh, mesh.GetMesh().get(), object, context);
            mesh.SetVisible(ReadBool(json, "meshVisible", true));
        }
    };

    // "skeletalMesh": {"mesh": {"id", "path"} (or {} for no model yet), "animation": {"id", "path"}
    // (absent = the rest pose), "animationTime": seconds, "autoPlay": b, "loop": b, "playRate": r,
    // "attachmentsFollowBones": b, "visible": b, "materials": [...] (sub-mesh overrides, as for
    // "mesh")}. Missing settings take their defaults (version 8 had none: on, on, 1, on; before
    // version 10 there was no "visible": on).
    template <>
    struct ComponentSerializer<Frost::SkeletalMeshComponent> {
        static void Write(const Frost::SkeletalMeshComponent& mesh, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& context) {
            Object skeletal;
            skeletal.emplace_back("mesh", mesh.GetSkeletalMeshReference().IsSet()
                                                  ? ReferenceToJson(mesh.GetSkeletalMeshReference(), context.levelDirectory,
                                                                    context.registry)
                                                  : Value(Object{}));
            if (mesh.GetAnimationReference().IsSet()) {
                skeletal.emplace_back("animation",
                                      ReferenceToJson(mesh.GetAnimationReference(), context.levelDirectory, context.registry));
            }
            skeletal.emplace_back("animationTime", ToJson(mesh.GetAnimationTime()));
            skeletal.emplace_back("autoPlay", Value(mesh.GetAutoPlay()));
            skeletal.emplace_back("loop", Value(mesh.IsLooping()));
            skeletal.emplace_back("playRate", ToJson(mesh.GetPlayRate()));
            skeletal.emplace_back("attachmentsFollowBones", Value(mesh.GetAttachmentsFollowBones()));
            skeletal.emplace_back("visible", Value(mesh.IsVisible()));

            const Frost::MeshAsset* const model = mesh.GetDrawable().get();
            Array materials = MaterialOverridesToJson(mesh, model, context);
            if (!materials.empty()) { skeletal.emplace_back("materials", Value(std::move(materials))); }

            json.emplace_back("skeletalMesh", Value(std::move(skeletal)));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const skeletal = json.Find("skeletalMesh");
            if (skeletal == nullptr || !skeletal->IsObject()) { return; }

            Frost::SkeletalMeshComponent& mesh = object.AddComponent<Frost::SkeletalMeshComponent>();
            if (const Value* const meshJson = skeletal->Find("mesh")) {
                if (const std::optional<Frost::AssetReference> reference = ReadReference(*meshJson, context.levelDirectory)) {
                    try {
                        mesh.SetSkeletalMesh(context.assets.LoadSkeletalMesh(*reference));
                    }
                    catch (const Frost::ImportError& e) {
                        mesh.SetUnresolvedSkeletalMesh(*reference);
                        context.warnings.push_back("'" + object.GetName() + "': " + e.what());
                    }
                }
            }

            if (const Value* const animationJson = skeletal->Find("animation")) {
                if (const std::optional<Frost::AssetReference> reference = ReadReference(*animationJson, context.levelDirectory)) {
                    try {
                        mesh.SetAnimation(context.assets.LoadAnimation(*reference));
                    }
                    catch (const Frost::ImportError& e) {
                        mesh.SetUnresolvedAnimation(*reference);
                        context.warnings.push_back("'" + object.GetName() + "': " + e.what());
                    }
                }
            }
            mesh.SetAnimationTime(ReadFloat(*skeletal, "animationTime", 0.0f));
            mesh.SetAutoPlay(ReadBool(*skeletal, "autoPlay", mesh.GetAutoPlay()));
            mesh.SetLooping(ReadBool(*skeletal, "loop", mesh.IsLooping()));
            mesh.SetPlayRate(ReadFloat(*skeletal, "playRate", mesh.GetPlayRate()));
            mesh.SetAttachmentsFollowBones(ReadBool(*skeletal, "attachmentsFollowBones", mesh.GetAttachmentsFollowBones()));
            mesh.SetVisible(ReadBool(*skeletal, "visible", mesh.IsVisible()));

            const Frost::MeshAsset* const model = mesh.GetDrawable().get();
            ReadMaterialOverrides(skeletal->Find("materials"), mesh, model, object, context);
        }
    };

    // "attachment": {"parent": object id, "bone": name, "slot": name, "socket": name}. A parent that is not in the
    // level is dropped after loading (see Load).
    template <>
    struct ComponentSerializer<Frost::AttachmentComponent> {
        static void Write(const Frost::AttachmentComponent& attachment, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& /*context*/) {
            json.emplace_back("attachment", Value(Object{{"parent", Value(static_cast<double>(attachment.parent))},
                                                         {"bone", Value(attachment.bone)},
                                                         {"slot", Value(attachment.slot)},
                                                         {"socket", Value(attachment.socket)}}));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const attachmentJson = json.Find("attachment");
            if (attachmentJson == nullptr || !attachmentJson->IsObject()) { return; }

            Frost::AttachmentComponent attachment;
            const std::optional<double> parent = ReadNumber(attachmentJson->Find("parent"));
            if (parent.has_value() && *parent >= 1.0 && *parent <= 9007199254740992.0 && std::floor(*parent) == *parent) {
                attachment.parent = static_cast<Frost::EntityId>(*parent);
            }
            else if (parent.has_value() && *parent != 0.0) {
                context.warnings.push_back("'" + object.GetName() + "': ignored an attachment to an invalid object id.");
            }
            if (const Value* const bone = attachmentJson->Find("bone"); bone != nullptr && bone->IsString()) {
                attachment.bone = bone->AsString();
            }
            if (const Value* const slot = attachmentJson->Find("slot"); slot != nullptr && slot->IsString()) {
                attachment.slot = slot->AsString();
            }
            if (const Value* const socket = attachmentJson->Find("socket"); socket != nullptr && socket->IsString()) {
                attachment.socket = socket->AsString();
            }
            object.AddComponent<Frost::AttachmentComponent>(std::move(attachment));
        }
    };

    // "animationTriggers": [{"key": key name (see KeyNames), "animation": {"id", "path"},
    //                        "visibilityKeys": [{"time": seconds, "object": object id, "visible": b}, ...]}, ...]
    // Keys naming objects that are not in the level are dropped after loading (see Load).
    template <>
    struct ComponentSerializer<Frost::AnimationTriggerComponent> {
        static void Write(const Frost::AnimationTriggerComponent& component, const Frost::GameObject& /*object*/,
                          Object& json, const WriteContext& context) {
            Array triggers;
            for (const Frost::AnimationTrigger& trigger : component.triggers) {
                Object triggerJson;
                triggerJson.emplace_back("key", Value(std::string(Frost::GetKeyName(trigger.key))));
                triggerJson.emplace_back("animation", trigger.animationReference.IsSet()
                                                              ? ReferenceToJson(trigger.animationReference,
                                                                                context.levelDirectory, context.registry)
                                                              : Value(Object{}));
                Array keys;
                for (const Frost::TriggerVisibilityKey& key : trigger.visibilityKeys) {
                    keys.emplace_back(Object{{"time", ToJson(key.time)},
                                             {"object", Value(static_cast<double>(key.object))},
                                             {"visible", Value(key.visible)}});
                }
                triggerJson.emplace_back("visibilityKeys", Value(std::move(keys)));
                triggers.emplace_back(std::move(triggerJson));
            }
            json.emplace_back("animationTriggers", Value(std::move(triggers)));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const triggersJson = json.Find("animationTriggers");
            if (triggersJson == nullptr || !triggersJson->IsArray()) { return; }

            Frost::AnimationTriggerComponent component;
            for (const Value& triggerJson : triggersJson->AsArray()) {
                if (!triggerJson.IsObject()) { continue; }

                const Value* const keyJson = triggerJson.Find("key");
                const std::optional<Frost::Key> key =
                        keyJson != nullptr && keyJson->IsString() ? Frost::FindKey(keyJson->AsString()) : std::nullopt;
                if (!key.has_value()) {
                    context.warnings.push_back("'" + object.GetName() + "': dropped an animation trigger with an unknown key.");
                    continue;
                }

                Frost::AnimationTrigger trigger{.key = *key, .animationReference = {}, .animation = nullptr, .visibilityKeys = {}};
                if (const Value* const animationJson = triggerJson.Find("animation")) {
                    if (const std::optional<Frost::AssetReference> reference = ReadReference(*animationJson, context.levelDirectory)) {
                        trigger.animationReference = *reference;
                        try {
                            trigger.animation = context.assets.LoadAnimation(*reference);
                        }
                        catch (const Frost::ImportError& e) {
                            context.warnings.push_back("'" + object.GetName() + "': " + e.what());
                        }
                    }
                }

                bool droppedKeys = false;
                if (const Value* const keys = triggerJson.Find("visibilityKeys"); keys != nullptr && keys->IsArray()) {
                    for (const Value& visibilityKey : keys->AsArray()) {
                        const std::optional<double> time = visibilityKey.IsObject() ? ReadNumber(visibilityKey.Find("time")) : std::nullopt;
                        const std::optional<double> target =
                                visibilityKey.IsObject() ? ReadNumber(visibilityKey.Find("object")) : std::nullopt;
                        if (!time.has_value() || !std::isfinite(*time) || *time < 0.0 || !target.has_value() || *target < 1.0 ||
                            *target > 9007199254740992.0 || std::floor(*target) != *target) {
                            droppedKeys = true;
                            continue;
                        }
                        trigger.visibilityKeys.push_back(Frost::TriggerVisibilityKey{.time = static_cast<float>(*time),
                                                                                     .object = static_cast<Frost::EntityId>(*target),
                                                                                     .visible = ReadBool(visibilityKey, "visible", true)});
                    }
                }
                if (droppedKeys) {
                    context.warnings.push_back("'" + object.GetName() + "': dropped invalid visibility keys of an animation trigger.");
                }
                trigger.SortVisibilityKeys();
                component.triggers.push_back(std::move(trigger));
            }
            object.AddComponent<Frost::AnimationTriggerComponent>(std::move(component));
        }
    };

    // "light": {"type", "enabled", "color", ...} (see LightToJson).
    template <>
    struct ComponentSerializer<Frost::LightComponent> {
        static void Write(const Frost::LightComponent& light, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& /*context*/) {
            json.emplace_back("light", LightToJson(light));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const light = json.Find("light");
            if (light == nullptr || !light->IsObject()) { return; }

            const std::optional<Frost::LightComponent> component =
                    ReadLight(*light, object.GetName(), context.lengthScale, context.warnings);
            if (component.has_value()) {
                object.AddComponent<Frost::LightComponent>(*component);
            }
        }
    };

    // "camera": {"verticalFov": degrees, "nearClip": n, "farClip": f, "offset": [x, y, z],
    //            "rotation": [pitch, yaw, roll] (degrees)}
    template <>
    struct ComponentSerializer<Frost::CameraComponent> {
        static void Write(const Frost::CameraComponent& camera, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& /*context*/) {
            json.emplace_back("camera", Value(Object{{"verticalFov", ToJson(camera.verticalFov)},
                                                     {"nearClip", ToJson(camera.nearClip)},
                                                     {"farClip", ToJson(camera.farClip)},
                                                     {"offset", ToJson(camera.offset)},
                                                     {"rotation", ToJson(camera.rotation)}}));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const camera = json.Find("camera");
            if (camera == nullptr || !camera->IsObject()) { return; }

            Frost::CameraComponent component;
            component.verticalFov = ReadFloat(*camera, "verticalFov", component.verticalFov);
            component.nearClip = ReadLength(*camera, "nearClip", component.nearClip, context.lengthScale);
            component.farClip = ReadLength(*camera, "farClip", component.farClip, context.lengthScale);
            component.offset = ReadLengthVector(camera->Find("offset"), component.offset, context.lengthScale);
            component.rotation = ReadVector<glm::vec3, 3>(camera->Find("rotation"), component.rotation);
            object.AddComponent<Frost::CameraComponent>(component);
        }
    };

    // "audioSource": {"sound": {"id", "path"} (or {} for no sound yet), "volume": gain, "pitch": p,
    //                 "loop": b, "playOnStart": b, "spatial": b, "minDistance": cm, "maxDistance": cm,
    //                 "rolloff": r, "attenuation": "none" / "inverse" / "linear" / "exponential",
    //                 "doppler": factor, "occlusion": amount}
    template <>
    struct ComponentSerializer<Frost::AudioSourceComponent> {
        static void Write(const Frost::AudioSourceComponent& source, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& context) {
            json.emplace_back("audioSource",
                              Value(Object{{"sound", source.soundReference.IsSet()
                                                             ? ReferenceToJson(source.soundReference, context.levelDirectory,
                                                                               context.registry)
                                                             : Value(Object{})},
                                           {"volume", ToJson(source.volume)},
                                           {"pitch", ToJson(source.pitch)},
                                           {"loop", Value(source.loop)},
                                           {"playOnStart", Value(source.playOnStart)},
                                           {"spatial", Value(source.spatial)},
                                           {"minDistance", ToJson(source.minDistance)},
                                           {"maxDistance", ToJson(source.maxDistance)},
                                           {"rolloff", ToJson(source.rolloff)},
                                           {"attenuation", Value(std::string(Frost::GetAudioAttenuationName(source.attenuation)))},
                                           {"doppler", ToJson(source.doppler)},
                                           {"occlusion", ToJson(source.occlusion)}}));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const audio = json.Find("audioSource");
            if (audio == nullptr || !audio->IsObject()) { return; }

            Frost::AudioSourceComponent component;
            if (const Value* const soundJson = audio->Find("sound")) {
                if (const std::optional<Frost::AssetReference> reference = ReadReference(*soundJson, context.levelDirectory)) {
                    try {
                        component.SetSound(context.assets.LoadSound(*reference));
                    }
                    catch (const Frost::ImportError& e) {
                        component.SetUnresolvedSound(*reference);
                        context.warnings.push_back("'" + object.GetName() + "': " + e.what());
                    }
                }
            }
            component.volume = ReadFloat(*audio, "volume", component.volume);
            component.pitch = ReadFloat(*audio, "pitch", component.pitch);
            component.loop = ReadBool(*audio, "loop", component.loop);
            component.playOnStart = ReadBool(*audio, "playOnStart", component.playOnStart);
            component.spatial = ReadBool(*audio, "spatial", component.spatial);
            component.minDistance = ReadLength(*audio, "minDistance", component.minDistance, context.lengthScale);
            component.maxDistance = ReadLength(*audio, "maxDistance", component.maxDistance, context.lengthScale);
            component.rolloff = ReadFloat(*audio, "rolloff", component.rolloff);
            component.doppler = ReadFloat(*audio, "doppler", component.doppler);
            component.occlusion = ReadFloat(*audio, "occlusion", component.occlusion);

            if (const Value* const attenuation = audio->Find("attenuation"); attenuation != nullptr) {
                bool known = false;
                for (const Frost::AudioAttenuation candidate : {Frost::AudioAttenuation::None, Frost::AudioAttenuation::Inverse,
                                                                Frost::AudioAttenuation::Linear, Frost::AudioAttenuation::Exponential}) {
                    if (attenuation->IsString() && attenuation->AsString() == Frost::GetAudioAttenuationName(candidate)) {
                        component.attenuation = candidate;
                        known = true;
                    }
                }
                if (!known) {
                    context.warnings.push_back("'" + object.GetName() + "': unknown audio attenuation; using \"" +
                                               Frost::GetAudioAttenuationName(component.attenuation) + "\".");
                }
            }
            object.AddComponent<Frost::AudioSourceComponent>(std::move(component));
        }
    };

    // "audioListener": {"active": b, "offset": [x, y, z]}. Only one listener of a level is active
    // (see Load).
    template <>
    struct ComponentSerializer<Frost::AudioListenerComponent> {
        static void Write(const Frost::AudioListenerComponent& listener, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& /*context*/) {
            json.emplace_back("audioListener", Value(Object{{"active", Value(listener.active)}, {"offset", ToJson(listener.offset)}}));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const listener = json.Find("audioListener");
            if (listener == nullptr || !listener->IsObject()) { return; }

            Frost::AudioListenerComponent component;
            component.active = ReadBool(*listener, "active", component.active);
            component.offset = ReadLengthVector(listener->Find("offset"), component.offset, context.lengthScale);
            object.AddComponent<Frost::AudioListenerComponent>(component);
        }
    };

    // "flyController": {"moveSpeed": units/s, "fastMultiplier": f, "lookSensitivity": degrees/unit}
    template <>
    struct ComponentSerializer<Frost::FlyControllerComponent> {
        static void Write(const Frost::FlyControllerComponent& controller, const Frost::GameObject& /*object*/,
                          Object& json, const WriteContext& /*context*/) {
            json.emplace_back("flyController", Value(Object{{"moveSpeed", ToJson(controller.moveSpeed)},
                                                            {"fastMultiplier", ToJson(controller.fastMultiplier)},
                                                            {"lookSensitivity", ToJson(controller.lookSensitivity)}}));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const controller = json.Find("flyController");
            if (controller == nullptr || !controller->IsObject()) { return; }

            Frost::FlyControllerComponent component;
            component.moveSpeed = ReadLength(*controller, "moveSpeed", component.moveSpeed, context.lengthScale);
            component.fastMultiplier = ReadFloat(*controller, "fastMultiplier", component.fastMultiplier);
            component.lookSensitivity = ReadFloat(*controller, "lookSensitivity", component.lookSensitivity);
            object.AddComponent<Frost::FlyControllerComponent>(component);
        }
    };

    // "characterController": {"walkSpeed": units/s, "jumpSpeed": units/s, "lookSensitivity": degrees/unit}
    template <>
    struct ComponentSerializer<Frost::CharacterControllerComponent> {
        static void Write(const Frost::CharacterControllerComponent& controller, const Frost::GameObject& /*object*/,
                          Object& json, const WriteContext& /*context*/) {
            json.emplace_back("characterController",
                              Value(Object{{"walkSpeed", ToJson(controller.walkSpeed)},
                                           {"jumpSpeed", ToJson(controller.jumpSpeed)},
                                           {"lookSensitivity", ToJson(controller.lookSensitivity)}}));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const controller = json.Find("characterController");
            if (controller == nullptr || !controller->IsObject()) { return; }

            Frost::CharacterControllerComponent component;
            component.walkSpeed = ReadLength(*controller, "walkSpeed", component.walkSpeed, context.lengthScale);
            component.jumpSpeed = ReadLength(*controller, "jumpSpeed", component.jumpSpeed, context.lengthScale);
            component.lookSensitivity = ReadFloat(*controller, "lookSensitivity", component.lookSensitivity);
            object.AddComponent<Frost::CharacterControllerComponent>(component);
        }
    };

    // "capsuleCollider": {"center": [x, y, z], "radius": r, "height": h, "layer": n, "mask": m}
    template <>
    struct ComponentSerializer<Frost::CapsuleColliderComponent> {
        static void Write(const Frost::CapsuleColliderComponent& collider, const Frost::GameObject& /*object*/,
                          Object& json, const WriteContext& /*context*/) {
            Object capsule{{"center", ToJson(collider.center)},
                           {"radius", ToJson(collider.radius)},
                           {"height", ToJson(collider.height)}};
            AppendCollisionFilter(collider.filter, capsule);
            json.emplace_back("capsuleCollider", Value(std::move(capsule)));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const collider = json.Find("capsuleCollider");
            if (collider == nullptr || !collider->IsObject()) { return; }

            Frost::CapsuleColliderComponent component;
            component.center = ReadLengthVector(collider->Find("center"), component.center, context.lengthScale);
            component.radius = ReadLength(*collider, "radius", component.radius, context.lengthScale);
            component.height = ReadLength(*collider, "height", component.height, context.lengthScale);
            component.filter = ReadCollisionFilter(*collider, object.GetName(), context.warnings);
            object.AddComponent<Frost::CapsuleColliderComponent>(component);
        }
    };

    // "boxCollider": {"center": [x, y, z], "size": [x, y, z], "layer": n, "mask": m}
    template <>
    struct ComponentSerializer<Frost::BoxColliderComponent> {
        static void Write(const Frost::BoxColliderComponent& collider, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& /*context*/) {
            Object box{{"center", ToJson(collider.center)}, {"size", ToJson(collider.size)}};
            AppendCollisionFilter(collider.filter, box);
            json.emplace_back("boxCollider", Value(std::move(box)));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const collider = json.Find("boxCollider");
            if (collider == nullptr || !collider->IsObject()) { return; }

            Frost::BoxColliderComponent component;
            component.center = ReadLengthVector(collider->Find("center"), component.center, context.lengthScale);
            component.size = ReadLengthVector(collider->Find("size"), component.size, context.lengthScale);
            component.filter = ReadCollisionFilter(*collider, object.GetName(), context.warnings);
            object.AddComponent<Frost::BoxColliderComponent>(component);
        }
    };

    // "rigidBody": {"simulated": b, "mass": kg}. The velocity is simulation state and not saved.
    template <>
    struct ComponentSerializer<Frost::RigidBodyComponent> {
        static void Write(const Frost::RigidBodyComponent& body, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& /*context*/) {
            json.emplace_back("rigidBody", Value(Object{{"simulated", Value(body.simulated)}, {"mass", ToJson(body.mass)}}));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& /*context*/) {
            const Value* const body = json.Find("rigidBody");
            if (body == nullptr || !body->IsObject()) { return; }

            Frost::RigidBodyComponent component;
            component.simulated = ReadBool(*body, "simulated", component.simulated);
            component.mass = ReadFloat(*body, "mass", component.mass);
            object.AddComponent<Frost::RigidBodyComponent>(component);
        }
    };

    // "physicsQuery": {"type": "raycast" | "sphereCast" | "capsuleCast" | "sphereOverlap" | "boxOverlap",
    //                  "mode": "closest" | "all", "enabled": b, "debug": b, "offset": [x, y, z],
    //                  "direction": [x, y, z], "maxDistance": d, "radius": r, "height": h,
    //                  "boxSize": [x, y, z], "layers": mask, "ignoreSelf": b}
    // The results are not saved. An unknown type or mode is read as the default, with a warning.
    template <>
    struct ComponentSerializer<Frost::PhysicsQueryComponent> {
        static void Write(const Frost::PhysicsQueryComponent& query, const Frost::GameObject& /*object*/, Object& json,
                          const WriteContext& /*context*/) {
            json.emplace_back("physicsQuery",
                              Value(Object{{"type", Value(std::string(Frost::GetPhysicsQueryTypeName(query.type)))},
                                           {"mode", Value(std::string(Frost::GetPhysicsQueryModeName(query.mode)))},
                                           {"enabled", Value(query.enabled)},
                                           {"debug", Value(query.debug)},
                                           {"offset", ToJson(query.offset)},
                                           {"direction", ToJson(query.direction)},
                                           {"maxDistance", ToJson(query.maxDistance)},
                                           {"radius", ToJson(query.radius)},
                                           {"height", ToJson(query.height)},
                                           {"boxSize", ToJson(query.boxSize)},
                                           {"layers", Value(static_cast<double>(query.layers))},
                                           {"ignoreSelf", Value(query.ignoreSelf)}}));
        }

        static void Read(const Value& json, Frost::GameObject& object, const ReadContext& context) {
            const Value* const query = json.Find("physicsQuery");
            if (query == nullptr || !query->IsObject()) { return; }

            Frost::PhysicsQueryComponent component;
            const auto readName = [&](const char* const key, auto parse, auto& target) {
                const Value* const value = query->Find(key);
                if (value == nullptr) { return; }
                if (value->IsString()) {
                    if (const auto parsed = parse(value->AsString())) {
                        target = *parsed;
                        return;
                    }
                }
                context.warnings.push_back("'" + object.GetName() + "': a physics query's " + key +
                                           " is not known; using the default.");
            };
            readName("type", Frost::ParsePhysicsQueryType, component.type);
            readName("mode", Frost::ParsePhysicsQueryMode, component.mode);
            component.enabled = ReadBool(*query, "enabled", component.enabled);
            component.debug = ReadBool(*query, "debug", component.debug);
            component.offset = ReadLengthVector(query->Find("offset"), component.offset, context.lengthScale);
            component.direction = ReadVector<glm::vec3, 3>(query->Find("direction"), component.direction);
            component.maxDistance = ReadLength(*query, "maxDistance", component.maxDistance, context.lengthScale);
            component.radius = ReadLength(*query, "radius", component.radius, context.lengthScale);
            component.height = ReadLength(*query, "height", component.height, context.lengthScale);
            component.boxSize = ReadLengthVector(query->Find("boxSize"), component.boxSize, context.lengthScale);
            if (const std::optional<double> layers = ReadNumber(query->Find("layers"));
                layers.has_value() && *layers >= 0.0 && *layers <= static_cast<double>(Frost::kAllCollisionLayers) &&
                std::floor(*layers) == *layers) {
                component.layers = static_cast<Frost::CollisionLayerMask>(*layers);
            }
            else if (query->Find("layers") != nullptr) {
                context.warnings.push_back("'" + object.GetName() + "': a physics query's layers are not valid; using all.");
            }
            component.ignoreSelf = ReadBool(*query, "ignoreSelf", component.ignoreSelf);
            object.AddComponent<Frost::PhysicsQueryComponent>(component);
        }
    };

    [[nodiscard]]
    Value ObjectToJson(const Frost::GameObject& object, const WriteContext& context) {
        Object json;
        json.emplace_back("id", Value(static_cast<double>(object.GetId())));
        json.emplace_back("name", Value(object.GetName()));

        Frost::BuiltinComponents::ForEach([&]<typename T>() {
            if (const T* const component = object.TryGetComponent<T>(); component != nullptr) {
                ComponentSerializer<T>::Write(*component, object, json, context);
            }
        });

        return Value(std::move(json));
    }

    void ReadComponents(const Value& json, Frost::GameObject& object, const ReadContext& context) {
        Frost::BuiltinComponents::ForEach([&]<typename T>() { ComponentSerializer<T>::Read(json, object, context); });
    }

}  // namespace

namespace Frost {

    void LevelSerializer::Save(const Level& level, const std::filesystem::path& path, const AssetRegistry* const registry) {
        const std::filesystem::path target = AssetManager::NormalizePath(path);
        const std::filesystem::path directory = target.parent_path();

        const WriteContext context{.levelDirectory = directory, .registry = registry};

        Array objects;
        for (const std::unique_ptr<GameObject>& object : level.GetObjects()) {
            objects.push_back(ObjectToJson(*object, context));
        }

        const LevelEnvironment& environment = level.GetEnvironment();

        const Value root(Object{{"format", Value(std::string(kFormatName))},
                                {"version", Value(static_cast<double>(kFormatVersion))},
                                {"name", Value(level.GetName())},
                                {"environment", Value(Object{{"ambientColor", ToJson(environment.ambientColor)},
                                                             {"ambientIntensity", ToJson(environment.ambientIntensity)}})},
                                {"objects", Value(std::move(objects))}});
        const std::string text = Json::Write(root);

        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            throw LevelError("Could not create directory '" + PathToUtf8(directory) + "': " + error.message());
        }

        // Write next to the target and rename over it: a crash or full disk mid-write leaves the
        // previous version intact instead of a truncated file.
        std::filesystem::path temporary = target;
        temporary += ".tmp";

        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.close();

            if (!file) {
                std::filesystem::remove(temporary, error);
                throw LevelError("Could not write level '" + PathToUtf8(target) + "'.");
            }
        }

        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw LevelError("Could not replace level '" + PathToUtf8(target) + "'.");
        }
    }

    LevelLoadResult LevelSerializer::Load(const std::filesystem::path& path, AssetManager& assets) {
        const std::filesystem::path source = AssetManager::NormalizePath(path);
        const std::filesystem::path directory = source.parent_path();
        const std::string where = "Level '" + PathToUtf8(source) + "'";

        Value root;
        try {
            root = Json::Parse(ReadFile(source));
        }
        catch (const LevelError&) {
            throw;
        }
        catch (const std::exception& e) {
            throw LevelError(where + " is not valid JSON: " + e.what());
        }

        const Value* const format = root.Find("format");
        if (format == nullptr || !format->IsString() || format->AsString() != kFormatName) {
            throw LevelError(where + " is not a Frost level file.");
        }

        const std::optional<double> version = ReadNumber(root.Find("version"));
        if (!version.has_value() || *version < 1.0) { throw LevelError(where + " has no valid format version."); }
        if (*version > kFormatVersion) {
            std::ostringstream message;
            message << where << " uses format version " << *version << ", newer than this build supports ("
                    << kFormatVersion << ").";
            throw LevelError(message.str());
        }

        LevelLoadResult result;

        const Value* const name = root.Find("name");
        result.level = std::make_unique<Level>(name != nullptr && name->IsString() ? name->AsString()
                                                                                 : PathToUtf8(source.stem()));

        if (const Value* const environment = root.Find("environment"); environment != nullptr && environment->IsObject()) {
            LevelEnvironment& target = result.level->GetEnvironment();
            target.ambientColor = ReadVector<glm::vec3, 3>(environment->Find("ambientColor"), target.ambientColor);
            target.ambientIntensity = ReadFloat(*environment, "ambientIntensity", target.ambientIntensity);
        }

        const Value* const objects = root.Find("objects");
        if (objects == nullptr || !objects->IsArray()) { return result; }  // An empty level.

        // Saved ids are kept (other data may refer to objects by id later); missing, malformed or
        // duplicate ones get fresh ids after every valid one has been placed.
        std::set<ObjectId> usedIds;
        for (const Value& json : objects->AsArray()) {
            const std::optional<double> id = ReadNumber(json.Find("id"));
            if (id.has_value() && *id >= 1.0 && *id <= 9007199254740992.0 && std::floor(*id) == *id) {
                usedIds.insert(static_cast<ObjectId>(*id));
            }
        }
        ObjectId nextFreeId = usedIds.empty() ? 1 : *usedIds.rbegin() + 1;
        std::set<ObjectId> placedIds;

        const ReadContext readContext{.levelDirectory = directory,
                                      .assets = assets,
                                      .warnings = result.warnings,
                                      .lengthScale = *version <= kMetersFormatVersion ? kMeter : 1.0f};

        for (const Value& json : objects->AsArray()) {
            if (!json.IsObject()) {
                result.warnings.push_back("Skipped an object entry that is not a JSON object.");
                continue;
            }

            ObjectId id = 0;
            const std::optional<double> savedId = ReadNumber(json.Find("id"));
            if (savedId.has_value() && *savedId >= 1.0 && std::floor(*savedId) == *savedId &&
                *savedId <= 9007199254740992.0 && !placedIds.contains(static_cast<ObjectId>(*savedId))) {
                id = static_cast<ObjectId>(*savedId);
            }
            else {
                id = nextFreeId++;
            }
            placedIds.insert(id);

            const Value* const objectName = json.Find("name");
            GameObject& object = result.level->AddObject(
                    id, objectName != nullptr && objectName->IsString() ? objectName->AsString() : "Object");

            ReadComponents(json, object, readContext);
        }

        // The game hears through one listener: of several active ones, the first (in the level's
        // order) stays active.
        const GameObject* activeListener = nullptr;
        std::size_t deactivated = 0;
        for (const std::unique_ptr<GameObject>& object : result.level->GetObjects()) {
            AudioListenerComponent* const listener = object->TryGetComponent<AudioListenerComponent>();
            if (listener == nullptr || !listener->active) { continue; }
            if (activeListener == nullptr) { activeListener = object.get(); }
            else {
                listener->active = false;
                ++deactivated;
            }
        }
        if (deactivated > 0) {
            result.warnings.push_back(std::to_string(deactivated + 1) + " objects had an active audio listener; the game hears "
                                      "through one at a time, so only the one on '" + activeListener->GetName() + "' is active now.");
        }

        // Attachments to objects that are not in the level, or that would loop, are dropped, and so
        // are animation triggers' keys naming objects that are not in it.
        Registry& registry = result.level->GetRegistry();
        for (const std::unique_ptr<GameObject>& object : result.level->GetObjects()) {
            if (AnimationTriggerComponent* const triggers = object->TryGetComponent<AnimationTriggerComponent>()) {
                std::size_t dropped = 0;
                for (AnimationTrigger& trigger : triggers->triggers) {
                    dropped += std::erase_if(trigger.visibilityKeys, [&](const TriggerVisibilityKey& key) {
                        return result.level->FindObject(key.object) == nullptr;
                    });
                }
                if (dropped > 0) {
                    result.warnings.push_back("'" + object->GetName() + "': dropped " + std::to_string(dropped) +
                                              " animation trigger visibility key(s) for objects that are not in the level.");
                }
            }

            AttachmentComponent* const attachment = object->TryGetComponent<AttachmentComponent>();
            if (attachment == nullptr || attachment->parent == 0) { continue; }

            if (result.level->FindObject(attachment->parent) == nullptr) {
                result.warnings.push_back("'" + object->GetName() + "' was attached to an object that is not in the level; it is no longer attached.");
                attachment->parent = 0;
            }
            else if (!CanAttach(registry, object->GetId(), attachment->parent)) {
                result.warnings.push_back("'" + object->GetName() + "' was attached in a loop; it is no longer attached.");
                attachment->parent = 0;
            }
        }

        return result;
    }

}  // namespace Frost
