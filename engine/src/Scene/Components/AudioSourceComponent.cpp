#include <Frost/Scene/Components/AudioSourceComponent.hpp>

#include <Frost/Assets/AssetManager.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

    [[nodiscard]]
    float FiniteOr(const float value, const float fallback) noexcept {
        return std::isfinite(value) ? value : fallback;
    }

}  // namespace

namespace Frost {

    void AudioSourceComponent::SetSound(std::shared_ptr<const SoundAsset> asset) {
        soundReference = asset != nullptr ? AssetReference{.id = asset->id, .path = asset->path} : AssetReference{};
        sound = std::move(asset);
    }

    void AudioSourceComponent::SetUnresolvedSound(AssetReference reference) {
        soundReference = std::move(reference);
        sound = nullptr;
    }

    bool AudioSourceComponent::IsSoundMissing() const noexcept { return soundReference.IsSet() && sound == nullptr; }

    AudioVoiceSettings AudioSourceComponent::ToVoiceSettings(const glm::vec3& position) const noexcept {
        const AudioSourceComponent defaults;
        const float minimum = std::max(FiniteOr(minDistance, defaults.minDistance), 0.01f * kCentimeter);

        return AudioVoiceSettings{.volume = std::max(FiniteOr(volume, defaults.volume), 0.0f),
                                  .pitch = FiniteOr(pitch, defaults.pitch) > 0.0f ? FiniteOr(pitch, defaults.pitch) : defaults.pitch,
                                  .looping = loop,
                                  .spatial = spatial,
                                  .position = position,
                                  .minDistance = minimum,
                                  .maxDistance = std::max(FiniteOr(maxDistance, defaults.maxDistance), minimum),
                                  .rolloff = std::max(FiniteOr(rolloff, defaults.rolloff), 0.0f),
                                  .attenuation = attenuation};
    }

}  // namespace Frost
