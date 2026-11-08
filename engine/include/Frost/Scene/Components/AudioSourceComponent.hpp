#pragma once

#include <Frost/Assets/AssetId.hpp>
#include <Frost/Audio/AudioEngine.hpp>
#include <Frost/Units.hpp>

#include <glm/vec3.hpp>

#include <memory>

namespace Frost {

    struct SoundAsset;

    // Plays a sound (a sound asset) from its object: heard from where the object is (it moves with
    // it, also when attached to another object), fading with distance from the listener, or, when
    // not spatial, the same everywhere (music, ambience).
    //
    // Everything here is a setting, saved with the level: the sound, how loud and at what pitch,
    // whether it loops, whether it starts playing when the game starts, and how it is heard in
    // space. Whether it is playing (and where in the sound it is) is run-time state, kept by the
    // AudioSystem that plays it: ask it to play, pause or stop the object's sound. Copying the
    // component (duplicating the object) copies the settings, not the playing.
    //
    // The sound is referenced by asset (id + last known path), kept even if it could not be loaded
    // (a missing file), so saving the level does not lose it.
    struct AudioSourceComponent {
        static constexpr const char* kDisplayName = "Audio Source";

        // The sound, by asset; and the loaded asset, or null (none, or could not be loaded).
        AssetReference soundReference;
        std::shared_ptr<const SoundAsset> sound;

        // Linear gain: 1 plays it as recorded, 0.5 is about 6 dB quieter, 0 is silent.
        float volume = 1.0f;

        // Playback speed, which also shifts the pitch: 2 plays it twice as fast, an octave up.
        float pitch = 1.0f;

        // Start over at the end; otherwise it stops there.
        bool loop = false;

        // Start playing when the game starts (see AudioSystem::Start).
        bool playOnStart = true;

        // Heard from the object's position (panned, fading with distance; see AudioAttenuation),
        // rather than the same everywhere.
        bool spatial = true;

        // Within minDistance it plays at full volume; beyond maxDistance it gets no quieter.
        // World units (centimeters).
        float minDistance = 1.0f * kMeter;
        float maxDistance = 50.0f * kMeter;

        // How fast it fades between the two (1 = the model's natural fade; more fades faster).
        float rolloff = 1.0f;
        AudioAttenuation attenuation = AudioAttenuation::Inverse;

        // The Doppler effect: how much moving towards or away from the listener (it, or the listener)
        // raises or lowers its pitch, as a passing car's or jet's does. 1 as in the real world, 0 not
        // at all, more exaggerates it. Spatial sources only (see AudioSystem).
        float doppler = 1.0f;

        // Occlusion: how much walls (colliders) between it and the listener muffle it, quieter and
        // duller. 1 fully (as AudioSettings says walls do), 0 not at all. Spatial sources only (see
        // AudioSystem).
        float occlusion = 1.0f;

        // Assigns a loaded sound (or none, with nullptr).
        void SetSound(std::shared_ptr<const SoundAsset> asset);

        // Records a sound reference that could not be loaded: nothing plays, but the reference
        // survives a save.
        void SetUnresolvedSound(AssetReference reference);

        // True when a sound is referenced but could not be loaded.
        [[nodiscard]]
        bool IsSoundMissing() const noexcept;

        // How a voice of it plays from `position` (where its object is in the world). Values out of
        // range (they come from UI fields and level files) are brought into range.
        [[nodiscard]]
        AudioVoiceSettings ToVoiceSettings(const glm::vec3& position) const noexcept;
    };

}  // namespace Frost
