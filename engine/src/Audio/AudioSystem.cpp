#include <Frost/Audio/AudioSystem.hpp>

#include "../Physics/ColliderQuery.hpp"

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Physics/Raycast.hpp>
#include <Frost/Scene/Components/AttachmentComponent.hpp>
#include <Frost/Scene/Components/AudioSourceComponent.hpp>
#include <Frost/Scene/Components/RigidBodyComponent.hpp>
#include <Frost/Scene/Hierarchy.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {

    // How quickly estimated velocities follow the motion (seconds): long enough to smooth out uneven
    // frame times, short enough for a passing car's pitch to drop as it passes.
    constexpr float kVelocitySmoothing = 0.05f;

    // Moving faster than this many times the speed of sound from one frame to the next is a
    // teleport (respawning, a level change, moving an object in the editor), not a motion.
    constexpr float kMaxSpeedOfSounds = 2.0f;

    // The Doppler pitch factor is kept within these (and the velocities along the line of hearing
    // above -0.9 c, where the formula blows up).
    constexpr float kMinDopplerPitch = 0.1f;
    constexpr float kMaxDopplerPitch = 10.0f;
    constexpr float kMaxApproachFraction = 0.9f;

    // Closer than this (centimeters), the source is where the listener is: no direction to hear it
    // from, no Doppler shift, no walls between.
    constexpr float kMinHearingDistance = 1.0f;

    [[nodiscard]]
    float OpenOctave() noexcept {
        static const float octave = std::log2(Frost::kAudioNoLowPass);
        return octave;
    }

    [[nodiscard]]
    bool IsFinite(const glm::vec3& v) noexcept {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    void SetError(std::string* const error, std::string message) {
        if (error != nullptr) { *error = std::move(message); }
    }

    // The fraction of the way to its target an eased value goes in `deltaTime` seconds.
    [[nodiscard]]
    float EaseFraction(const float deltaTime, const float response) noexcept {
        if (!(response > 0.0f)) { return 1.0f; }
        return 1.0f - std::exp(-std::max(deltaTime, 0.0f) / response);
    }

    // The object at the top of `object`'s chain of attachments (itself if not attached).
    [[nodiscard]]
    Frost::EntityId RootOf(const Frost::Registry& registry, Frost::EntityId object) {
        for (std::size_t depth = 0; depth < Frost::kMaxAttachmentDepth && Frost::IsAttached(registry, object); ++depth) {
            object = registry.Get<Frost::AttachmentComponent>(object).parent;
        }
        return object;
    }

    // Where in its occlusion interval a voice traces (0 to 1), so voices started together (a level's
    // sounds, at its start) do not all trace in the same frame: spread evenly by object id.
    [[nodiscard]]
    float OcclusionPhase(const Frost::EntityId object) noexcept {
        const double golden = static_cast<double>(object % 1000003) * 0.6180339887498949;
        return static_cast<float>(golden - std::floor(golden));
    }

    // Two unit vectors perpendicular to `direction` (unit length) and to each other.
    void PerpendicularBasis(const glm::vec3& direction, glm::vec3& u, glm::vec3& v) noexcept {
        const glm::vec3 helper = std::abs(direction.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        u = glm::normalize(glm::cross(direction, helper));
        v = glm::cross(u, direction);
    }

}  // namespace

namespace Frost {

    AudioSystem::AudioSystem(AudioEngine& engine, const AudioSettings& settings) : m_Engine(engine), m_Settings(settings) {}

    // The voices go before the engine they were made on (which the owner keeps alive).
    AudioSystem::~AudioSystem() = default;

    const AudioSettings& AudioSystem::GetSettings() const noexcept { return m_Settings; }

    void AudioSystem::SetSettings(const AudioSettings& settings) noexcept { m_Settings = settings; }

    std::vector<std::string> AudioSystem::Start(Registry& registry) {
        std::vector<EntityId> toPlay;
        registry.Each<AudioSourceComponent>([&](const EntityId object, const AudioSourceComponent& source) {
            if (source.playOnStart && source.sound != nullptr) { toPlay.push_back(object); }
        });

        std::vector<std::string> problems;
        for (const EntityId object : toPlay) {
            std::string error;
            if (!Play(registry, object, &error)) { problems.push_back(std::move(error)); }
        }
        return problems;
    }

    AudioSystem::Voice* AudioSystem::Prepare(Registry& registry, const EntityId object, std::string* const error) {
        const AudioSourceComponent* const source = registry.TryGet<AudioSourceComponent>(object);
        if (source == nullptr) {
            SetError(error, "The object has no audio source.");
            return nullptr;
        }
        if (source->sound == nullptr) {
            SetError(error, source->IsSoundMissing() ? "Its sound could not be loaded; it may have been deleted."
                                                     : "It has no sound to play.");
            return nullptr;
        }

        if (const auto found = m_Voices.find(object); found != m_Voices.end()) {
            if (found->second.voice->GetSound() == source->sound) { return &found->second; }
            m_Voices.erase(found);  // Another sound now: that one stops.
        }

        Voice voice;
        try {
            voice.voice = m_Engine.CreateVoice(source->sound);
        }
        catch (const AudioError& e) {
            SetError(error, e.what());
            return nullptr;
        }
        voice.octave = voice.targetOctave = OpenOctave();

        // Set up before it is heard, so it starts where (and as) it should: already muffled if it
        // starts behind a wall, heard from where the listener was last.
        Voice& added = m_Voices.insert_or_assign(object, std::move(voice)).first->second;
        Refresh(registry, object, added, 0.0f);
        return &added;
    }

    bool AudioSystem::Play(Registry& registry, const EntityId object, std::string* const error) {
        Voice* const voice = Prepare(registry, object, error);
        if (voice == nullptr) { return false; }
        voice->voice->Play();
        return true;
    }

    void AudioSystem::Pause(const EntityId object) {
        if (const auto found = m_Voices.find(object); found != m_Voices.end()) { found->second.voice->Pause(); }
    }

    void AudioSystem::Stop(const EntityId object) { m_Voices.erase(object); }

    void AudioSystem::StopAll() { m_Voices.clear(); }

    AudioPlayback AudioSystem::GetPlayback(const EntityId object) const {
        const auto found = m_Voices.find(object);
        if (found == m_Voices.end()) { return AudioPlayback::Stopped; }

        // At its end it has been heard to the end: stopped, even if the mixer has yet to say so.
        const AudioVoice& voice = *found->second.voice;
        if (voice.IsAtEnd()) { return AudioPlayback::Stopped; }
        return voice.IsPlaying() ? AudioPlayback::Playing : AudioPlayback::Paused;
    }

    double AudioSystem::GetTime(const EntityId object) const {
        const auto found = m_Voices.find(object);
        if (found == m_Voices.end() || found->second.voice->IsAtEnd()) { return 0.0; }
        return found->second.voice->GetTime();
    }

    std::optional<AudioVoiceInfo> AudioSystem::GetVoiceInfo(const EntityId object) const {
        const auto found = m_Voices.find(object);
        if (found == m_Voices.end() || found->second.voice->IsAtEnd()) { return std::nullopt; }
        const Voice& voice = found->second;
        return AudioVoiceInfo{.dopplerPitch = voice.dopplerPitch,
                              .muffleGain = voice.gain,
                              .lowPassCutoff = std::exp2(voice.octave),
                              .blockedRays = voice.blockedRays,
                              .rays = voice.rays,
                              .walls = voice.walls};
    }

    bool AudioSystem::Seek(Registry& registry, const EntityId object, const double seconds, std::string* const error) {
        Voice* const voice = Prepare(registry, object, error);
        if (voice == nullptr) { return false; }
        voice->voice->Seek(seconds);
        return true;
    }

    void AudioSystem::Update(Registry& registry, const AudioListener& listener, const float deltaTime, const EntityId listenerObject) {
        m_OcclusionCollidersGathered = false;  // Gathered anew (if needed) for this update.
        m_Listener = listener;
        m_ListenerObject = listenerObject;
        Track(m_ListenerMotion, listener.position, deltaTime);
        m_Engine.SetListener(listener);

        for (auto it = m_Voices.begin(); it != m_Voices.end();) {
            const AudioSourceComponent* const source = registry.TryGet<AudioSourceComponent>(it->first);
            Voice& voice = it->second;

            const bool orphaned = source == nullptr || source->sound != voice.voice->GetSound();
            const bool finished = voice.voice->IsAtEnd();  // Heard to the end (not looping).
            if (orphaned || finished) {
                it = m_Voices.erase(it);
                continue;
            }

            Refresh(registry, it->first, voice, deltaTime);
            ++it;
        }
        m_OcclusionCollidersGathered = false;  // (Play and Seek between updates gather their own.)
    }

    std::size_t AudioSystem::GetVoiceCount() const noexcept { return m_Voices.size(); }

    void AudioSystem::Refresh(Registry& registry, const EntityId object, Voice& voice, const float deltaTime) {
        const AudioSourceComponent& source = registry.Get<AudioSourceComponent>(object);
        const glm::vec3 position = ComputeWorldPlacement(registry, object).position;
        AudioVoiceSettings settings = source.ToVoiceSettings(position);

        Track(voice.motion, position, deltaTime);

        const glm::vec3 toSource = position - m_Listener.position;
        const float distance = glm::length(toSource);
        const bool heardFromSomewhere = settings.spatial && std::isfinite(distance) && distance >= kMinHearingDistance;

        // The Doppler effect, along the line from the listener to the source.
        voice.dopplerPitch = 1.0f;
        const float doppler = std::isfinite(source.doppler) ? std::max(source.doppler, 0.0f) : 1.0f;
        const float speedOfSound = m_Settings.speedOfSound;
        if (heardFromSomewhere && doppler > 0.0f && std::isfinite(speedOfSound) && speedOfSound > 0.0f) {
            const glm::vec3 direction = toSource / distance;
            // Positive: the listener moving towards the source; the source moving away.
            const float listenerSpeed = std::max(glm::dot(m_ListenerMotion.velocity, direction) * doppler, -kMaxApproachFraction * speedOfSound);
            const float sourceSpeed = std::max(glm::dot(voice.motion.velocity, direction) * doppler, -kMaxApproachFraction * speedOfSound);
            voice.dopplerPitch = std::clamp((speedOfSound + listenerSpeed) / (speedOfSound + sourceSpeed), kMinDopplerPitch, kMaxDopplerPitch);
        }
        settings.pitch *= voice.dopplerPitch;

        // Occlusion: traced every so often, eased into every frame (at once the first time, so a
        // sound starting behind a wall starts muffled).
        const float occlusion = std::isfinite(source.occlusion) ? std::clamp(source.occlusion, 0.0f, 1.0f) : 1.0f;
        if (heardFromSomewhere && occlusion > 0.0f) {
            voice.sinceOcclusion += std::max(deltaTime, 0.0f);
            if (!voice.occlusionKnown || voice.sinceOcclusion >= m_Settings.occlusionInterval) {
                TraceOcclusion(registry, object, position, voice);
                voice.targetGain = 1.0f + (voice.targetGain - 1.0f) * occlusion;
                voice.targetOctave = OpenOctave() + (voice.targetOctave - OpenOctave()) * occlusion;
                // The first time, start at the voice's own phase in the interval (the next trace
                // comes sooner, never later, than the interval).
                voice.sinceOcclusion = voice.occlusionKnown ? 0.0f : OcclusionPhase(object) * m_Settings.occlusionInterval;
            }
        }
        else {
            voice.targetGain = 1.0f;
            voice.targetOctave = OpenOctave();
            voice.blockedRays = voice.rays = voice.walls = 0;
        }
        const float ease = voice.occlusionKnown ? EaseFraction(deltaTime, m_Settings.occlusionResponse) : 1.0f;
        voice.gain += (voice.targetGain - voice.gain) * ease;
        voice.octave += (voice.targetOctave - voice.octave) * ease;
        voice.occlusionKnown = true;

        settings.muffleGain = voice.gain;
        settings.lowPassCutoff = std::exp2(voice.octave);
        voice.voice->Apply(settings);
    }

    const std::vector<AudioSystem::OcclusionCollider>& AudioSystem::GetOcclusionColliders(const Registry& registry) {
        if (m_OcclusionCollidersGathered) { return m_OcclusionColliders; }
        m_OcclusionColliders.clear();
        Detail::ForEachCollider(registry, RaycastFilter{}, [&](const EntityId entity, const CollisionShape& shape) {
            const Bounds bounds = GetBounds(shape);
            const RigidBodyComponent* const body = registry.TryGet<RigidBodyComponent>(entity);
            m_OcclusionColliders.push_back(OcclusionCollider{.entity = entity,
                                                             .shape = shape,
                                                             .center = bounds.GetCenter(),
                                                             .radius = glm::length(bounds.GetSize()) * 0.5f,
                                                             .wall = body == nullptr || !body->simulated});
        });
        m_OcclusionCollidersGathered = true;
        return m_OcclusionColliders;
    }

    void AudioSystem::TraceOcclusion(const Registry& registry, const EntityId object, const glm::vec3& position, Voice& voice) {
        const glm::vec3 origin = m_Listener.position;
        const glm::vec3 toSource = position - origin;
        const float distance = glm::length(toSource);

        // Straight at the source, and at four points around it, across the line of hearing.
        glm::vec3 u;
        glm::vec3 v;
        PerpendicularBasis(toSource / distance, u, v);
        const float spread = std::isfinite(m_Settings.occlusionSpread) ? std::max(m_Settings.occlusionSpread, 0.0f) : 0.0f;
        const std::array<glm::vec3, 5> targets = {position, position + u * spread, position - u * spread, position + v * spread,
                                                  position - v * spread};

        // Neither the source's own object nor the listener's (with what they are attached to: an
        // attached object has no collider of its own) is a wall.
        const EntityId sourceRoot = RootOf(registry, object);
        const EntityId listenerRoot = m_ListenerObject != 0 ? RootOf(registry, m_ListenerObject) : 0;
        const std::vector<OcclusionCollider>& colliders = GetOcclusionColliders(registry);

        const float wallVolume = std::isfinite(m_Settings.wallVolume) ? std::clamp(m_Settings.wallVolume, 0.0f, 1.0f) : 1.0f;
        const float wallOctave = std::log2(std::clamp(std::isfinite(m_Settings.wallCutoff) ? m_Settings.wallCutoff : kAudioNoLowPass, 20.0f,
                                                      kAudioNoLowPass));

        float gainSum = 0.0f;
        float octaveSum = 0.0f;
        voice.blockedRays = 0;
        voice.walls = 0;
        for (const glm::vec3& target : targets) {
            const glm::vec3 path = target - origin;
            const float length = glm::length(path);
            int walls = 0;
            if (IsFinite(path) && length > 0.0f) {
                // The colliders the ray crosses between the listener and the target (as RaycastAll
                // finds them), less those that are no walls: things that move about (characters,
                // props), and the source's and listener's own.
                const Ray ray{.origin = origin, .direction = path / length};
                for (const OcclusionCollider& collider : colliders) {
                    if (!collider.wall || collider.entity == sourceRoot || collider.entity == listenerRoot) { continue; }
                    // Passed over when the ray's segment misses the sphere around it.
                    const float along = std::clamp(glm::dot(collider.center - origin, ray.direction), 0.0f, length);
                    const glm::vec3 nearest = origin + ray.direction * along - collider.center;
                    if (glm::dot(nearest, nearest) > collider.radius * collider.radius) { continue; }
                    if (IntersectRay(ray, collider.shape, length).has_value()) { ++walls; }
                }
            }
            gainSum += std::pow(wallVolume, static_cast<float>(walls));
            octaveSum += walls == 0 ? OpenOctave() : std::min(wallOctave - static_cast<float>(walls - 1), OpenOctave());
            voice.blockedRays += walls > 0 ? 1 : 0;
            voice.walls = std::max(voice.walls, walls);
        }
        voice.rays = static_cast<int>(targets.size());
        voice.targetGain = gainSum / static_cast<float>(targets.size());
        voice.targetOctave = octaveSum / static_cast<float>(targets.size());
    }

    void AudioSystem::Track(Motion& motion, const glm::vec3& position, const float deltaTime) const {
        if (!motion.position.has_value() || !(deltaTime > 0.0f)) {
            if (!motion.position.has_value()) { motion.velocity = glm::vec3(0.0f); }
            motion.position = position;
            return;
        }

        const glm::vec3 measured = (position - *motion.position) / deltaTime;
        motion.position = position;
        if (!IsFinite(measured) || glm::length(measured) > kMaxSpeedOfSounds * m_Settings.speedOfSound) {
            motion.velocity = glm::vec3(0.0f);  // A teleport.
            return;
        }
        motion.velocity += (measured - motion.velocity) * EaseFraction(deltaTime, kVelocitySmoothing);
    }

}  // namespace Frost
