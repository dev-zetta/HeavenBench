/*
 * HeavenBench - PNG image loading
 *
 * Normalizes images decoded by Filament's vendored stb_image library to RGBA8
 * for benchmark-owned UI and sky textures.
 */
#ifndef HEAVENBENCH_SRC_RHI_PNG_IMAGE_H_
#define HEAVENBENCH_SRC_RHI_PNG_IMAGE_H_

#include "engine/asset_file_system.h"
#include "engine/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace heaven {

struct PngImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

[[nodiscard]] Result<PngImage> load_png_rgba8(
    const std::string& path, AssetFileSystemRef assets = std::nullopt);

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_PNG_IMAGE_H_
