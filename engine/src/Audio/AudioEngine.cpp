// First, before anything can include <windows.h>: miniaudio's implementation includes it on Windows,
// and its min/max macros would break std::min/std::max in this file.
#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
#endif

#include <Frost/Audio/AudioEngine.hpp>

#include <Frost/Assets/AssetManager.hpp>
#include <Frost/Assets/Sound.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

// miniaudio is private to this file: no engine header includes it. Built here (the implementation
// is in the header) unless the build links a miniaudio library (see cmake/Dependencies.cmake).
#if defined(FROST_MINIAUDIO_IMPLEMENTATION)
#define MINIAUDIO_IMPLEMENTATION
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wpedantic"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

// Installed as <miniaudio.h> or <miniaudio/miniaudio.h>, depending on the package.
#if __has_include(<miniaudio.h>)
#include <miniaudio.h>
#else
#include <miniaudio/miniaudio.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace {

    // Sane ranges for what comes from level files and UI fields.
    constexpr float kMaxVolume = 16.0f;
    constexpr float kMinPitch = 0.01f;
    constexpr float kMaxPitch = 16.0f;
    constexpr float kMinDistance = 0.01f;  // Centimeters.

    [[nodiscard]]
    float Finite(const float value, const float fallback) noexcept {
        return std::isfinite(value) ? value : fallback;
    }

    // (None is not miniaudio's "none", which turns spatialization off altogether: the sound would
    // lose its panning, and be louder than the same sound spatialized up close. It is inverse
    // fading with no rolloff: panned, never quieter; see AudioVoice::Apply.)
    [[nodiscard]]
    ma_attenuation_model ToMiniaudio(const Frost::AudioAttenuation attenuation) noexcept {
        switch (attenuation) {
            case Frost::AudioAttenuation::None: return ma_attenuation_model_inverse;
            case Frost::AudioAttenuation::Inverse: return ma_attenuation_model_inverse;
            case Frost::AudioAttenuation::Linear: return ma_attenuation_model_linear;
            case Frost::AudioAttenuation::Exponential: return ma_attenuation_model_exponential;
        }
        return ma_attenuation_model_inverse;
    }

    // --- Muffling (gain and low-pass) -------------------------------------------------------------
    //
    // A node between each voice and the mix: a gain (the voice's volume times its muffling), and a
    // low-pass filter made of two one-pole stages (12 dB per octave). At gain 1 with no low-pass it
    // passes audio through unchanged.
    //
    // Threads: the main thread sets targets (AudioVoice::Apply), the mixing thread runs the filter
    // (MuffleNodeProcess), gliding the gain and the filter coefficient to the targets across each
    // block so that changing them often never clicks. They share only atomics: the targets, and a
    // flag asking the next block to take them at once (so a voice that starts behind a wall starts
    // muffled). Everything else in MuffleState belongs to the mixing thread from the moment the
    // node is created until ma_node_uninit has returned (which waits until the mixing thread is done
    // with the node), and is set up before and torn down after that.
    struct MuffleState {
        std::atomic<float> targetGain{1.0f};
        std::atomic<float> targetCoefficient{1.0f};
        std::atomic<bool> jumpToTargets{true};

        // Mixing thread only (see above).
        float gain = 1.0f;
        float coefficient = 1.0f;
        std::vector<float> filter;  // Two stages per channel: sized before the node exists, never resized.
        ma_uint32 channels = 0;
    };
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
                  "The mixing thread must never wait on a lock.");

    // The node as miniaudio sees it. miniaudio hands its callback the address of the node's
    // ma_node_base; the callback turns that back into the MuffleNode, which is only defined if the
    // two are pointer-interconvertible: a standard-layout struct and its first member are. So this
    // holds nothing but C types (the node base, a pointer), standard-layout on every compiler; the
    // C++ state (atomics, a vector, whose layout is the standard library's business) is pointed to.
    struct MuffleNode {
        ma_node_base base;
        MuffleState* state;
    };
    static_assert(std::is_standard_layout_v<MuffleNode>, "MuffleNode must be standard-layout to be reached from its ma_node_base.");
    static_assert(offsetof(MuffleNode, base) == 0, "MuffleNode's ma_node_base must come first.");

    // Two identical one-pole stages are 3 dB down at about 0.6436 of their own cutoff: each stage's
    // is set that much higher, so the pair's is where asked.
    constexpr float kTwoStageCutoffRatio = 0.6436f;

    // The one-pole coefficient for a cutoff of `cutoff` Hz at `sampleRate` (1: no filtering).
    [[nodiscard]]
    float LowPassCoefficient(const float cutoff, const std::uint32_t sampleRate) noexcept {
        if (!(cutoff < Frost::kAudioNoLowPass) || sampleRate == 0) { return 1.0f; }
        const float stageCutoff = std::max(cutoff, 10.0f) / kTwoStageCutoffRatio;
        if (stageCutoff >= 0.45f * static_cast<float>(sampleRate)) { return 1.0f; }
        return 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * stageCutoff / static_cast<float>(sampleRate));
    }

}  // namespace

// The muffle node's callback, on the mixing thread. C language linkage: miniaudio calls it through a
// pointer to a C function (a function's language linkage is part of its type), and static: it is
// this file's own.
extern "C" {

    static void FrostMuffleNodeProcess(ma_node* const node, const float** const framesIn, ma_uint32* const frameCountIn,
                                       float** const framesOut, ma_uint32* const frameCountOut) {
        // `node` is the address of a MuffleNode's ma_node_base (see MuffleNode): the MuffleNode.
        MuffleState& self = *static_cast<MuffleNode*>(node)->state;

        // miniaudio gives a node like this one as many input frames as it asks output of (filling
        // what its input lacks with silence); kept within both all the same.
        ma_uint32 frames = *frameCountOut;
        if (frameCountIn != nullptr) { frames = std::min(frames, *frameCountIn); }
        *frameCountOut = frames;
        if (frames == 0) { return; }

        const float* const in = framesIn != nullptr ? framesIn[0] : nullptr;
        float* const out = framesOut[0];
        const ma_uint32 channels = self.channels;
        if (in == nullptr || channels == 0 || self.filter.size() != static_cast<std::size_t>(channels) * 2) {
            ma_silence_pcm_frames(out, frames, ma_format_f32, channels);  // Never happens; silence rather than garbage.
            return;
        }

        // Acquire: targets set before the jump was asked for are the ones taken.
        const bool jump = self.jumpToTargets.exchange(false, std::memory_order_acquire);
        const float gainTarget = self.targetGain.load(std::memory_order_relaxed);
        const float coefficientTarget = self.targetCoefficient.load(std::memory_order_relaxed);
        if (jump) {
            self.gain = gainTarget;
            self.coefficient = coefficientTarget;
        }
        const float gainStep = (gainTarget - self.gain) / static_cast<float>(frames);
        const float coefficientStep = (coefficientTarget - self.coefficient) / static_cast<float>(frames);

        float gain = self.gain;
        float coefficient = self.coefficient;
        float* const filter = self.filter.data();
        for (ma_uint32 frame = 0; frame < frames; ++frame) {
            gain += gainStep;
            coefficient += coefficientStep;
            for (ma_uint32 channel = 0; channel < channels; ++channel) {
                float& first = filter[2 * channel];
                float& second = filter[2 * channel + 1];
                const std::size_t sample = static_cast<std::size_t>(frame) * channels + channel;
                first += coefficient * (in[sample] - first);
                second += coefficient * (first - second);
                out[sample] = second * gain;
            }
        }
        self.gain = gainTarget;
        self.coefficient = coefficientTarget;

        // A filter ringing down to silence ends in denormals, which are slow on some CPUs.
        for (float& value : self.filter) {
            if (std::abs(value) < 1.0e-20f) { value = 0.0f; }
        }
    }

}  // extern "C"

namespace {

    const ma_node_vtable g_MuffleNodeVtable = {FrostMuffleNodeProcess, nullptr, 1, 1, 0};

    // --- The decoded sound, as a data source that tracks where it is ---------------------------
    //
    // The sound reads its decoder through this: reads, seeks, format and length go straight to the
    // decoder (so looping and the end of the sound behave exactly as with the decoder itself), and
    // where it is is kept in an atomic as it reads and seeks, on the mixing thread. So the main
    // thread knows where a voice is without touching the decoder while it is being read (which
    // miniaudio's own cursor query would). A seek asked for but not yet made (the mixing thread makes
    // it) is where the voice is, as far as anyone asking is concerned.
    constexpr ma_uint64 kNoPendingSeek = ~ma_uint64{0};

    struct TrackedSourceState {
        ma_decoder* decoder = nullptr;  // Set before the data source exists.
        std::atomic<ma_uint64> cursor{0};
        std::atomic<ma_uint64> pendingSeek{kNoPendingSeek};
    };
    static_assert(std::atomic<ma_uint64>::is_always_lock_free, "The mixing thread must never wait on a lock.");

    // As MuffleNode: only C types (the data source base, a pointer), so that miniaudio's pointer to
    // its base is also a pointer to it.
    struct TrackedSource {
        ma_data_source_base base;
        TrackedSourceState* state;
    };
    static_assert(std::is_standard_layout_v<TrackedSource>, "TrackedSource must be standard-layout to be reached from its base.");
    static_assert(offsetof(TrackedSource, base) == 0, "TrackedSource's ma_data_source_base must come first.");

    [[nodiscard]]
    TrackedSourceState& StateOf(ma_data_source* const source) {
        return *static_cast<TrackedSource*>(source)->state;
    }

}  // namespace

// The tracked data source's callbacks (C language linkage, as the muffle node's). Reads and seeks
// come from the mixing thread (and a seek, when a sound at its end is started again, from the main
// thread while the sound is stopped); the rest from either.
extern "C" {

    static ma_result FrostTrackedSourceRead(ma_data_source* const source, void* const framesOut, const ma_uint64 frameCount,
                                            ma_uint64* const framesRead) {
        TrackedSourceState& state = StateOf(source);
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(state.decoder, framesOut, frameCount, &read);
        state.cursor.fetch_add(read, std::memory_order_relaxed);
        if (framesRead != nullptr) { *framesRead = read; }
        return result;
    }

    static ma_result FrostTrackedSourceSeek(ma_data_source* const source, const ma_uint64 frame) {
        TrackedSourceState& state = StateOf(source);
        const ma_result result = ma_decoder_seek_to_pcm_frame(state.decoder, frame);
        if (result == MA_SUCCESS) { state.cursor.store(frame, std::memory_order_relaxed); }
        // The seek asked for (if this is it) is made.
        ma_uint64 pending = frame;
        state.pendingSeek.compare_exchange_strong(pending, kNoPendingSeek, std::memory_order_release, std::memory_order_relaxed);
        return result;
    }

    static ma_result FrostTrackedSourceGetDataFormat(ma_data_source* const source, ma_format* const format, ma_uint32* const channels,
                                                     ma_uint32* const sampleRate, ma_channel* const channelMap, const size_t channelMapCapacity) {
        return ma_decoder_get_data_format(StateOf(source).decoder, format, channels, sampleRate, channelMap, channelMapCapacity);
    }

    static ma_result FrostTrackedSourceGetCursor(ma_data_source* const source, ma_uint64* const cursor) {
        *cursor = StateOf(source).cursor.load(std::memory_order_relaxed);
        return MA_SUCCESS;
    }

    static ma_result FrostTrackedSourceGetLength(ma_data_source* const source, ma_uint64* const length) {
        return ma_decoder_get_length_in_pcm_frames(StateOf(source).decoder, length);
    }

}  // extern "C"

namespace {

    const ma_data_source_vtable g_TrackedSourceVtable = {FrostTrackedSourceRead,    FrostTrackedSourceSeek,
                                                         FrostTrackedSourceGetDataFormat, FrostTrackedSourceGetCursor,
                                                         FrostTrackedSourceGetLength, nullptr, 0};

    [[nodiscard]]
    glm::vec3 NormalizedOr(const glm::vec3& v, const glm::vec3& fallback) noexcept {
        const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return std::isfinite(length) && length > 1e-6f ? v / length : fallback;
    }

}  // namespace

namespace Frost {

    const char* GetAudioAttenuationName(const AudioAttenuation attenuation) noexcept {
        switch (attenuation) {
            case AudioAttenuation::None: return "none";
            case AudioAttenuation::Inverse: return "inverse";
            case AudioAttenuation::Linear: return "linear";
            case AudioAttenuation::Exponential: return "exponential";
        }
        return "inverse";
    }

    // --- AudioVoice ------------------------------------------------------------------------------

    // Heap-allocated and never moved: miniaudio keeps pointers to the decoder, the sound and the
    // muffle node (and the muffle node to its state). The sound plays into the muffle node, which
    // plays into the mix. Members are declared so that what is used is destroyed after its users.
    struct AudioVoice::Impl {
        // First, so it outlives the decoder reading its bytes.
        std::shared_ptr<const SoundAsset> sound;
        ma_decoder decoder{};
        // Before what points to them: made first, destroyed last.
        TrackedSourceState sourceState;
        TrackedSource source{{}, &sourceState};
        MuffleState muffleState;
        MuffleNode muffle{{}, &muffleState};
        ma_sound maSound{};
        std::uint32_t mixSampleRate = 0;
        std::uint32_t soundSampleRate = 0;  // The decoder's (the sound's own).
        bool decoderReady = false;
        bool sourceReady = false;
        bool soundReady = false;
        bool muffleReady = false;
        // Whether Play has started the muffle node (it is mixed from then on). Until then, muffling
        // set by Apply is taken at once, not glided into (a sound starting behind a wall starts
        // muffled). Main thread only.
        bool started = false;
        // Seeking a sound that has played to the end moves it without clearing miniaudio's end
        // flag (Play does); until then it is not at the end any more.
        bool seekedFromEnd = false;

        Impl() = default;
        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;

        ~Impl() {
            // First out of the mix: ma_node_uninit detaches the muffle node and waits until the mixing
            // thread is done with it (mid-callback, or reading the sound through it). From then on
            // nothing of this voice is reachable from the mixing thread: the sound, the data source it
            // reads and the decoder under that go, then (as members) their states and the asset's
            // bytes.
            if (muffleReady) { ma_node_uninit(&muffle.base, nullptr); }
            if (soundReady) { ma_sound_uninit(&maSound); }
            if (sourceReady) { ma_data_source_uninit(&source.base); }
            if (decoderReady) { ma_decoder_uninit(&decoder); }
        }
    };

    AudioVoice::AudioVoice(std::unique_ptr<Impl> impl) : m_Impl(std::move(impl)) {}

    AudioVoice::~AudioVoice() = default;

    void AudioVoice::Play() {
        if (!m_Impl->started) {
            ma_node_set_state(&m_Impl->muffle.base, ma_node_state_started);
            m_Impl->started = true;
        }
        // At the end, miniaudio starts over from the beginning (or from where it was sought to).
        ma_sound_start(&m_Impl->maSound);
        m_Impl->seekedFromEnd = false;
    }

    void AudioVoice::Pause() { ma_sound_stop(&m_Impl->maSound); }

    bool AudioVoice::IsPlaying() const { return ma_sound_is_playing(&m_Impl->maSound) != MA_FALSE; }

    bool AudioVoice::IsAtEnd() const { return !m_Impl->seekedFromEnd && ma_sound_at_end(&m_Impl->maSound) != MA_FALSE; }

    double AudioVoice::GetTime() const {
        // From the data source's own record (see TrackedSourceState), never from the decoder, which
        // the mixing thread may be reading.
        const TrackedSourceState& state = m_Impl->sourceState;
        const ma_uint64 pending = state.pendingSeek.load(std::memory_order_acquire);
        const ma_uint64 frame = pending != kNoPendingSeek ? pending : state.cursor.load(std::memory_order_relaxed);
        if (m_Impl->soundSampleRate == 0) { return 0.0; }
        return std::min(static_cast<double>(frame) / m_Impl->soundSampleRate, GetLength());
    }

    // From the asset (known exactly since it was imported), rather than the decoder (which for an
    // MP3 would scan the whole stream to find out).
    double AudioVoice::GetLength() const { return m_Impl->sound->sound.info.GetDuration(); }

    void AudioVoice::Seek(const double seconds) {
        const SoundInfo& info = m_Impl->sound->sound.info;
        const double clamped = std::clamp(std::isfinite(seconds) ? seconds : 0.0, 0.0, GetLength());
        const auto frame = std::min(static_cast<ma_uint64>(clamped * info.sampleRate), static_cast<ma_uint64>(info.frameCount));

        if (ma_sound_at_end(&m_Impl->maSound) != MA_FALSE) { m_Impl->seekedFromEnd = frame < info.frameCount; }
        // Where it is from now on, as far as GetTime is concerned, until the mixing thread makes the
        // seek (see TrackedSourceState).
        m_Impl->sourceState.pendingSeek.store(frame, std::memory_order_release);
        ma_sound_seek_to_pcm_frame(&m_Impl->maSound, frame);
    }

    void AudioVoice::Apply(const AudioVoiceSettings& settings) {
        ma_sound* const sound = &m_Impl->maSound;

        // An invalid pitch (not above 0) plays as recorded, as an invalid volume does.
        const float pitch = Finite(settings.pitch, 1.0f) > 0.0f ? std::clamp(settings.pitch, kMinPitch, kMaxPitch) : 1.0f;
        ma_sound_set_pitch(sound, pitch);
        ma_sound_set_looping(sound, settings.looping ? MA_TRUE : MA_FALSE);

        // The volume is applied by the muffle node too (its gain is the volume times the muffling),
        // rather than by ma_sound_set_volume, whose volume the mixing thread reads as a plain float.
        // Only atomics are shared with the mixing thread (see MuffleState).
        MuffleState& muffle = m_Impl->muffleState;
        const float volume = std::clamp(Finite(settings.volume, 1.0f), 0.0f, kMaxVolume);
        muffle.targetGain.store(volume * std::clamp(Finite(settings.muffleGain, 1.0f), 0.0f, 1.0f), std::memory_order_relaxed);
        muffle.targetCoefficient.store(LowPassCoefficient(Finite(settings.lowPassCutoff, kAudioNoLowPass), m_Impl->mixSampleRate),
                                       std::memory_order_relaxed);
        // Not mixed yet (see Impl::started): taken at once by the first block. Release: the targets
        // above are what that block sees.
        if (!m_Impl->started) { muffle.jumpToTargets.store(true, std::memory_order_release); }

        ma_sound_set_spatialization_enabled(sound, settings.spatial ? MA_TRUE : MA_FALSE);
        if (!settings.spatial) { return; }

        const float minDistance = std::max(Finite(settings.minDistance, 100.0f), kMinDistance);
        const float maxDistance = std::max(Finite(settings.maxDistance, minDistance), minDistance);
        ma_sound_set_position(sound, Finite(settings.position.x, 0.0f), Finite(settings.position.y, 0.0f),
                              Finite(settings.position.z, 0.0f));
        ma_sound_set_min_distance(sound, minDistance);
        ma_sound_set_max_distance(sound, maxDistance);
        const bool fades = settings.attenuation != AudioAttenuation::None;
        ma_sound_set_rolloff(sound, fades ? std::max(Finite(settings.rolloff, 1.0f), 0.0f) : 0.0f);
        ma_sound_set_attenuation_model(sound, ToMiniaudio(settings.attenuation));
    }

    const std::shared_ptr<const SoundAsset>& AudioVoice::GetSound() const noexcept { return m_Impl->sound; }

    // --- AudioEngine ------------------------------------------------------------------------------

    // Heap-allocated and never moved: miniaudio keeps pointers into the context and the engine.
    struct AudioEngine::Impl {
        ma_context context{};
        ma_engine engine{};
        bool contextReady = false;
        bool engineReady = false;
        bool audible = false;
        AudioOutput output = AudioOutput::Default;
        std::string deviceName;
        float masterVolume = 1.0f;

        Impl() = default;
        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;

        ~Impl() {
            if (engineReady) { ma_engine_uninit(&engine); }
            if (contextReady) { ma_context_uninit(&context); }
        }

        // The engine on the default device. False (nothing to clean up) if it cannot be opened.
        bool InitDefault() {
            ma_engine_config config = ma_engine_config_init();
            if (ma_engine_init(&config, &engine) != MA_SUCCESS) { return false; }
            engineReady = true;

            // miniaudio's own last resort, when no real backend works, is its null backend: a silent
            // device. That is running silently, not playing to a device.
            const ma_device* const device = ma_engine_get_device(&engine);
            if (device == nullptr || device->pContext == nullptr || device->pContext->backend == ma_backend_null) {
                std::cerr << "[Audio] No audio device was found; running without sound.\n";
                output = AudioOutput::Silent;
                deviceName = "(silent)";
                return true;
            }
            audible = true;
            deviceName = device->playback.name[0] != '\0' ? device->playback.name : "(default device)";
            return true;
        }

        // The engine on miniaudio's null backend: a device that consumes audio in real time.
        bool InitSilent() {
            const ma_backend backends[] = {ma_backend_null};
            if (ma_context_init(backends, 1, nullptr, &context) != MA_SUCCESS) { return false; }
            contextReady = true;

            ma_engine_config config = ma_engine_config_init();
            config.pContext = &context;
            if (ma_engine_init(&config, &engine) != MA_SUCCESS) { return false; }
            engineReady = true;
            deviceName = "(silent)";
            return true;
        }

        bool InitManual(const std::uint32_t sampleRate, const std::uint32_t channels) {
            ma_engine_config config = ma_engine_config_init();
            config.noDevice = MA_TRUE;
            config.sampleRate = sampleRate;
            config.channels = channels;
            if (ma_engine_init(&config, &engine) != MA_SUCCESS) { return false; }
            engineReady = true;
            deviceName = "(manual)";
            return true;
        }
    };

    AudioEngine::AudioEngine(const AudioEngineSettings& settings) : m_Impl(std::make_unique<Impl>()) {
        m_Impl->output = settings.output;

        switch (settings.output) {
            case AudioOutput::Default:
                if (m_Impl->InitDefault()) { break; }
                // No device (or it cannot be opened): run silently rather than not at all.
                std::cerr << "[Audio] Could not open the audio device; running without sound.\n";
                m_Impl->output = AudioOutput::Silent;
                m_Impl->InitSilent();
                break;
            case AudioOutput::Silent: m_Impl->InitSilent(); break;
            case AudioOutput::Manual:
                if (settings.sampleRate == 0 || settings.channels == 0) {
                    throw std::invalid_argument("Manual audio output needs a sample rate and a channel count.");
                }
                m_Impl->InitManual(settings.sampleRate, settings.channels);
                break;
        }

        if (!m_Impl->engineReady) {
            std::cerr << "[Audio] Audio is unavailable; sounds will not play.\n";
            m_Impl->deviceName = "(unavailable)";
            return;
        }

        // One listener, at the origin looking down -Z with +Y up (the world's convention).
        ma_engine_listener_set_world_up(&m_Impl->engine, 0, 0.0f, 1.0f, 0.0f);
    }

    AudioEngine::~AudioEngine() = default;

    bool AudioEngine::IsAudible() const noexcept { return m_Impl->audible; }

    const std::string& AudioEngine::GetDeviceName() const noexcept { return m_Impl->deviceName; }

    std::uint32_t AudioEngine::GetSampleRate() const noexcept {
        return m_Impl->engineReady ? ma_engine_get_sample_rate(&m_Impl->engine) : 0;
    }

    std::uint32_t AudioEngine::GetChannels() const noexcept {
        return m_Impl->engineReady ? ma_engine_get_channels(&m_Impl->engine) : 0;
    }

    void AudioEngine::SetListener(const AudioListener& listener) {
        if (!m_Impl->engineReady) { return; }

        const glm::vec3 forward = NormalizedOr(listener.forward, glm::vec3(0.0f, 0.0f, -1.0f));
        const glm::vec3 up = NormalizedOr(listener.up, glm::vec3(0.0f, 1.0f, 0.0f));
        ma_engine_listener_set_position(&m_Impl->engine, 0, Finite(listener.position.x, 0.0f), Finite(listener.position.y, 0.0f),
                                        Finite(listener.position.z, 0.0f));
        ma_engine_listener_set_direction(&m_Impl->engine, 0, forward.x, forward.y, forward.z);
        ma_engine_listener_set_world_up(&m_Impl->engine, 0, up.x, up.y, up.z);
    }

    void AudioEngine::SetMasterVolume(const float volume) {
        m_Impl->masterVolume = std::clamp(Finite(volume, 1.0f), 0.0f, kMaxVolume);
        if (m_Impl->engineReady) { ma_engine_set_volume(&m_Impl->engine, m_Impl->masterVolume); }
    }

    float AudioEngine::GetMasterVolume() const noexcept { return m_Impl->masterVolume; }

    std::unique_ptr<AudioVoice> AudioEngine::CreateVoice(std::shared_ptr<const SoundAsset> sound) {
        if (sound == nullptr) { throw std::invalid_argument("AudioEngine::CreateVoice: no sound."); }
        if (!m_Impl->engineReady) { throw AudioError("Audio is unavailable, so '" + sound->name + "' cannot be played."); }

        auto impl = std::make_unique<AudioVoice::Impl>();
        impl->sound = std::move(sound);
        const SoundData& data = impl->sound->sound;

        // Decoded as it plays, to 32-bit float (the mixer's format) at its own rate and channel
        // count (the mixer converts those). The format is known, so it is not probed for.
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        config.encodingFormat = data.info.format == SoundFormat::Mp3 ? ma_encoding_format_mp3 : ma_encoding_format_wav;
        if (ma_decoder_init_memory(data.encoded.data(), data.encoded.size(), &config, &impl->decoder) != MA_SUCCESS) {
            throw AudioError("'" + impl->sound->name + "' could not be decoded for playback.");
        }
        impl->decoderReady = true;
        ma_uint32 soundSampleRate = 0;
        ma_decoder_get_data_format(&impl->decoder, nullptr, nullptr, &soundSampleRate, nullptr, 0);
        impl->soundSampleRate = soundSampleRate;

        // The data source the sound reads (its state complete before miniaudio can use it).
        impl->sourceState.decoder = &impl->decoder;
        ma_data_source_config sourceConfig = ma_data_source_config_init();
        sourceConfig.vtable = &g_TrackedSourceVtable;
        if (ma_data_source_init(&sourceConfig, &impl->source.base) != MA_SUCCESS) {
            throw AudioError("'" + impl->sound->name + "' could not be set up for playback.");
        }
        impl->sourceReady = true;

        // Its muffle node, in the mix (its state complete before miniaudio can call it); then the
        // sound, into the muffle node.
        impl->mixSampleRate = ma_engine_get_sample_rate(&m_Impl->engine);
        impl->muffleState.channels = ma_engine_get_channels(&m_Impl->engine);
        impl->muffleState.filter.assign(static_cast<std::size_t>(impl->muffleState.channels) * 2, 0.0f);
        ma_node_config muffleConfig = ma_node_config_init();
        muffleConfig.vtable = &g_MuffleNodeVtable;
        muffleConfig.pInputChannels = &impl->muffleState.channels;
        muffleConfig.pOutputChannels = &impl->muffleState.channels;
        muffleConfig.initialState = ma_node_state_stopped;  // Started by the first Play.
        if (ma_node_init(ma_engine_get_node_graph(&m_Impl->engine), &muffleConfig, nullptr, &impl->muffle.base) != MA_SUCCESS) {
            throw AudioError("'" + impl->sound->name + "' could not be set up for playback.");
        }
        impl->muffleReady = true;
        ma_node_attach_output_bus(&impl->muffle.base, 0, ma_engine_get_endpoint(&m_Impl->engine), 0);

        if (ma_sound_init_from_data_source(&m_Impl->engine, &impl->source.base, MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT, nullptr,
                                           &impl->maSound) != MA_SUCCESS) {
            throw AudioError("'" + impl->sound->name + "' could not be set up for playback.");
        }
        impl->soundReady = true;
        ma_node_attach_output_bus(&impl->maSound, 0, &impl->muffle.base, 0);

        // No Doppler shift from miniaudio: voices have no velocity there (and its speed of sound is in
        // meters). AudioSystem works it out, into the pitch.
        ma_sound_set_doppler_factor(&impl->maSound, 0.0f);
        return std::unique_ptr<AudioVoice>(new AudioVoice(std::move(impl)));
    }

    void AudioEngine::Render(const std::span<float> frames) {
        if (m_Impl->output != AudioOutput::Manual || !m_Impl->engineReady) {
            throw AudioError("AudioEngine::Render needs Manual audio output.");
        }
        const std::uint32_t channels = GetChannels();
        ma_uint64 read = 0;
        ma_engine_read_pcm_frames(&m_Impl->engine, frames.data(), frames.size() / channels, &read);
        // Whatever was not produced is silence.
        std::fill(frames.begin() + static_cast<std::ptrdiff_t>(read * channels), frames.end(), 0.0f);
    }

}  // namespace Frost
