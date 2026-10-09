/*
 * HeavenBench — Texture store
 *
 * Owns DDS texture loading + the path/handle cache and the two shared
 * 1×1 placeholder textures (white + flat normal) the material instance
 * cache falls back to for missing maps. This is core infrastructure that
 * the backend exposes to render features through IResourceProvider.
 *
 * Extracted verbatim out of FilamentBackend::load_texture /
 * get_filament_texture and the placeholder-creation block in
 * init_pipelines().
 */
#ifndef HEAVENBENCH_SRC_RHI_TEXTURE_STORE_H_
#define HEAVENBENCH_SRC_RHI_TEXTURE_STORE_H_

#include "engine/asset_file_system.h"
#include "rhi/rhi.h"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace filament {
class Engine;
class Texture;
}

namespace heaven {

class TextureStore {
public:
    struct Stats {
        uint64_t srgb = 0;
        uint64_t scalar = 0;
        uint64_t normal = 0;
        uint64_t hdr = 0;
        uint64_t fallbacks = 0;
        uint64_t hires = 0;
        uint64_t originals = 0;
        uint64_t missing_variants = 0;
        uint64_t unsupported_formats = 0;
        uint64_t invalid_variants = 0;
        uint64_t selected_mip_bytes = 0;
        uint32_t effective_max_dimension = 0;
        TextureQuality requested_quality = TextureQuality::Original;
        TextureQuality active_quality = TextureQuality::Original;
    };
    explicit TextureStore(filament::Engine& engine,
                          AssetFileSystemRef assets = std::nullopt)
        : engine_(engine), assets_(assets) {}
    ~TextureStore();
    void set_asset_file_system(AssetFileSystemRef assets) noexcept {
        assets_ = assets;
        refresh_active_quality();
    }
    void set_texture_quality(TextureQuality quality) noexcept;
    void clear_loaded_textures();
    void record_external_original_texture(
        uint32_t width, uint32_t height, TextureSemantic semantic,
        bool complete_mip_chain) noexcept;

    // Load a DDS texture from disk (cached by path). Returns an opaque
    // handle (0 = failure).
    [[nodiscard]] uint64_t load(
        const std::string& path,
        TextureSemantic semantic = TextureSemantic::ScalarLinear);

    // Original lens-dirt DDS, converted once into a separately cached linear
    // RGBA16F gain of 1 + .2 * dirt. This post-process input deliberately uses
    // the authored source rather than the high-resolution surface-map tier.
    // Filament's dirtStrength must be one when binding this gain texture.
    [[nodiscard]] uint64_t load_bloom_dirt_gain(const std::string& path);

    // Resolve a handle from load() to a Filament texture (nullptr on
    // handle 0 or unknown handle).
    [[nodiscard]] filament::Texture* get(uint64_t handle) const;

    // Build the shared 1×1 white + flat-normal + zero-depth placeholders.
    // Call once after construction.
    void create_placeholders();
    [[nodiscard]] filament::Texture* placeholder()        const { return placeholder_tex_; }
    [[nodiscard]] filament::Texture* placeholder_normal() const { return placeholder_normal_tex_; }
    // 1×1 placeholder whose red channel reads 0.0, used as the depth-sampler
    // fallback by scene-depth consumers (god rays, volfog, clouds, SSGI, heat
    // haze, splash, lens flares). Their shaders treat a non-positive depth as
    // "far / no geometry", so binding THIS placeholder (rather than the white
    // one, which reads 1.0 m and would falsely occlude) makes the consumers
    // fall back to no occlusion when the real Scene_Depth_Texture is absent

    [[nodiscard]] filament::Texture* placeholder_depth()  const { return placeholder_depth_tex_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    filament::Engine& engine_;
    AssetFileSystemRef assets_;
    uint64_t next_tex_id_ = 1;
    std::unordered_map<uint64_t, filament::Texture*> texture_map_;
    std::unordered_map<std::string, uint64_t>        texture_cache_;
    filament::Texture* placeholder_tex_        = nullptr;
    filament::Texture* placeholder_normal_tex_ = nullptr;
    filament::Texture* placeholder_depth_tex_  = nullptr;
    Stats stats_{};
    bool warned_hires_fallback_ = false;
    bool warned_cpu_decode_ = false;

    void refresh_active_quality() noexcept;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_TEXTURE_STORE_H_
