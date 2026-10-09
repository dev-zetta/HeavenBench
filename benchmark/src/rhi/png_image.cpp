/*
 * HeavenBench - PNG image loading through Filament's vendored stb_image
 */
#include "rhi/png_image.h"
#include "engine/asset_file_system.h"

#include <stb_image.h>

#include <limits>
#include <climits>
#include <memory>

namespace heaven {

Result<PngImage> load_png_rgba8(const std::string& path,
                                AssetFileSystemRef assets) {
    int width = 0;
    int height = 0;
    auto encoded = read_asset_file(path, assets);
    if (!encoded || encoded->size() > static_cast<std::size_t>(INT_MAX)) {
        return std::unexpected(
            encoded ? Error("PNG is too large: " + path) : encoded.error());
    }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(
            encoded->data(), static_cast<int>(encoded->size()),
            &width, &height, nullptr, STBI_rgb_alpha),
        &stbi_image_free);
    if (!pixels) {
        const char* reason = stbi_failure_reason();
        return std::unexpected(Error(
            "could not decode PNG '" + path + "': " +
            (reason ? reason : "unknown stb_image error")));
    }

    constexpr size_t kChannels = 4;
    const size_t decoded_width = static_cast<size_t>(width);
    const size_t decoded_height = static_cast<size_t>(height);
    if (width <= 0 || height <= 0 ||
        decoded_height > std::numeric_limits<size_t>::max() / kChannels ||
        decoded_width > std::numeric_limits<size_t>::max() /
            (decoded_height * kChannels)) {
        return std::unexpected(Error("decoded PNG is too large: " + path));
    }

    PngImage image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    const size_t byte_count = decoded_width * decoded_height * kChannels;
    image.rgba.assign(pixels.get(), pixels.get() + byte_count);
    return image;
}

} // namespace heaven
