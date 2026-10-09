/*
 * HeavenBench — Procedural sky IBL probe implementation
 *
 * Moved verbatim out of FilamentBackend::build_sky_cubemap. Pure CPU
 * texel generation plus the Filament cubemap upload; no backend state.
 */
#include "rhi/sky_ibl.h"
#include "rhi/sky_atmosphere.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Texture.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace heaven {

filament::Texture* build_sky_cubemap(
    filament::Engine& engine, const LightingParams& lighting,
    bool include_clouds)
{
    constexpr uint32_t FACE = render_tuning::ibl.face_size;
    constexpr uint32_t FACE_SZ = FACE * FACE * 4;
    constexpr uint32_t CUBE_SZ = FACE_SZ * 6;

    // Standard cubemap face-axis convention (Filament / D3D / GL all
    // agree): the face whose normal is "+X" is sampled when the
    // world-space ray is along +X. Within each face, we walk a 2D
    // grid (u, v) ∈ [-1,1]² and reconstruct the world-space direction
    // for that texel. The mapping below is the canonical one
    // (matching `KhronosGroup/glTF-Sample-Viewer`):
    //   +X: (+1, -v, -u)   -X: (-1, -v, +u)
    //   +Y: (+u, +1, +v)   -Y: (+u, -1, -v)
    //   +Z: (+u, -v, +1)   -Z: (-u, -v, -1)
    auto face_dir = [](int face, float u, float v) {
        switch (face) {
            case 0: return glm::vec3( 1.0f,    -v,    -u);  // +X
            case 1: return glm::vec3(-1.0f,    -v,    +u);  // -X
            case 2: return glm::vec3(   +u,  1.0f,    +v);  // +Y
            case 3: return glm::vec3(   +u, -1.0f,    -v);  // -Y
            case 4: return glm::vec3(   +u,    -v,  1.0f);  // +Z
            case 5: return glm::vec3(   -u,    -v, -1.0f);  // -Z
        }
        return glm::vec3(0.0f);
    };

    const glm::vec3 sun_c = lighting.sun_color;
    const glm::vec3 sun_dir = glm::normalize(lighting.sun_dir);
    const SkyAtmosphereParams sky = make_sky_atmosphere_params(lighting);

    // ── Cheap value-noise for cloud cover ────────────────────────
    // We can't pull GLSL's hash here, so this is a rough 32-bit
    // integer hash mimicking GPU `fract(sin(...) *...)` tricks but
    // deterministic. Smoothing is bilinear over a 64-cell grid mapped
    // around a sphere via great-circle distance from a fixed pole.
    auto hash01 = [](uint32_t x) -> float {
        x = (x ^ 61u) ^ (x >> 16);
        x *= 9u;
        x ^= x >> 4;
        x *= 0x27d4eb2du;
        x ^= x >> 15;
        return static_cast<float>(x) / static_cast<float>(0xFFFFFFFFu);
    };
    auto hash2 = [&](int gx, int gy) {
        return hash01(static_cast<uint32_t>(gx) * 374761393u
                    + static_cast<uint32_t>(gy) * 668265263u);
    };
    auto smoothstep = [](float a, float b, float x) {
        x = std::clamp((x - a) / (b - a + 1e-6f), 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    };
    auto cloud_density = [&](glm::vec3 d) {
        // Project on a horizontal plane (cheap perspective-look
        // clouds — fine for 32×32 mips, where most of the cubemap
        // gets averaged anyway).
        const auto projection = render_tuning::ibl.cloud_projection;
        const auto shape = render_tuning::ibl.cloud_shape;
        if (d.z <= projection.x) return 0.0f;
        float stretch = 1.0f / std::max(d.z, projection.x);
        float fx = d.x * stretch * projection.z;
        float fy = d.y * stretch * projection.z;
        int gx = static_cast<int>(std::floor(fx));
        int gy = static_cast<int>(std::floor(fy));
        float ux = fx - static_cast<float>(gx);
        float uy = fy - static_cast<float>(gy);
        float n00 = hash2(gx,     gy);
        float n10 = hash2(gx + 1, gy);
        float n01 = hash2(gx,     gy + 1);
        float n11 = hash2(gx + 1, gy + 1);
        ux = ux * ux * (3.0f - 2.0f * ux);
        uy = uy * uy * (3.0f - 2.0f * uy);
        float n = (n00 * (1 - ux) + n10 * ux) * (1 - uy)
                + (n01 * (1 - ux) + n11 * ux) * uy;
        // Threshold and shape into wisps.
        n = smoothstep(shape.x, shape.y, n);
        // Fade to zero at the horizon to hide the planar projection.
        n *= smoothstep(projection.x, projection.y, d.z);
        return n;
    };

    // ── Generate face data ───────────────────────────────────────
    auto* buf = new float[CUBE_SZ];
    auto write_pixel = [&](size_t off, glm::vec3 c) {
        c = glm::max(c, glm::vec3(0.0f));
        buf[off + 0] = c.r;
        buf[off + 1] = c.g;
        buf[off + 2] = c.b;
        buf[off + 3] = 1.0f;
    };

    for (int face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < FACE; ++y) {
            float v = ((static_cast<float>(y) + 0.5f) / FACE) * 2.0f - 1.0f;
            for (uint32_t x = 0; x < FACE; ++x) {
                float u = ((static_cast<float>(x) + 0.5f) / FACE) * 2.0f - 1.0f;
                glm::vec3 d = glm::normalize(face_dir(face, u, v));

                glm::vec3 base = evaluate_sky_atmosphere(sky, d, sun_dir);

                // Keep a tiny broad sun glow in the low-resolution cubemap so
                // rough reflections still catch the sun direction after mip
                // filtering. The visible sky material renders the physical disk.
                float sd = std::max(glm::dot(d, sun_dir), 0.0f);
                const auto sun_glow = render_tuning::ibl.sun_glow;
                float glow = std::pow(sd, sun_glow.x) * sun_glow.y
                           + std::pow(sd, sun_glow.z) * sun_glow.w;
                base += sun_c * glow *
                    std::clamp(
                        lighting.sun_intensity /
                            render_tuning::lighting.reference_sun_lux,
                        0.0f, 1.0f);

                // Subtle cloud overlay above horizon. Gated on the old
                // cloud-quad feature toggle: this cubemap is the IBL
                // reflection probe, so baking clouds here paints them into
                // every reflective surface even when the cloud feature is
                // disabled. Skip the overlay entirely when clouds are off so
                // reflections match the cloudless sky.
                if (include_clouds) {
                    float density = cloud_density(d);
                    if (density > 0.0f) {
                        const auto cloud_tint = render_tuning::ibl.cloud_color;
                        const auto cloud_shape = render_tuning::ibl.cloud_shape;
                        glm::vec3 cloud(
                            cloud_tint.x, cloud_tint.y, cloud_tint.z);
                        cloud += sun_c * std::pow(sd, 2.0f) * cloud_shape.w;
                        base = glm::mix(
                            base, cloud, density * cloud_shape.z);
                    }
                }

                size_t pixel_off = face * FACE_SZ + (y * FACE + x) * 4;
                write_pixel(pixel_off, base);
            }
        }
    }

    auto* tex = filament::Texture::Builder()
        .width(FACE).height(FACE).levels(render_tuning::ibl.mip_levels)
        .sampler(filament::Texture::Sampler::SAMPLER_CUBEMAP)
        .format(filament::Texture::InternalFormat::RGBA16F)
        .usage(filament::Texture::Usage::SAMPLEABLE |
               filament::Texture::Usage::COLOR_ATTACHMENT |
               filament::Texture::Usage::UPLOADABLE |
               filament::Texture::Usage::GEN_MIPMAPPABLE)
        .build(engine);

    tex->setImage(engine, 0, 0, 0, 0, FACE, FACE, 6,
        filament::Texture::PixelBufferDescriptor(
            buf, CUBE_SZ * sizeof(float),
            filament::Texture::Format::RGBA,
            filament::Texture::Type::FLOAT,
            [](void* b, size_t, void*) { delete[] static_cast<float*>(b); }));
    tex->generateMipmaps(engine);

    return tex;
}

} // namespace heaven
