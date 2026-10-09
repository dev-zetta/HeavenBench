// Actual loading UI pixels: fill amount and changing stage text, with no
// procedural scene or authored splash artwork obscuring the measurements.
#include "smoke_cases.h"
#include "rhi/filament_backend.h"
#include "rhi/loading_screen.h"
#include "rhi/screenshot_ppm.h"

#include <filament/Engine.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <backend/PixelBufferDescriptor.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_loading_progress]";
constexpr std::uint32_t kWidth = 640;
constexpr std::uint32_t kHeight = 360;
using Pixels = std::vector<std::uint8_t>;

class ContextSentinel {
public:
    ContextSentinel() : previous_(ImGui::GetCurrentContext()), context_(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(context_);
    }
    ContextSentinel(const ContextSentinel&) = delete;
    ContextSentinel& operator=(const ContextSentinel&) = delete;
    ~ContextSentinel() {
        ImGui::DestroyContext(context_);
        ImGui::SetCurrentContext(previous_);
    }
    [[nodiscard]] bool preserved() const { return ImGui::GetCurrentContext() == context_; }
private:
    ImGuiContext* previous_;
    ImGuiContext* context_;
};

class LoadingFixture {
public:
    explicit LoadingFixture(filament::Engine& engine) : engine_(engine) {}
    LoadingFixture(const LoadingFixture&) = delete;
    LoadingFixture& operator=(const LoadingFixture&) = delete;
    ~LoadingFixture() {
        if (loading_) loading_->set_output_target(nullptr);
        loading_.reset();
        engine_.destroy(target_);
        engine_.destroy(color_);
        engine_.destroy(renderer_);
        engine_.destroy(swapchain_);
    }

    bool init() {
        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kWidth, kHeight);
        if (!renderer_ || !swapchain_) return false;
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        // Filament permits combining these documented texture usage flags.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kWidth).height(kHeight).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).usage(usage).build(engine_);
        if (!color_) return false;
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_).build(engine_);
        if (!target_) return false;
        auto assets = AssetFileSystem::create_memory("loading-progress-fixture");
        if (!assets) return false;
        assets_ = std::move(*assets);
        loading_ = std::make_unique<LoadingScreen>(engine_);
        loading_->set_output_target(target_);
        return true;
    }

    bool capture(std::string_view status, float progress, Pixels& pixels,
                 const ContextSentinel& context, int frames = 4) {
        // Use one draw for the first-presentation regression; later samples
        // settle material/font changes with additional real draws.
        // Every call ends its frame and detaches both borrowed view targets.
        for (int frame = 0; frame < frames; ++frame) {
            loading_->show(*swapchain_, assets_.data_root(), status, kWidth, kHeight,
                          progress, std::cref(assets_));
            engine_.flushAndWait();
            if (!context.preserved()) return false;
            if (!renderer_->getFrameInfoHistory().empty()) {
                std::printf("%s FAIL: loading advanced the caller renderer's frame history\n", kTag);
                return false;
            }
        }
        pixels.assign(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
        std::atomic_bool complete{false};
        // The explicit RenderTarget overload supports reading completed targets
        // outside a frame (unlike the default swap-chain readback overload).
        renderer_->readPixels(target_, 0, 0, kWidth, kHeight,
            filament::backend::PixelBufferDescriptor(pixels.data(), pixels.size(),
                filament::backend::PixelDataFormat::RGBA,
                filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &complete));
        engine_.flushAndWait();
        return complete.load() && context.preserved();
    }

private:
    filament::Engine& engine_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    AssetFileSystem assets_;
    std::unique_ptr<LoadingScreen> loading_;
};

struct Bounds {
    std::uint32_t min_x = kWidth, min_y = kHeight, max_x = 0, max_y = 0;
    std::size_t count = 0;
    void add(std::uint32_t x, std::uint32_t y) {
        min_x = std::min(min_x, x); min_y = std::min(min_y, y);
        max_x = std::max(max_x, x); max_y = std::max(max_y, y); ++count;
    }
    [[nodiscard]] bool contains(std::uint32_t x, std::uint32_t y) const {
        return count && x >= min_x && x <= max_x && y >= min_y && y <= max_y;
    }
};

bool blue_fill(const std::uint8_t* pixel) {
    return pixel[2] > 140 && pixel[2] > static_cast<int>(pixel[0]) + 60 &&
           pixel[1] > static_cast<int>(pixel[0]) + 35;
}

Bounds fill_bounds(const Pixels& pixels, const Bounds* region = nullptr) {
    Bounds found;
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            if (region && !region->contains(x, y)) continue;
            const auto offset = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            if (blue_fill(pixels.data() + offset)) found.add(x, y);
        }
    return found;
}

bool save_frame(const char* name, const Pixels& pixels) {
    const char* output = std::getenv("HEAVEN_SMOKE_LOADING_OUTPUT_DIR");
    if (!output || !*output) return true;
    const std::filesystem::path directory(output);
    std::filesystem::create_directories(directory);
    return write_screenshot_ppm(directory / (std::string(name) + ".ppm"),
                                kWidth, kHeight, pixels).has_value();
}

bool check_first_presentation(const Pixels& pixels) {
    const Bounds bar = fill_bounds(pixels);
    std::size_t glyph_pixels = 0;
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            if (bar.contains(x, y)) continue;
            const auto offset = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            const auto* color = pixels.data() + offset;
            glyph_pixels += std::min({color[0], color[1], color[2]}) > 150;
        }
    // The neutral artwork cannot supply blue fill or white glyphs. Check the
    // actual first presentation before any warm-up draws can hide a missing UI.
    if (!bar.count || bar.max_x - bar.min_x < kWidth / 4 ||
            bar.max_y - bar.min_y > kHeight / 6 || glyph_pixels < 100) {
        std::printf("%s FAIL: first presentation has no visible progress/text "
                    "(%zu fill pixels, %zu glyph pixels)\n", kTag, bar.count, glyph_pixels);
        return false;
    }
    std::printf("%s first presentation: %zu fill pixels, %zu glyph pixels\n",
                kTag, bar.count, glyph_pixels);
    return true;
}

bool check_pixels(const Pixels& zero, const Pixels& half, const Pixels& full,
                  const Pixels& changed_status) {
    // Discover the bar from the rendered blue fill rather than repeating the
    // UI's layout coordinates, widget width or percentage-to-rectangle formula.
    const Bounds bar = fill_bounds(full);
    if (!bar.count || bar.max_x - bar.min_x < kWidth / 2 ||
        bar.max_y - bar.min_y > kHeight / 6) {
        std::printf("%s FAIL: full progress has no visible, wide bar (%zu fill pixels)\n",
                    kTag, bar.count);
        return false;
    }
    const auto empty = fill_bounds(zero, &bar);
    const auto halfway = fill_bounds(half, &bar);
    const double ratio = static_cast<double>(halfway.count) / static_cast<double>(bar.count);
    if (empty.count > bar.count / 100 || ratio < 0.35 || ratio > 0.65) {
        std::printf("%s FAIL: visible fill does not follow 0/50/100%%: %zu/%zu/%zu pixels\n",
                    kTag, empty.count, halfway.count, bar.count);
        return false;
    }

    Bounds text;
    std::size_t old_glyphs = 0, new_glyphs = 0;
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            if (bar.contains(x, y)) continue;
            const auto offset = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            const auto* before = full.data() + offset;
            const auto* after = changed_status.data() + offset;
            const int delta = std::max({std::abs(static_cast<int>(before[0]) - after[0]),
                std::abs(static_cast<int>(before[1]) - after[1]),
                std::abs(static_cast<int>(before[2]) - after[2])});
            if (delta <= 12) continue;
            const bool old_text = std::min({before[0], before[1], before[2]}) > 100;
            const bool new_text = std::min({after[0], after[1], after[2]}) > 100;
            if (!old_text && !new_text) continue;
            text.add(x, y);
            old_glyphs += old_text;
            new_glyphs += new_text;
        }
    if (text.count < 40 || old_glyphs < 20 || new_glyphs < 20 ||
        text.max_x - text.min_x < 60 || text.max_y - text.min_y > kHeight / 8) {
        std::printf("%s FAIL: stage change has no isolated visible glyph change "
                    "(%zu pixels, old/new %zu/%zu)\n", kTag, text.count, old_glyphs, new_glyphs);
        return false;
    }
    std::printf("%s bar fill 0/50/100%%=%zu/%zu/%zu pixels; status changed %zu glyph pixels\n",
                kTag, empty.count, halfway.count, bar.count, text.count);
    return true;
}
} // namespace

CaseResult run_loading_progress_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (const auto ready = backend.init_pipelines(ctx.config); !ready) return CaseResult::Skip;
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return CaseResult::Fail;
    const auto owner_counts = [&] {
        return std::tuple{engine->getVertexBufferCount(), engine->getIndexBufferCount(),
                          engine->getViewCount(), engine->getTextureCount()};
    };
    const auto baseline = owner_counts();
    ContextSentinel context;
    bool ok = true;
    try {
        {
            LoadingFixture fixture(*engine);
            Pixels first, zero, half, full, changed_status, unknown;
            if (!fixture.init()) return CaseResult::Fail;
            ok = fixture.capture("Reading scene files", 0.5f, first, context, 1) &&
                 save_frame("loading-first", first) && check_first_presentation(first) &&
                 fixture.capture("Reading scene files", 0.0f, zero, context) &&
                 fixture.capture("Reading scene files", 0.5f, half, context) &&
                 fixture.capture("Reading scene files", 1.0f, full, context) &&
                 fixture.capture("Uploading refined meshes", 1.0f, changed_status, context) &&
                 fixture.capture("Preparing geometry", -1.0f, unknown, context);
            if (ok) {
                ok = save_frame("loading-000", zero) && save_frame("loading-050", half) &&
                     save_frame("loading-100", full) && save_frame("loading-status", changed_status) &&
                     save_frame("loading-unknown", unknown) && check_pixels(zero, half, full, changed_status);
            }
        }
        engine->flushAndWait();
        ok &= context.preserved() && owner_counts() == baseline;
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        ok = false;
    }
    if (!ok) return CaseResult::Fail;
    std::printf("%s PASS: real bar/text pixels, private-context restoration and "
                "capture-target/resource teardown\n", kTag);
    return CaseResult::Pass;
}
} // namespace heaven::smoke
