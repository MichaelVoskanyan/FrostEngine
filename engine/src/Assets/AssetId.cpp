#include <Frost/Assets/AssetId.hpp>

#include <chrono>
#include <mutex>
#include <random>

namespace Frost {

    AssetId AssetId::Generate() {
        // One generator per process, seeded from the OS entropy source (and the clock, in case a
        // platform's random_device is deterministic).
        static std::mutex mutex;
        static std::mt19937_64 generator = [] {
            std::random_device device;
            std::seed_seq seed{device(), device(), device(), device(),
                               static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
            return std::mt19937_64(seed);
        }();

        const std::lock_guard lock(mutex);

        AssetId id;
        do {
            id.high = generator();
            id.low = generator();
            // Version 4 (random) and RFC 4122 variant bits, so the value is a well-formed UUID.
            id.high = (id.high & ~0xF000ull) | 0x4000ull;
            id.low = (id.low & ~(0xC000000000000000ull)) | 0x8000000000000000ull;
        } while (!id.IsValid());

        return id;
    }

    std::optional<AssetId> AssetId::Parse(const std::string_view text) {
        if (text.size() != 32) { return std::nullopt; }

        AssetId id;
        for (std::size_t i = 0; i < 32; ++i) {
            const char c = text[i];
            std::uint64_t digit = 0;
            if (c >= '0' && c <= '9') { digit = static_cast<std::uint64_t>(c - '0'); }
            else if (c >= 'a' && c <= 'f') { digit = static_cast<std::uint64_t>(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { digit = static_cast<std::uint64_t>(c - 'A' + 10); }
            else { return std::nullopt; }

            std::uint64_t& half = i < 16 ? id.high : id.low;
            half = (half << 4) | digit;
        }
        return id;
    }

    std::string AssetId::ToString() const {
        constexpr char kHex[] = "0123456789abcdef";
        std::string text(32, '0');
        for (std::size_t i = 0; i < 16; ++i) {
            text[i] = kHex[(high >> (60 - 4 * i)) & 0xF];
            text[16 + i] = kHex[(low >> (60 - 4 * i)) & 0xF];
        }
        return text;
    }

}  // namespace Frost
