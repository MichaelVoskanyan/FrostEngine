#include <Frost/Assets/Sound.hpp>

#include <Frost/Assets/ImportError.hpp>
#include <Frost/Platform/Filesystem.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>

namespace {

    using Bytes = std::span<const std::byte>;

    // Multi-byte values are assembled byte by byte: WAV is little-endian, MP3 headers big-endian,
    // whatever the host is. Callers check bounds first.
    [[nodiscard]]
    std::uint32_t ByteAt(const Bytes bytes, const std::size_t offset) {
        return std::to_integer<std::uint32_t>(bytes[offset]);
    }

    [[nodiscard]]
    std::uint16_t ReadU16LE(const Bytes bytes, const std::size_t offset) {
        return static_cast<std::uint16_t>(ByteAt(bytes, offset) | (ByteAt(bytes, offset + 1) << 8));
    }

    [[nodiscard]]
    std::uint32_t ReadU32LE(const Bytes bytes, const std::size_t offset) {
        return ByteAt(bytes, offset) | (ByteAt(bytes, offset + 1) << 8) | (ByteAt(bytes, offset + 2) << 16) |
               (ByteAt(bytes, offset + 3) << 24);
    }

    [[nodiscard]]
    std::uint32_t ReadU32BE(const Bytes bytes, const std::size_t offset) {
        return (ByteAt(bytes, offset) << 24) | (ByteAt(bytes, offset + 1) << 16) | (ByteAt(bytes, offset + 2) << 8) |
               ByteAt(bytes, offset + 3);
    }

    // Whether `tag` (ASCII) is at `offset`.
    [[nodiscard]]
    bool HasTag(const Bytes bytes, const std::size_t offset, const std::string_view tag) {
        return offset <= bytes.size() && tag.size() <= bytes.size() - offset &&
               std::memcmp(bytes.data() + offset, tag.data(), tag.size()) == 0;
    }

    [[noreturn]]
    void Fail(const std::string& debugName, const std::string& problem) {
        throw Frost::ImportError("Sound '" + debugName + "' " + problem + ".");
    }

    void ValidateChannels(const std::uint16_t channels, const std::string& debugName) {
        if (channels == 0 || channels > Frost::kMaxSoundChannels) {
            Fail(debugName, "has " + std::to_string(channels) + " channels (1 to " + std::to_string(Frost::kMaxSoundChannels) +
                                    " are supported)");
        }
    }

    // What every sound must satisfy, whatever its format.
    void ValidateInfo(const Frost::SoundInfo& info, const std::string& debugName) {
        ValidateChannels(info.channels, debugName);
        if (info.sampleRate < Frost::kMinSoundSampleRate || info.sampleRate > Frost::kMaxSoundSampleRate) {
            Fail(debugName, "has a sample rate of " + std::to_string(info.sampleRate) + " Hz (" +
                                    std::to_string(Frost::kMinSoundSampleRate) + " to " +
                                    std::to_string(Frost::kMaxSoundSampleRate) + " Hz are supported)");
        }
        if (info.frameCount == 0) { Fail(debugName, "contains no audio"); }
    }

    // --- WAV -------------------------------------------------------------------------------------

    constexpr std::uint16_t kWaveFormatPcm = 0x0001;
    constexpr std::uint16_t kWaveFormatFloat = 0x0003;
    constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;

    // The WAVE_FORMAT_EXTENSIBLE sub-format GUID after its first two bytes (which hold the actual
    // format tag): {xxxxxxxx-0000-0010-8000-00AA00389B71}, as stored (little-endian fields).
    constexpr std::array<std::uint8_t, 14> kExtensibleGuidTail = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                                                  0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

    [[nodiscard]]
    bool IsWav(const Bytes bytes) {
        return (HasTag(bytes, 0, "RIFF") || HasTag(bytes, 0, "RF64") || HasTag(bytes, 0, "BW64")) && HasTag(bytes, 8, "WAVE");
    }

    [[nodiscard]]
    Frost::SoundInfo ProbeWav(const Bytes bytes, const std::string& debugName) {
        if (!HasTag(bytes, 0, "RIFF")) {
            Fail(debugName, "is an RF64/BW64 WAV (the format for files over 4 GB), which is not supported");
        }

        // The chunks end where the RIFF header says, unless it claims more than the file holds (or
        // is a streaming writer's placeholder): then at the end of the file.
        const std::uint64_t riffSize = ReadU32LE(bytes, 4);
        const std::size_t end = riffSize < 4 || riffSize + 8 > bytes.size() ? bytes.size() : static_cast<std::size_t>(riffSize + 8);

        bool haveFormat = false;
        std::uint16_t formatTag = 0;
        std::uint16_t channels = 0;
        std::uint32_t sampleRate = 0;
        std::uint16_t blockAlign = 0;
        std::uint16_t bitsPerSample = 0;
        std::optional<std::uint64_t> dataSize;

        std::size_t offset = 12;
        while (offset < end && end - offset >= 8) {
            const std::uint32_t size = ReadU32LE(bytes, offset + 4);
            const std::size_t body = offset + 8;
            const std::size_t available = end - body;

            if (HasTag(bytes, offset, "fmt ")) {
                if (size < 16 || size > available) { Fail(debugName, "has a malformed format chunk"); }
                formatTag = ReadU16LE(bytes, body);
                channels = ReadU16LE(bytes, body + 2);
                sampleRate = ReadU32LE(bytes, body + 4);
                blockAlign = ReadU16LE(bytes, body + 12);
                bitsPerSample = ReadU16LE(bytes, body + 14);

                if (formatTag == kWaveFormatExtensible) {
                    // cbSize, valid bits, channel mask, then the sub-format GUID, whose first two
                    // bytes are the format tag proper.
                    if (size < 40) { Fail(debugName, "has a malformed extensible format chunk"); }
                    formatTag = ReadU16LE(bytes, body + 24);
                    for (std::size_t i = 0; i < kExtensibleGuidTail.size(); ++i) {
                        if (ByteAt(bytes, body + 26 + i) != kExtensibleGuidTail[i]) {
                            Fail(debugName, "has an extensible format with an unknown sub-format, which is not supported");
                        }
                    }
                }
                haveFormat = true;
            }
            else if (HasTag(bytes, offset, "data") && !dataSize.has_value()) {
                // A data chunk that claims more than there is (a recording that was cut off) is
                // what is there.
                dataSize = std::min<std::uint64_t>(size, available);
            }

            // Chunks are padded to an even size. One running past the end (a cut-off data chunk)
            // ends the file.
            const std::uint64_t next = std::uint64_t{body} + size + (size & 1u);
            if (next > end) { break; }
            offset = static_cast<std::size_t>(next);
        }

        if (!haveFormat) { Fail(debugName, "is not a valid WAV file (it has no format chunk)"); }
        if (!dataSize.has_value()) { Fail(debugName, "is not a valid WAV file (it has no data chunk)"); }

        Frost::SoundInfo info;
        switch (formatTag) {
            case kWaveFormatPcm:
                if (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) {
                    Fail(debugName, "has " + std::to_string(bitsPerSample) +
                                            "-bit PCM samples, which are not supported (8, 16, 24 or 32-bit are)");
                }
                info.format = Frost::SoundFormat::WavPcm;
                break;
            case kWaveFormatFloat:
                if (bitsPerSample != 32 && bitsPerSample != 64) {
                    Fail(debugName, "has " + std::to_string(bitsPerSample) +
                                            "-bit float samples, which are not supported (32 or 64-bit are)");
                }
                info.format = Frost::SoundFormat::WavFloat;
                break;
            default: {
                char tag[8];
                std::snprintf(tag, sizeof(tag), "0x%04X", static_cast<unsigned>(formatTag));
                Fail(debugName, std::string("is a compressed WAV (format ") + tag +
                                        "), which is not supported; save it as PCM WAV or as MP3");
            }
        }

        info.sampleRate = sampleRate;
        info.channels = channels;
        info.bitsPerSample = bitsPerSample;
        ValidateChannels(info.channels, debugName);  // Before dividing by the block size.
        if (blockAlign != info.channels * (bitsPerSample / 8)) {
            Fail(debugName, "has an inconsistent block size (" + std::to_string(blockAlign) + " bytes for " +
                                    std::to_string(info.channels) + " channels of " + std::to_string(bitsPerSample) + " bits)");
        }

        info.frameCount = *dataSize / blockAlign;
        info.bitRate = info.sampleRate * blockAlign * 8;
        ValidateInfo(info, debugName);
        return info;
    }

    // --- MP3 -------------------------------------------------------------------------------------

    // One MPEG audio frame header.
    struct MpegFrame {
        int version = 0;  // 0 = MPEG-1, 1 = MPEG-2, 2 = MPEG-2.5.
        int layer = 0;    // 1, 2 or 3.
        std::uint32_t sampleRate = 0;
        std::uint16_t channels = 0;
        std::uint32_t bytes = 0;    // The whole frame, header included.
        std::uint32_t samples = 0;  // Sample frames it decodes to.

        // Whether another frame belongs to the same stream.
        [[nodiscard]]
        bool SameStream(const MpegFrame& other) const noexcept {
            return version == other.version && layer == other.layer && sampleRate == other.sampleRate;
        }
    };

    // Kilobits per second, by [MPEG-1 ? 0 : 1][layer - 1][bit rate index]. Index 0 is "free format"
    // (not supported), 15 is invalid.
    constexpr std::uint16_t kMpegBitRates[2][3][15] = {
            {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
             {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
             {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
            {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
             {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
             {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},
    };

    // Hz, by [version][sample rate index].
    constexpr std::uint32_t kMpegSampleRates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};

    // The frame header at `offset`, if there is a valid one (free-format frames are not).
    [[nodiscard]]
    std::optional<MpegFrame> ParseMpegFrame(const Bytes bytes, const std::size_t offset) {
        if (offset > bytes.size() || bytes.size() - offset < 4) { return std::nullopt; }
        const std::uint32_t header = ReadU32BE(bytes, offset);

        if ((header & 0xFFE00000u) != 0xFFE00000u) { return std::nullopt; }  // Frame sync.
        const std::uint32_t versionBits = (header >> 19) & 3u;
        const std::uint32_t layerBits = (header >> 17) & 3u;
        const std::uint32_t bitRateIndex = (header >> 12) & 15u;
        const std::uint32_t sampleRateIndex = (header >> 10) & 3u;
        const std::uint32_t padding = (header >> 9) & 1u;
        const std::uint32_t channelMode = (header >> 6) & 3u;
        const std::uint32_t emphasis = header & 3u;
        if (versionBits == 1 || layerBits == 0 || bitRateIndex == 0 || bitRateIndex == 15 || sampleRateIndex == 3 ||
            emphasis == 2) {
            return std::nullopt;  // Reserved values: not a frame.
        }

        MpegFrame frame;
        frame.version = versionBits == 3 ? 0 : versionBits == 2 ? 1 : 2;
        frame.layer = 4 - static_cast<int>(layerBits);
        frame.sampleRate = kMpegSampleRates[frame.version][sampleRateIndex];
        frame.channels = channelMode == 3 ? 1 : 2;

        const std::uint32_t bitRate = kMpegBitRates[frame.version == 0 ? 0 : 1][frame.layer - 1][bitRateIndex] * 1000u;
        if (frame.layer == 1) {
            frame.samples = 384;
            frame.bytes = (12 * bitRate / frame.sampleRate + padding) * 4;
        }
        else if (frame.layer == 2 || frame.version == 0) {
            frame.samples = 1152;
            frame.bytes = 144 * bitRate / frame.sampleRate + padding;
        }
        else {
            frame.samples = 576;  // Layer III of MPEG-2/2.5: half the samples, half the bytes per bit rate.
            frame.bytes = 72 * bitRate / frame.sampleRate + padding;
        }
        if (frame.bytes <= 4) { return std::nullopt; }
        return frame;
    }

    // Frames that must follow a frame header back to back for it to count as the start of the
    // stream (fewer if the stream ends first). Two headers in a row occur by chance in a large
    // enough non-audio file; five do not.
    constexpr int kMpegSyncFrames = 5;

    // The frame at `offset`, if it starts a run of frames of the same stream (as `like`, if given)
    // that ends exactly at `end` or lasts kMpegSyncFrames frames.
    [[nodiscard]]
    std::optional<MpegFrame> ConfirmMpegFrame(const Bytes bytes, const std::size_t offset, const std::size_t end,
                                              const std::optional<MpegFrame>& like) {
        const std::optional<MpegFrame> first = ParseMpegFrame(bytes, offset);
        if (!first.has_value() || (like.has_value() && !like->SameStream(*first))) { return std::nullopt; }

        std::size_t position = offset;
        for (int i = 0; i < kMpegSyncFrames; ++i) {
            const std::optional<MpegFrame> frame = ParseMpegFrame(bytes, position);
            if (!frame.has_value() || !first->SameStream(*frame) || frame->bytes > end - position) { return std::nullopt; }
            position += frame->bytes;
            if (position == end) { break; }
        }
        return first;
    }

    // The first frame at or after `offset` that starts a confirmed run (see ConfirmMpegFrame).
    [[nodiscard]]
    std::optional<std::size_t> FindMpegFrame(const Bytes bytes, std::size_t offset, const std::size_t end,
                                             const std::optional<MpegFrame>& like) {
        for (; offset < end && end - offset >= 4; ++offset) {
            if (std::to_integer<std::uint8_t>(bytes[offset]) == 0xFF && ConfirmMpegFrame(bytes, offset, end, like).has_value()) {
                return offset;
            }
        }
        return std::nullopt;
    }

    // Where the audio frames of an MP3 file can be: after its ID3v2 tag(s), before its APEv2 and
    // ID3v1 tags.
    [[nodiscard]]
    std::pair<std::size_t, std::size_t> MpegAudioRange(const Bytes bytes) {
        std::size_t begin = 0;
        // ID3v2: "ID3", version (2), flags, size (4 bytes of 7 bits), plus a footer if flagged.
        while (HasTag(bytes, begin, "ID3") && bytes.size() - begin >= 10) {
            std::uint32_t size = 0;
            for (std::size_t i = 6; i < 10; ++i) {
                const std::uint32_t part = ByteAt(bytes, begin + i);
                if (part >= 0x80) { return {begin, bytes.size()}; }  // Not a valid tag; look for frames from here.
                size = (size << 7) | part;
            }
            const std::uint64_t tagEnd = std::uint64_t{begin} + 10 + size + ((ByteAt(bytes, begin + 5) & 0x10u) != 0 ? 10 : 0);
            if (tagEnd > bytes.size()) { return {bytes.size(), bytes.size()}; }
            begin = static_cast<std::size_t>(tagEnd);
        }

        std::size_t end = bytes.size();
        // ID3v1: the last 128 bytes, starting with "TAG".
        if (end - begin >= 128 && HasTag(bytes, end - 128, "TAG")) { end -= 128; }
        // APEv2: a 32-byte footer "APETAGEX", whose size covers the items and the footer (plus a
        // 32-byte header if flagged).
        if (end - begin >= 32 && HasTag(bytes, end - 32, "APETAGEX")) {
            const std::uint64_t size = std::uint64_t{ReadU32LE(bytes, end - 32 + 12)} +
                                       ((ReadU32LE(bytes, end - 32 + 20) & 0x80000000u) != 0 ? 32u : 0u);
            if (size <= end - begin) { end -= static_cast<std::size_t>(size); }
        }
        return {begin, end};
    }

    // The encoder delay and padding (sample frames) a Xing/Info header records in its LAME
    // extension, if it has one. `xingOffset` is where "Xing"/"Info" is, `frameEnd` where its frame
    // ends.
    struct MpegEncoderTrim {
        std::uint32_t delay = 0;
        std::uint32_t padding = 0;
    };

    [[nodiscard]]
    std::optional<MpegEncoderTrim> ReadMpegEncoderTrim(const Bytes bytes, const std::size_t xingOffset, const std::size_t frameEnd) {
        // "Xing"/"Info", u32 flags, then optional fields: frame count, byte count, 100-byte TOC,
        // quality; then the LAME extension, whose encoder delay and padding are 12 bits each at
        // offset 21.
        if (frameEnd - xingOffset < 8) { return std::nullopt; }
        std::size_t position = xingOffset + 8;
        const std::uint32_t flags = ReadU32BE(bytes, xingOffset + 4);
        if ((flags & 1u) != 0) { position += 4; }
        if ((flags & 2u) != 0) { position += 4; }
        if ((flags & 4u) != 0) { position += 100; }
        if ((flags & 8u) != 0) { position += 4; }

        if (position > frameEnd || frameEnd - position < 24) { return std::nullopt; }
        if (!HasTag(bytes, position, "LAME") && !HasTag(bytes, position, "Lavc") && !HasTag(bytes, position, "Lavf")) {
            return std::nullopt;
        }
        return MpegEncoderTrim{.delay = (ByteAt(bytes, position + 21) << 4) | (ByteAt(bytes, position + 22) >> 4),
                               .padding = ((ByteAt(bytes, position + 22) & 0x0Fu) << 8) | ByteAt(bytes, position + 23)};
    }

    // An MP3 decoder's own delay (sample frames): its output lags the stream by this much. Decoders
    // that honor a LAME header skip it along with the encoder delay, and count it as part of the
    // padding at the end (as LAME does).
    constexpr std::uint32_t kMpegDecoderDelay = 528 + 1;

    [[nodiscard]]
    Frost::SoundInfo ProbeMp3(const Bytes bytes, const std::string& debugName) {
        const auto [begin, end] = MpegAudioRange(bytes);

        const std::optional<std::size_t> start = FindMpegFrame(bytes, begin, end, std::nullopt);
        if (!start.has_value()) {
            Fail(debugName, "is neither a WAV nor an MP3 file (no WAV header, and no MPEG audio frames; free-format MP3s "
                            "are not supported)");
        }
        const MpegFrame first = *ParseMpegFrame(bytes, *start);
        if (first.layer != 3) {
            Fail(debugName, "is MPEG audio Layer " + std::string(first.layer == 1 ? "I" : "II") +
                                    ", which is not supported (only Layer III, MP3, is)");
        }

        // A Xing/Info (LAME) or VBRI header in the first frame makes it a header, not audio. It sits
        // after the side information, whose size depends on the version and channel count.
        const std::size_t sideInformation = first.version == 0 ? (first.channels == 1 ? 17 : 32) : (first.channels == 1 ? 9 : 17);
        const std::size_t xingOffset = *start + 4 + sideInformation;
        const std::size_t firstEnd = *start + first.bytes;
        const bool xing = xingOffset + 4 <= firstEnd && (HasTag(bytes, xingOffset, "Xing") || HasTag(bytes, xingOffset, "Info"));
        const bool vbri = *start + 4 + 32 + 4 <= firstEnd && HasTag(bytes, *start + 4 + 32, "VBRI");
        const std::optional<MpegEncoderTrim> trim = xing ? ReadMpegEncoderTrim(bytes, xingOffset, firstEnd) : std::nullopt;

        // Every frame, as decoders take them: one that is not followed by the next frame's header
        // was cut short by a damaged stretch, and is dropped along with the damage (up to the next
        // confirmed frame) - unless nothing follows, when it is the last frame (before trailing
        // bytes that are not audio). A final frame cut off by the end of the file is dropped.
        std::uint64_t frames = 0;
        std::uint64_t samples = 0;
        std::uint64_t audioBytes = 0;
        std::size_t position = (xing || vbri) ? firstEnd : *start;
        while (position < end) {
            const std::optional<MpegFrame> frame = ParseMpegFrame(bytes, position);
            if (!frame.has_value() || !first.SameStream(*frame) || frame->bytes > end - position) {
                const std::optional<std::size_t> next = FindMpegFrame(bytes, position + 1, end, first);
                if (!next.has_value()) { break; }
                position = *next;
                continue;
            }

            const std::size_t frameEnd = position + frame->bytes;
            if (frameEnd != end) {
                const std::optional<MpegFrame> following = ParseMpegFrame(bytes, frameEnd);
                if (!following.has_value() || !first.SameStream(*following)) {
                    if (const std::optional<std::size_t> next = FindMpegFrame(bytes, position + 1, end, first)) {
                        position = *next;  // Cut short by damage: dropped.
                        continue;
                    }
                }
            }

            ++frames;
            samples += frame->samples;
            audioBytes += frame->bytes;
            position = frameEnd;
        }
        if (frames == 0) { Fail(debugName, "contains no audio frames"); }

        // The length decoders that honor a LAME header play: without the encoder delay and the
        // decoder delay at the start, nor the padding at the end (which includes the decoder delay).
        std::uint64_t length = samples;
        if (trim.has_value()) {
            const std::uint64_t skipped = std::uint64_t{trim->delay} + kMpegDecoderDelay +
                                          (trim->padding > kMpegDecoderDelay ? trim->padding - kMpegDecoderDelay : 0);
            if (skipped < samples) { length = samples - skipped; }
        }

        Frost::SoundInfo info;
        info.format = Frost::SoundFormat::Mp3;
        info.sampleRate = first.sampleRate;
        info.channels = first.channels;
        info.bitsPerSample = 0;
        info.frameCount = length;
        info.bitRate = static_cast<std::uint32_t>(audioBytes * 8 * first.sampleRate / samples);
        ValidateInfo(info, debugName);
        return info;
    }

}  // namespace

namespace Frost {

    std::string DescribeSoundFormat(const SoundInfo& info) {
        switch (info.format) {
            case SoundFormat::WavPcm: return "WAV (" + std::to_string(info.bitsPerSample) + "-bit PCM)";
            case SoundFormat::WavFloat: return "WAV (" + std::to_string(info.bitsPerSample) + "-bit float)";
            case SoundFormat::Mp3: return "MP3";
        }
        return "Unknown";
    }

    SoundData SoundImporter::Import(const std::filesystem::path& path) {
        const std::string name = PathToUtf8(path);

        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error) { throw ImportError("Failed to open sound file '" + name + "': " + error.message() + "."); }
        if (size > kMaxSoundFileBytes) {
            throw ImportError("Sound file '" + name + "' is too large (" + std::to_string(size / (1024 * 1024)) +
                              " MB; at most " + std::to_string(kMaxSoundFileBytes / (1024 * 1024)) + " MB).");
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) { throw ImportError("Failed to open sound file '" + name + "'."); }

        SoundData sound;
        sound.encoded.resize(static_cast<std::size_t>(size));
        if (!sound.encoded.empty() &&
            !file.read(reinterpret_cast<char*>(sound.encoded.data()), static_cast<std::streamsize>(sound.encoded.size()))) {
            throw ImportError("Failed to read sound file '" + name + "'.");
        }

        sound.info = Probe(sound.encoded, name);
        return sound;
    }

    SoundInfo SoundImporter::Probe(const std::span<const std::byte> encoded, const std::string& debugName) {
        if (encoded.empty()) { Fail(debugName, "is empty"); }
        if (encoded.size() > kMaxSoundFileBytes) { Fail(debugName, "is too large"); }
        return IsWav(encoded) ? ProbeWav(encoded, debugName) : ProbeMp3(encoded, debugName);
    }

}  // namespace Frost
