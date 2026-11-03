#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace Frost {

    // A decoded image in the one layout the engine uploads to the GPU: 8-bit RGBA, rows top to
    // bottom, tightly packed (width * 4 bytes per row). Whether the values are sRGB-encoded or
    // linear is not a property of the pixels but of how they are sampled; see TextureSettings.
    struct ImageData {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> pixels;  // width * height * 4 bytes.

        // A width x height image filled with a single RGBA color (e.g. a 1x1 white placeholder).
        [[nodiscard]]
        static ImageData SolidColor(std::uint32_t width, std::uint32_t height, std::array<std::uint8_t, 4> rgba);
    };

    // Decodes PNG and JPEG images (the two formats glTF requires) into ImageData. Images with
    // fewer than four channels are expanded (gray -> RGB, missing alpha -> opaque); 16-bit PNGs
    // are reduced to 8 bits.
    class ImageImporter final {
    public:
        ImageImporter() = delete;

        // Throws ImportError if the file cannot be read or is not a decodable PNG/JPEG.
        [[nodiscard]]
        static ImageData Import(const std::filesystem::path& path);

        // Decodes an in-memory PNG/JPEG (e.g. one embedded in a glTF binary). `debugName` is only
        // used in error messages.
        [[nodiscard]]
        static ImageData Decode(std::span<const std::byte> encoded, const std::string& debugName = "<memory>");
    };

}  // namespace Frost
