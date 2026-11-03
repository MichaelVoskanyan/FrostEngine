#pragma once

#include <stdexcept>

namespace Frost {

    // Thrown by the asset importers (GltfImporter, ImageImporter) for problems with the asset
    // itself: an unreadable or missing file, malformed or unsupported content, or a broken link to
    // an external file (a .gltf's .bin or image). It is a std::runtime_error, so existing catch
    // sites keep working, but having its own type lets callers treat bad content as a reportable,
    // recoverable condition rather than an engine failure.
    class ImportError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

}  // namespace Frost
