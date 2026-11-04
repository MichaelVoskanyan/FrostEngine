#pragma once

#include <cstdint>

namespace Frost {

    enum class AssetType : std::uint8_t {
        Mesh,     // A model (.fmesh, imported from glTF).
        Texture,  // An image (.ftex, imported from PNG/JPEG).
    };

}  // namespace Frost
