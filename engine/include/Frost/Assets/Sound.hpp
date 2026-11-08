#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Frost {

    // How a sound's audio is encoded: the source file's own format, which a sound asset keeps (see
    // SoundData). Stored in .fsound files as a u8; append new values, never renumber.
    enum class SoundFormat : std::uint8_t {
        WavPcm = 0,    // WAV, integer PCM (unsigned 8-bit, or signed 16/24/32-bit little-endian).
        WavFloat = 1,  // WAV, IEEE float PCM (32 or 64-bit little-endian).
        Mp3 = 2,       // MPEG-1/2/2.5 Audio Layer III.
    };

    // What a sound is, as read from its file's headers (nothing is decoded to get it).
    struct SoundInfo {
        SoundFormat format = SoundFormat::WavPcm;
        std::uint32_t sampleRate = 0;     // Frames per second (Hz).
        std::uint16_t channels = 0;       // 1 = mono, 2 = stereo, ...
        std::uint16_t bitsPerSample = 0;  // Per sample as stored (WAV); 0 for compressed formats (MP3).
        // Sample frames (one sample per channel) it plays for. For an MP3 with a LAME header,
        // without the encoder's delay and padding (as decoders that honor the header play it).
        std::uint64_t frameCount = 0;
        // Average encoded bit rate (bits per second): exact for WAV, averaged over the frames for
        // MP3 (which may be variable bit rate).
        std::uint32_t bitRate = 0;

        // How long it plays, in seconds.
        [[nodiscard]]
        double GetDuration() const noexcept {
            return sampleRate == 0 ? 0.0 : static_cast<double>(frameCount) / static_cast<double>(sampleRate);
        }

        [[nodiscard]]
        bool operator==(const SoundInfo&) const noexcept = default;
    };

    // Limits a sound must be within to be imported or read back (see SoundImporter::Probe).
    inline constexpr std::uint32_t kMinSoundSampleRate = 1000;
    inline constexpr std::uint32_t kMaxSoundSampleRate = 384000;
    inline constexpr std::uint16_t kMaxSoundChannels = 8;
    inline constexpr std::uint64_t kMaxSoundFileBytes = std::uint64_t{1} << 30;  // 1 GiB.

    // A sound as a sound asset (.fsound) holds it: the source file's bytes exactly as they were
    // (a complete .wav or .mp3 file, headers and all), and what they contain. Keeping the
    // encoded file rather than decoded samples keeps assets as small as their sources (an MP3
    // decoded to PCM is ~10x larger); the audio system decodes (or streams) it when playing.
    struct SoundData {
        SoundInfo info;
        std::vector<std::byte> encoded;
    };

    // The short name of a format ("WAV (16-bit PCM)", "WAV (32-bit float)", "MP3"), for display.
    [[nodiscard]]
    std::string DescribeSoundFormat(const SoundInfo& info);

    // Reads WAV and MP3 files (the formats AssetImporter imports sounds from) into SoundData,
    // validating them and reading their properties from their headers:
    //   - WAV: RIFF/WAVE with integer PCM (8/16/24/32-bit) or IEEE float (32/64-bit) samples, plain
    //     or WAVE_FORMAT_EXTENSIBLE. Compressed WAVs (ADPCM, mu-law, ...) and RF64 are rejected.
    //     A data chunk that claims more than the file holds (a recording that was cut off) is
    //     taken as what is there.
    //   - MP3: MPEG-1, 2 or 2.5 Layer III, constant or variable bit rate, with or without ID3v2,
    //     ID3v1 and APEv2 tags. Every frame header is read to find the length, which is the
    //     length decoders play: a Xing/Info/VBRI header frame is not audio, a LAME header's encoder
    //     delay and padding (and the decoder's own delay, 529 frames) are left out, and a frame cut
    //     short by a damaged stretch is dropped with it. Free-format streams and Layer I/II files
    //     are rejected.
    // Sounds must have 1 to kMaxSoundChannels channels, a sample rate from kMinSoundSampleRate to
    // kMaxSoundSampleRate, at least one frame, and be at most kMaxSoundFileBytes.
    class SoundImporter final {
    public:
        SoundImporter() = delete;

        // Throws ImportError if the file cannot be read or is not a supported WAV/MP3.
        [[nodiscard]]
        static SoundData Import(const std::filesystem::path& path);

        // The properties of an in-memory WAV or MP3 file (the format is recognized by content,
        // not by name). `debugName` is only used in error messages. Throws ImportError like Import.
        [[nodiscard]]
        static SoundInfo Probe(std::span<const std::byte> encoded, const std::string& debugName = "<memory>");
    };

}  // namespace Frost
