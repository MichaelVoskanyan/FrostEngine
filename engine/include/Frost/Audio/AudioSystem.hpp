#pragma once

#include <Frost/Audio/AudioEngine.hpp>
#include <Frost/Physics/Collision.hpp>
#include <Frost/Scene/Registry.hpp>
#include <Frost/Units.hpp>

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Frost {

    // Where an object's sound is.
    enum class AudioPlayback : std::uint8_t {
        Stopped,  // Not playing, at the start (never played, stopped, or played to its end).
        Playing,
        Paused,   // Not playing, somewhere in the sound; playing resumes from there.
    };

    // How sound behaves in the world, for every audio source an AudioSystem plays.
    struct AudioSettings {
        // The Doppler effect: how fast sound travels (centimeters per second; 343 m/s in air).
        float speedOfSound = 343.0f * kMeter;

        // Occlusion: what one wall (collider) between a sound and the listener lets through: this
        // much of its volume (linear; 0.3 is about -10 dB)...
        float wallVolume = 0.3f;
        // ...and only what is below this frequency (Hz); each further wall an octave lower.
        float wallCutoff = 1200.0f;
        // Sound bends around edges: besides straight at it, it is listened for this far around the
        // source (centimeters), so a sound just behind a corner is partly heard.
        float occlusionSpread = 30.0f * kCentimeter;
        // How often occlusion is worked out (seconds; it traces rays), and how quickly it follows
        // (the time it takes to get most of the way to a change, seconds), so walking past a doorway
        // does not make sounds jump.
        float occlusionInterval = 1.0f / 20.0f;
        float occlusionResponse = 0.1f;
    };

    // How a playing (or paused) sound is heard right now, for tools and tests.
    struct AudioVoiceInfo {
        float dopplerPitch = 1.0f;      // The Doppler effect's pitch factor (on top of the source's pitch).
        float muffleGain = 1.0f;        // Occlusion: the gain walls leave it...
        float lowPassCutoff = kAudioNoLowPass;  // ...and the frequency (Hz) they muffle it above.
        int blockedRays = 0;            // Of the rays listened along (0 if not spatial), how many walls cut.
        int rays = 0;
        int walls = 0;                  // The most walls any of them crossed.
    };

    // Plays a level's audio sources (AudioSourceComponent): one voice per object whose sound is
    // playing or paused, made on the AudioEngine. The components say how each sound plays; this
    // keeps track of which are playing, and where in their sounds they are. Run by the game (and by
    // the editor, to preview sounds).
    //
    // Every frame (Update), each voice follows its object (where it is in the world, attached to
    // another object or not) and its component's settings (volume, pitch, looping, ... changed
    // while it plays take effect right away); a voice that has played to its end stops; and a voice
    // whose object is gone, whose object no longer has an audio source, or whose sound was changed
    // stops too. So deleting an object or its component, or swapping its sound, never leaves a sound
    // playing.
    //
    // Spatial sounds are also heard as the world makes them sound:
    //   - The Doppler effect: a sound coming closer is higher, going away lower, as
    //     (c + v_listener) / (c + v_source), with c the speed of sound and each velocity along the
    //     line from the listener to the source (scaled by the source's doppler setting). Velocities
    //     are worked out from how far sources and the listener move from frame to frame (so whatever
    //     moves them counts: physics, animation, attachment, gameplay code), smoothed over a few
    //     frames; a jump faster than twice the speed of sound is a teleport, not a motion.
    //   - Occlusion: rays are traced from the listener to the source and to points around it (see
    //     AudioSettings::occlusionSpread) through the level's colliders (see RaycastAll). Each wall a
    //     ray crosses lets through AudioSettings::wallVolume of it, and muffles it above
    //     AudioSettings::wallCutoff (an octave lower for each further wall); the rays are averaged,
    //     so a sound half hidden is half muffled. The source's own object and the listener's are not
    //     walls, nor are simulated rigid bodies (characters, props: things that move about, not
    //     walls). Worked out every AudioSettings::occlusionInterval, and eased into.
    //
    // One registry at a time (objects are known by id): StopAll before playing another one. Must
    // not outlive the AudioEngine.
    class AudioSystem final {
    public:
        explicit AudioSystem(AudioEngine& engine, const AudioSettings& settings = {});
        ~AudioSystem();

        AudioSystem(const AudioSystem&) = delete;
        AudioSystem& operator=(const AudioSystem&) = delete;

        AudioSystem(AudioSystem&&) = delete;
        AudioSystem& operator=(AudioSystem&&) = delete;

        [[nodiscard]]
        const AudioSettings& GetSettings() const noexcept;

        void SetSettings(const AudioSettings& settings) noexcept;

        // Plays every audio source set to play on start (that has a sound). Call once, when the level
        // starts. Returns a message per sound that could not be played.
        std::vector<std::string> Start(Registry& registry);

        // Plays the object's sound: resumes it if paused, starts it from the beginning if stopped,
        // and leaves it be if already playing. False (with the reason in `error`, if given) if the
        // object has no audio source, its sound is not loaded, or it cannot be played.
        bool Play(Registry& registry, EntityId object, std::string* error = nullptr);

        // Stops playing the object's sound, keeping its place (Play resumes it).
        void Pause(EntityId object);

        // Stops playing the object's sound and goes back to its start.
        void Stop(EntityId object);

        void StopAll();

        [[nodiscard]]
        AudioPlayback GetPlayback(EntityId object) const;

        // How far into its sound the object is, in seconds (0 when stopped).
        [[nodiscard]]
        double GetTime(EntityId object) const;

        // How the object's sound is heard now (Doppler, occlusion); nullopt if it is stopped.
        [[nodiscard]]
        std::optional<AudioVoiceInfo> GetVoiceInfo(EntityId object) const;

        // Moves the object's place in its sound to `seconds` (clamped to the sound): a playing sound
        // carries on from there, a paused one will resume from there, and a stopped one becomes
        // paused there. False (as Play) if it has no sound to play.
        bool Seek(Registry& registry, EntityId object, double seconds, std::string* error = nullptr);

        // Hears everything from `listener` (an audio listener, or the camera) and brings the voices
        // up to date (see above); `deltaTime` is the time since the last update (seconds; for the
        // Doppler effect and easing occlusion), `listenerObject` the object the listener is on, if
        // any (whose collider is no wall). Call once per frame.
        void Update(Registry& registry, const AudioListener& listener, float deltaTime, EntityId listenerObject = 0);

        // Voices playing or paused.
        [[nodiscard]]
        std::size_t GetVoiceCount() const noexcept;

    private:
        // Where a tracked point (a source, or the listener) is, and how fast it moves.
        struct Motion {
            std::optional<glm::vec3> position;
            glm::vec3 velocity{0.0f};
        };

        struct Voice {
            std::unique_ptr<AudioVoice> voice;
            Motion motion;
            float dopplerPitch = 1.0f;
            // Occlusion: where it is heading, and where it is (the cutoff in octaves, log2 Hz, so it
            // eases evenly across the range).
            float targetGain = 1.0f;
            float targetOctave = 0.0f;
            float gain = 1.0f;
            float octave = 0.0f;
            float sinceOcclusion = 0.0f;
            bool occlusionKnown = false;
            int blockedRays = 0;
            int rays = 0;
            int walls = 0;
        };

        // The object's voice, made (stopped, set up as its component says) if it has none, or if its
        // sound is not the component's. Null (with the reason in `error`) if it cannot be.
        Voice* Prepare(Registry& registry, EntityId object, std::string* error);

        // Brings one voice up to date for this frame (position, settings, Doppler, occlusion).
        void Refresh(Registry& registry, EntityId object, Voice& voice, float deltaTime);

        // Works out what walls between the listener and the source at `position` do to it now.
        void TraceOcclusion(const Registry& registry, EntityId object, const glm::vec3& position, Voice& voice);

        // A collider as occlusion traces it: its shape in the world, a sphere around it (to pass
        // over it quickly), and whether it is a wall at all (not a simulated rigid body).
        struct OcclusionCollider {
            EntityId entity = 0;
            CollisionShape shape;
            glm::vec3 center{0.0f};
            float radius = 0.0f;
            bool wall = true;
        };

        // The level's colliders, gathered once per update (the first time a voice traces), as the
        // raycasts see them (see RaycastAll).
        const std::vector<OcclusionCollider>& GetOcclusionColliders(const Registry& registry);

        void Track(Motion& motion, const glm::vec3& position, float deltaTime) const;

        AudioEngine& m_Engine;
        AudioSettings m_Settings;
        std::unordered_map<EntityId, Voice> m_Voices;

        std::vector<OcclusionCollider> m_OcclusionColliders;
        bool m_OcclusionCollidersGathered = false;

        // The listener as last updated.
        AudioListener m_Listener;
        EntityId m_ListenerObject = 0;
        Motion m_ListenerMotion;
    };

}  // namespace Frost
