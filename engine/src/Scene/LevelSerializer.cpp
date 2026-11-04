#include <Frost/Scene/LevelSerializer.hpp>

#include "../Assets/Json.hpp"

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Assets/AssetRegistry.hpp>
#include <Frost/Platform/Filesystem.hpp>
#include <Frost/Renderer/Mesh.hpp>
#include <Frost/Scene/Level.hpp>

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
    // 2: asset references are {"id", "path"} objects. 1: plain path strings (still read).
    constexpr int kFormatVersion = 2;


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

    [[nodiscard]]
    Value MaterialOverrideToJson(const std::size_t subMeshIndex, const Frost::GameObject& object,
                                 const Frost::MaterialOverride& materialOverride,
                                 const std::filesystem::path& levelDirectory, const Frost::AssetRegistry* const registry) {
        Object json;
        json.emplace_back("subMesh", Value(static_cast<double>(subMeshIndex)));

        // The name lets the override find its sub-mesh again if the model is re-exported with its
        // parts in a different order.
        if (object.GetMesh() != nullptr) {
            const std::vector<Frost::SubMesh>& subMeshes = object.GetMesh()->mesh->GetSubMeshes();
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

    [[nodiscard]]
    Value ObjectToJson(const Frost::GameObject& object, const std::filesystem::path& levelDirectory,
                       const Frost::AssetRegistry* const registry) {
        const Frost::Transform& transform = object.GetTransform();

        Object json;
        json.emplace_back("id", Value(static_cast<double>(object.GetId())));
        json.emplace_back("name", Value(object.GetName()));
        json.emplace_back("transform", Value(Object{{"position", ToJson(transform.position)},
                                                    {"rotation", ToJson(transform.rotation)},
                                                    {"scale", ToJson(transform.scale)}}));

        if (object.GetMeshReference().IsSet()) {
            json.emplace_back("mesh", ReferenceToJson(object.GetMeshReference(), levelDirectory, registry));
        }
        if (object.GetLight().has_value()) { json.emplace_back("light", LightToJson(*object.GetLight())); }

        Array materials;
        for (std::size_t i = 0; i < object.GetMaterialSlotCount(); ++i) {
            const std::optional<Frost::MaterialOverride>& materialOverride = object.GetMaterialOverride(i);
            if (materialOverride.has_value()) {
                materials.push_back(MaterialOverrideToJson(i, object, *materialOverride, levelDirectory, registry));
            }
        }
        if (!materials.empty()) { json.emplace_back("materials", Value(std::move(materials))); }

        return Value(std::move(json));
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

    // Nullopt (with a warning) for an unknown light type.
    [[nodiscard]]
    std::optional<Frost::LightComponent> ReadLight(const Value& json, const std::string& objectName,
                                                   std::vector<std::string>& warnings) {
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
        light.range = ReadFloat(json, "range", light.range);
        light.innerConeAngle = ReadFloat(json, "innerConeAngle", light.innerConeAngle);
        light.outerConeAngle = ReadFloat(json, "outerConeAngle", light.outerConeAngle);
        return light;
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

}  // namespace

namespace Frost {

    void LevelSerializer::Save(const Level& level, const std::filesystem::path& path, const AssetRegistry* const registry) {
        const std::filesystem::path target = AssetManager::NormalizePath(path);
        const std::filesystem::path directory = target.parent_path();

        Array objects;
        for (const std::unique_ptr<GameObject>& object : level.GetObjects()) {
            objects.push_back(ObjectToJson(*object, directory, registry));
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

            if (const Value* const transform = json.Find("transform"); transform != nullptr) {
                Transform& target = object.GetTransform();
                target.position = ReadVector<glm::vec3, 3>(transform->Find("position"), target.position);
                target.rotation = ReadVector<glm::vec3, 3>(transform->Find("rotation"), target.rotation);
                target.scale = ReadVector<glm::vec3, 3>(transform->Find("scale"), target.scale);
            }

            if (const Value* const light = json.Find("light"); light != nullptr && light->IsObject()) {
                object.GetLight() = ReadLight(*light, object.GetName(), result.warnings);
            }

            if (const Value* const mesh = json.Find("mesh"); mesh != nullptr) {
                if (const std::optional<AssetReference> reference = ReadReference(*mesh, directory)) {
                    try {
                        object.SetMesh(assets.LoadMesh(*reference));
                    }
                    catch (const ImportError& e) {
                        object.SetUnresolvedMesh(*reference);
                        result.warnings.push_back("'" + object.GetName() + "': " + e.what());
                    }
                }
            }

            const Value* const materials = json.Find("materials");
            if (materials == nullptr || !materials->IsArray()) { continue; }

            for (const Value& materialJson : materials->AsArray()) {
                // By name when the loaded model has a sub-mesh of that name, else by index.
                std::optional<std::size_t> index;
                const Value* const subMeshName = materialJson.Find("subMeshName");
                if (object.GetMesh() != nullptr && subMeshName != nullptr && subMeshName->IsString()) {
                    index = object.GetMesh()->mesh->FindSubMesh(subMeshName->AsString());
                }
                if (!index.has_value()) {
                    const std::optional<double> subMesh = ReadNumber(materialJson.Find("subMesh"));
                    if (subMesh.has_value() && *subMesh >= 0.0 && std::floor(*subMesh) == *subMesh &&
                        *subMesh < 65536.0) {
                        index = static_cast<std::size_t>(*subMesh);
                    }
                }

                const bool outOfRange = object.GetMesh() != nullptr && index.has_value() &&
                                        *index >= object.GetMesh()->mesh->GetSubMeshes().size();
                if (!index.has_value() || outOfRange) {
                    result.warnings.push_back("'" + object.GetName() +
                                              "': dropped a material override for a sub-mesh the model does not have.");
                    continue;
                }

                object.SetMaterialOverride(*index, ReadMaterialOverride(materialJson, directory));
            }
        }

        return result;
    }

}  // namespace Frost
