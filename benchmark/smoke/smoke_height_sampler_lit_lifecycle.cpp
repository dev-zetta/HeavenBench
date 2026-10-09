/* Height sampling must not invalidate later textured material uniform data. */
#include "smoke_height_sampler_lit_lifecycle.h"

#include "engine/camera.h"
#include "engine/render_test_scene.h"
#include "engine/render_test_scene_catalog.h"
#include "rhi/filament_backend.h"
#include "rhi/height_sample_batch.h"
#include "rhi/materials_data.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/Texture.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_height_sampler_lit_lifecycle]";

struct Pixels {
    int width = 0, height = 0;
    std::vector<unsigned char> rgb;
};

// These are broad texture-content gates rather than a golden image. The
// procedural checker has sRGB values 215/42, and the marker is 205/42/35.
struct Appearance {
    std::size_t marker_samples = 0, red_samples = 0;
    std::size_t bright_samples = 0, dark_samples = 0;
    double bright_mean = 0, dark_mean = 0;
    [[nodiscard]] double checker_contrast() const { return bright_mean - dark_mean; }
    [[nodiscard]] bool visible() const {
        return marker_samples >= 2 && red_samples >= 2 &&
            bright_samples >= 6 && dark_samples >= 6 && checker_contrast() >= 12;
    }
};

class RestoreBackend {
public:
    RestoreBackend(FilamentBackend& backend, const AppConfig& config,
                   PathResolverRef paths, AssetFileSystemRef assets)
        : backend_(backend), config_(config), paths_(paths), assets_(assets) {}
    ~RestoreBackend() { if (!attempted_) static_cast<void>(restore()); }
    bool restore() noexcept {
        attempted_ = true;
        try {
            backend_.set_capture_time(std::nullopt);
            backend_.set_asset_context(paths_, assets_);
            const auto restored = backend_.reconfigure_for_reload(config_);
            if (!restored) {
                std::printf("%s restore failed: %s\n", kTag, restored.error().message.c_str());
                return false;
            }
            backend_.debug_render_context().engine->flushAndWait();
            return true;
        } catch (...) {
            std::printf("%s original backend restoration threw\n", kTag);
            return false;
        }
    }
private:
    FilamentBackend& backend_;
    const AppConfig& config_;
    PathResolverRef paths_;
    AssetFileSystemRef assets_;
    bool attempted_ = false;
};

class TemporaryCapture {
public:
    TemporaryCapture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("heaven-height-lit-" + std::to_string(stamp) + ".ppm");
    }
    ~TemporaryCapture() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    std::filesystem::path path;
};

[[nodiscard]] Pixels read_pixels(const std::filesystem::path& path, int width, int height) {
    std::ifstream stream(path, std::ios::binary);
    std::string magic;
    Pixels pixels;
    int maximum = 0;
    stream >> magic >> pixels.width >> pixels.height >> maximum;
    char delimiter = 0;
    stream.get(delimiter);
    if (!stream || magic != "P6" || pixels.width != width || pixels.height != height ||
        maximum != 255 || delimiter != '\n')
        throw std::runtime_error("Final screenshot has invalid PPM dimensions or header");
    pixels.rgb.resize(std::size_t(width) * height * 3);
    stream.read(reinterpret_cast<char*>(pixels.rgb.data()),
        static_cast<std::streamsize>(pixels.rgb.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("Final screenshot payload is incomplete or oversized");
    return pixels;
}

[[nodiscard]] bool intersects_marker(glm::dvec3 origin, glm::dvec3 direction) {
    // The actual height_blocker is an axis-aligned red box in the fixture.
    const glm::dvec3 minimum{-.12, 3.1, 0}, maximum{.12, 3.9, .9};
    double near = 0, far = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-14) {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis]) return false;
            continue;
        }
        double a = (minimum[axis] - origin[axis]) / direction[axis];
        double b = (maximum[axis] - origin[axis]) / direction[axis];
        if (a > b) std::swap(a, b);
        near = std::max(near, a); far = std::min(far, b);
    }
    return far >= near && far > 0;
}

[[nodiscard]] Appearance appearance(const Pixels& image, const Camera& camera) {
    Appearance result;
    const glm::dvec3 origin = camera.position;
    const glm::dvec3 front = camera.front();
    // glm::lookAt uses front cross world-up for positive screen X.
    const auto right = glm::normalize(glm::cross(front, glm::dvec3{0, 0, 1}));
    const auto up = glm::cross(right, front);
    const double tangent = std::tan(glm::radians(double(camera.fov)) * .5);
    const double aspect = double(image.width) / image.height;
    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
        const auto direction = front + right * ((2 * (x + .5) / image.width - 1) * tangent * aspect) +
            up * ((1 - 2 * (y + .5) / image.height) * tangent);
        const auto index = (std::size_t(y) * image.width + x) * 3;
        const double r = image.rgb[index], g = image.rgb[index + 1], b = image.rgb[index + 2];
        if (intersects_marker(origin, direction)) {
            ++result.marker_samples;
            result.red_samples += r >= 40 && r >= 1.5 * g && r >= 1.5 * b &&
                r - g >= 12 && r - b >= 12;
        }
        if (direction.z >= -1e-12) continue;
        const double t = -origin.z / direction.z;
        if (t <= 0) continue;
        const auto p = origin + t * direction;
        // The two outer ground strips are clear of all four raised objects.
        if (!((p.x >= -5.8 && p.x <= -3.5) || (p.x >= 3.5 && p.x <= 5.8)) ||
            p.y < -1.8 || p.y > 7.8) continue;
        const double u = (p.x + 6) / 12, v = (p.y + 2) / 10;
        const double cell_x = u * 8, cell_y = v * 8;
        const double local_x = cell_x - std::floor(cell_x), local_y = cell_y - std::floor(cell_y);
        // Exclude texture-cell edges, where ordinary filtering blends classes.
        if (local_x < .2 || local_x > .8 || local_y < .2 || local_y > .8) continue;
        const double luminance = .2126 * r + .7152 * g + .0722 * b;
        if ((int(cell_x) ^ int(cell_y)) & 1) {
            ++result.bright_samples; result.bright_mean += luminance;
        } else {
            ++result.dark_samples; result.dark_mean += luminance;
        }
    }
    if (result.bright_samples) result.bright_mean /= static_cast<double>(result.bright_samples);
    if (result.dark_samples) result.dark_mean /= static_cast<double>(result.dark_samples);
    return result;
}

[[nodiscard]] Pixels capture(FilamentBackend& backend, const RenderTestScene& scene,
                             const AppConfig& config, const Camera& camera,
                             int width, int height) {
    auto* engine = backend.debug_render_context().engine;
    const auto lighting = lighting_for_time_of_day(config.time_of_day);
    const WeatherState weather(config.weather);
    const TonemapParams tonemap;
    const MaterialResolver resolve = [&](const std::string& mesh, const std::string& surface) {
        return scene.resolve_material(mesh, surface);
    };
    const auto draw = [&] {
        const auto frame = backend.render_frame(scene.scene.instances, {}, scene.scene.gpu_meshes,
            resolve, camera, lighting, weather, tonemap, width, height);
        if (!frame.presented) engine->flushAndWait();
        return frame.presented;
    };
    unsigned presented = 0;
    for (unsigned attempt = 0; attempt < 32 && presented < 4; ++attempt) presented += draw();
    if (presented != 4) throw std::runtime_error("Four warm-up frames were not presented");
    TemporaryCapture file;
    if (const auto requested = backend.request_screenshot(file.path); !requested)
        throw std::runtime_error(requested.error().message);
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        static_cast<void>(draw());
        engine->flushAndWait(); engine->pumpMessageQueues();
        const auto completed = backend.screenshot_complete();
        if (!completed) throw std::runtime_error(completed.error().message);
        if (*completed) return read_pixels(file.path, width, height);
    }
    throw std::runtime_error("Final main-view screenshot did not complete");
}

void print_appearance(const char* phase, const Appearance& value) {
    std::printf("%s %s: red marker %zu/%zu pixels, checker bright/dark %zu/%zu "
        "means %.3f/%.3f, contrast %.3f\n", kTag, phase, value.red_samples,
        value.marker_samples, value.bright_samples, value.dark_samples,
        value.bright_mean, value.dark_mean, value.checker_contrast());
}
} // namespace

CaseResult run_height_sampler_lit_lifecycle_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament || ctx.config.hdr) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (const auto initialized = backend.init_pipelines(ctx.config); !initialized) {
        std::printf("%s initialization failed: %s\n", kTag, initialized.error().message.c_str());
        return CaseResult::Fail;
    }
    const auto original_context = backend.debug_render_context();
    if (!original_context.engine || ctx.width < 64 || ctx.height < 64) {
        std::printf("%s requires an initialized backend and drawable >=64x64\n", kTag);
        return CaseResult::Fail;
    }
    std::optional<AssetFileSystem> assets;
    std::optional<PathResolver> paths;
    std::optional<RenderTestScene> scene;
    RestoreBackend restore(backend, ctx.config, original_context.paths, original_context.assets);
    bool ok = false;
    try {
        AppConfig config = ctx.config;
        if (!apply_render_test_scene_defaults(config, "displacement"))
            throw std::runtime_error("Displacement scene defaults are missing");
        // Same deterministic textures, geometry and ordinary lit routing as
        // the application fixture. Time-varying AA is unnecessary for this
        // resource-lifecycle comparison; original geometry tests cover AA.
        config.features.taa = false; config.features.msaa = false;
        config.quality.msaa_samples = 1; config.time_of_day = 12;
        auto definition = make_render_test_scene("displacement");
        if (!definition) throw std::runtime_error(definition.error().message);
        if (definition->instances.size() != 5 || definition->meshes.size() != 5)
            throw std::runtime_error("Displacement fixture no longer has its five concrete objects");
        auto generated = AssetFileSystem::create_memory("height-sampler-lit-lifecycle");
        auto generated_paths = PathResolver::create("height-sampler-lit-lifecycle");
        if (!generated || !generated_paths) throw std::runtime_error("Generated asset namespace creation failed");
        if (const auto mounted = mount_render_test_scene_assets(*definition, *generated); !mounted)
            throw std::runtime_error(mounted.error().message);
        if (const auto reloaded = backend.reconfigure_for_reload(config); !reloaded)
            throw std::runtime_error(reloaded.error().message);
        assets.emplace(std::move(*generated)); paths.emplace(std::move(*generated_paths));
        backend.set_asset_context(std::cref(*paths), std::cref(*assets));
        auto uploaded = upload_render_test_scene(std::move(*definition), backend, config);
        if (!uploaded) throw std::runtime_error(uploaded.error().message);
        scene.emplace(std::move(*uploaded));
        const MaterialResolver resolve = [&](const std::string& mesh, const std::string& surface) {
            return scene->resolve_material(mesh, surface);
        };
        // Application startup samples and destroys temporary View/material
        // resources before constructing the persistent visible renderables.
        if (const auto prepared = backend.prepare_static_displacement(scene->scene, resolve); !prepared)
            throw std::runtime_error(prepared.error().message);
        original_context.engine->flushAndWait();
        backend.set_particle_emitters({}); backend.set_scene_lights({});
        backend.set_capture_time(0.0f);
        Camera camera;
        camera.position = {4.5f, -6.0f, 3.5f}; camera.yaw = 119.3578f;
        camera.pitch = -17.8024f; camera.fov = 50;
        const auto before = capture(backend, *scene, config, camera, ctx.width, ctx.height);
        const auto first = appearance(before, camera);
        print_appearance("after startup preparation", first);
        if (!first.visible()) throw std::runtime_error("Startup sampling erased visible marker color or checker content");
        const auto material = scene->resolve_material({}, "render_test_coarse_height");
        auto* height = backend.get_filament_texture(material.height);
        if (!height || height->getWidth() != 64 || height->getLevels() != 7)
            throw std::runtime_error("Actual selected procedural height texture is missing");
        std::array<HeightSampleQuery, 4> queries{{
            {11, {.17f,.19f}, {.125f,.125f}}, {23, {.37f,.43f}, {.125f,.125f}},
            {41, {.61f,.67f}, {.125f,.125f}}, {59, {.83f,.89f}, {.125f,.125f}}}};
        for (unsigned repetition = 0; repetition < 2; ++repetition) {
            // The live scene and its textures stay unchanged. Destruction of
            // each actual sampler job and batch is the only intervening work.
            auto batch = HeightSampleBatch::create(*original_context.engine,
                {k_heaven_height_sample_filamat, k_heaven_height_sample_filamat_size});
            if (!batch) throw std::runtime_error(batch.error().message);
            auto values = (*batch)->sample(height, queries, {1,1,0,0});
            if (!values || values->size() != queries.size())
                throw std::runtime_error(values ? "Height lifecycle sample count is wrong" : values.error().message);
            for (std::size_t i = 0; i < queries.size(); ++i)
                if ((*values)[i].id != queries[i].id || !std::isfinite((*values)[i].height))
                    throw std::runtime_error("Height lifecycle query identity/value is invalid");
            batch->reset(); original_context.engine->flushAndWait();
            const auto after = capture(backend, *scene, config, camera, ctx.width, ctx.height);
            const auto content = appearance(after, camera);
            print_appearance("after sampler destruction", content);
            double difference = 0; unsigned maximum = 0;
            for (std::size_t i = 0; i < before.rgb.size(); ++i) {
                const auto delta = unsigned(std::abs(int(before.rgb[i]) - int(after.rgb[i])));
                difference += delta; maximum = std::max(maximum, delta);
            }
            difference /= static_cast<double>(before.rgb.size());
            std::printf("%s destruction %u: %zu actual native queries, final RGB mean/max change %.6f/%u\n",
                kTag, repetition + 1, values->size(), difference, maximum);
            if (!content.visible() || difference > 2 || maximum > 16)
                throw std::runtime_error("Sampler teardown changed persistent textured material appearance");
        }
        const auto stats = backend.renderer_feature_stats();
        if (stats.texture_fallbacks != 0 || stats.srgb_textures == 0 || stats.scalar_textures == 0 ||
            original_context.scene->getRenderableCount() == 0)
            throw std::runtime_error("Fixture lacks real typed texture inputs or visible renderables");
        ok = true;
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
    }
    ok = restore.restore() && ok;
    if (!ok) return CaseResult::Fail;
    std::printf("%s PASS: startup preparation and two same-Engine sampler teardowns retain "
        "actual checker and red marker; no geometry-lighting equivalence or golden-image claim\n", kTag);
    return CaseResult::Pass;
}
} // namespace heaven::smoke
