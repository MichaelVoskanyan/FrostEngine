#include <Frost/Assets/Image.hpp>

#include <Frost/Assets/ImportError.hpp>

#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

// stb_image is compiled into this translation unit only. Decoding is restricted to the formats the
// engine (and glTF) actually use, and to memory buffers: files are read through std::filesystem
// paths here so that non-ASCII paths work on every platform.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_FAILURE_USERMSG

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <stb_image.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace Frost {

    ImageData ImageData::SolidColor(const std::uint32_t width, const std::uint32_t height,
                                    const std::array<std::uint8_t, 4> rgba) {
        ImageData image;
        image.width = width;
        image.height = height;
        image.pixels.resize(static_cast<std::size_t>(width) * height * 4);

        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            image.pixels[i + 0] = rgba[0];
            image.pixels[i + 1] = rgba[1];
            image.pixels[i + 2] = rgba[2];
            image.pixels[i + 3] = rgba[3];
        }

        return image;
    }

    ImageData ImageImporter::Import(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) { throw ImportError("Failed to open image file '" + path.string() + "'."); }

        const std::streamoff size = file.tellg();
        if (size < 0) { throw ImportError("Failed to read image file '" + path.string() + "'."); }

        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        file.seekg(0);
        if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            throw ImportError("Failed to read image file '" + path.string() + "'.");
        }

        return Decode(bytes, path.string());
    }

    ImageData ImageImporter::Decode(const std::span<const std::byte> encoded, const std::string& debugName) {
        if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw ImportError("Image '" + debugName + "' is too large to decode.");
        }

        int width = 0;
        int height = 0;
        int channelsInFile = 0;

        // Always request RGBA so every texture shares one upload path and one GPU format.
        const std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
                stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()),
                                      static_cast<int>(encoded.size()), &width, &height, &channelsInFile,
                                      STBI_rgb_alpha),
                &stbi_image_free);

        if (decoded == nullptr) {
            const char* const reason = stbi_failure_reason();
            throw ImportError("Failed to decode image '" + debugName + "': " +
                                     (reason != nullptr ? reason : "unknown error") + ".");
        }

        if (width <= 0 || height <= 0) { throw ImportError("Image '" + debugName + "' has an empty size."); }

        ImageData image;
        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);

        const std::size_t byteCount = static_cast<std::size_t>(image.width) * image.height * 4;
        image.pixels.assign(decoded.get(), decoded.get() + byteCount);

        return image;
    }

}  // namespace Frost
