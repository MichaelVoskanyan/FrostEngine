#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace Frost {

    struct SoundAsset;

    // Thrown for audio that cannot be played (a sound that cannot be decoded, audio being
    // unavailable altogether).
    class AudioError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    // How a spatial sound gets quieter with distance from the listener, between its minimum
    // distance (full volume) and its maximum distance (no quieter beyond it):
    //   Inverse:     min / (min + rolloff * (d - min)), as sound fades in the real world.
    //   Linear:      1 - rolloff * (d - min) / (max - min), silent at the maximum distance (rolloff 1).
    //   Exponential: (d / min) ^ -rolloff.
    //   None:        no fading: as loud at any distance (still panned by where it is).
    // Stored in level files by name (see GetAudioAttenuationName); append new values, never renumber.
    enum class AudioAttenuation : std::uint8_t { None, Inverse, Linear, Exponential };

    // "none", "inverse", "linear", "exponential".
    [[nodiscard]]
    const char* GetAudioAttenuationName(AudioAttenuation attenuation) noexcept;

    // Where sound is heard from (the camera, normally): a position and which way it faces. World
    // space, in centimeters (see Frost/Units.hpp); `forward` and `up` need not be normalized.
    struct AudioListener {
        glm::vec3 position{0.0f};
        glm::vec3 forward{0.0f, 0.0f, -1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
    };

    // A low-pass cutoff (Hz) at or above which a voice is not filtered at all (above hearing).
    inline constexpr float kAudioNoLowPass = 20000.0f;

    // How a voice plays (see AudioSourceComponent, which these come from). Values outside their
    // range are brought into it when applied.
    struct AudioVoiceSettings {
        float volume = 1.0f;  // Linear gain, 0 and up.
        float pitch = 1.0f;   // Playback speed (and pitch), above 0.
        bool looping = false;

        // A spatial voice is heard from `position`: panned by where it is relative to the
        // listener, and fading with distance (see AudioAttenuation). Otherwise it plays as
        // recorded, the same wherever the listener is (music, UI).
        bool spatial = true;
        glm::vec3 position{0.0f};  // World space, centimeters.
        float minDistance = 100.0f;
        float maxDistance = 5000.0f;
        float rolloff = 1.0f;
        AudioAttenuation attenuation = AudioAttenuation::Inverse;

        // Muffling on top of everything else (what walls do to it; see AudioSystem): a gain (linear,
        // 0 to 1) and a low-pass filter's cutoff, in Hz (at or above kAudioNoLowPass: no filtering).
        // Changes glide over the next mixing period, so they can change every frame without
        // clicking. Apply to spatial and non-spatial voices alike.
        float muffleGain = 1.0f;
        float lowPassCutoff = kAudioNoLowPass;
    };

    class AudioEngine;

    // One playing instance of a sound: its own position in it, its own settings. Created stopped
    // (at the start) by AudioEngine::CreateVoice; playing and pausing leave its position where it
    // is. Keeps its sound asset alive (it decodes the asset's audio as it plays). Must not outlive
    // the AudioEngine that made it.
    class AudioVoice final {
    public:
        ~AudioVoice();

        AudioVoice(const AudioVoice&) = delete;
        AudioVoice& operator=(const AudioVoice&) = delete;

        AudioVoice(AudioVoice&&) = delete;
        AudioVoice& operator=(AudioVoice&&) = delete;

        // Starts (or resumes) playing from its current position.
        void Play();

        // Stops playing, keeping its position (Play resumes from there).
        void Pause();

        [[nodiscard]]
        bool IsPlaying() const;

        // Whether it has played to the end: everything has been heard (a voice that is not looping
        // stops there). Known a mixing period or so after its last sample is mixed (IsPlaying may
        // still say true meanwhile).
        [[nodiscard]]
        bool IsAtEnd() const;

        // Its position in the sound (where it is being decoded: a mixing period or so ahead of what is
        // heard), and the sound's length, in seconds.
        [[nodiscard]]
        double GetTime() const;

        [[nodiscard]]
        double GetLength() const;

        // Moves its position (clamped to the sound; playing or not).
        void Seek(double seconds);

        void Apply(const AudioVoiceSettings& settings);

        [[nodiscard]]
        const std::shared_ptr<const SoundAsset>& GetSound() const noexcept;

    private:
        friend class AudioEngine;
        struct Impl;

        explicit AudioVoice(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> m_Impl;
    };

    // Where audio goes.
    enum class AudioOutput : std::uint8_t {
        // The system's default playback device; if there is none (or it cannot be opened), a
        // silent one, so everything still runs (and plays, inaudibly).
        Default,
        // A silent device, which plays in real time without making a sound (e.g. on a headless
        // machine, or to run the game muted).
        Silent,
        // No device: nothing plays by itself; the mix is pulled with AudioEngine::Render, as fast
        // as wanted (for tools and tests that need the exact output).
        Manual,
    };

    struct AudioEngineSettings {
        AudioOutput output = AudioOutput::Default;
        // For Manual output (devices choose their own).
        std::uint32_t sampleRate = 48000;
        std::uint32_t channels = 2;
    };

    // The audio device and mixer (miniaudio), owned by Engine. Plays voices (AudioVoice), each
    // decoded from a sound asset (see SoundAsset) as it plays, mixed and spatialized for one
    // listener. Systems (AudioSystem) decide which voices exist and how they play; this only plays
    // them. Calls are made from the main thread; mixing runs on the device's own thread.
    //
    // World positions are in centimeters. The engine itself shifts no pitch for motion: the Doppler
    // effect is computed by AudioSystem, into the voices' pitch.
    class AudioEngine final {
    public:
        explicit AudioEngine(const AudioEngineSettings& settings = {});
        ~AudioEngine();

        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        AudioEngine(AudioEngine&&) = delete;
        AudioEngine& operator=(AudioEngine&&) = delete;

        // Whether sound is actually heard: false for a silent device (asked for, or the fallback
        // when the default device could not be opened) and for Manual output.
        [[nodiscard]]
        bool IsAudible() const noexcept;

        // The playback device's name, or what is used instead ("(silent)", "(manual)").
        [[nodiscard]]
        const std::string& GetDeviceName() const noexcept;

        // The mix's sample rate and channel count.
        [[nodiscard]]
        std::uint32_t GetSampleRate() const noexcept;

        [[nodiscard]]
        std::uint32_t GetChannels() const noexcept;

        void SetListener(const AudioListener& listener);

        // Overall volume (linear gain, 0 and up), on top of every voice's.
        void SetMasterVolume(float volume);

        [[nodiscard]]
        float GetMasterVolume() const noexcept;

        // A new voice of `sound`, stopped at its start. Throws AudioError if the sound's audio cannot
        // be decoded, or std::invalid_argument for a null sound.
        [[nodiscard]]
        std::unique_ptr<AudioVoice> CreateVoice(std::shared_ptr<const SoundAsset> sound);

        // Manual output only: mixes the next `frames.size() / GetChannels()` frames (interleaved
        // 32-bit float samples). Throws AudioError with any other output.
        void Render(std::span<float> frames);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

}  // namespace Frost
