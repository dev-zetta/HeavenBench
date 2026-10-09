/*
 * HeavenBench — In-UI configuration controller implementation (GPU-free)
 *
 * Pure logic: applies a frame's OverlayIntents over the Working/Active configs
 * in the fixed order edits → save → load → reload, routing each setting by its
 * catalog Apply classification and driving renderer effects through the
 * abstract RendererEffects interface. Validation/serialization are delegated to
 * the Settings Catalog and the Config_File module so this file owns no parallel
 * validation path and references no Filament symbol.
 */
#include "rhi/overlay/config_controller.h"

#include <algorithm>
#include <filesystem>
#include <utility>
#include <variant>

namespace heaven {

namespace {

// Extract a bool from a committed/stored SettingValue for the set_feature_enabled
// call. Live feature toggles are bool settings, so the variant normally carries
// a bool; the int fallback keeps this total if a caller surfaces 0/1 instead.
[[nodiscard]] bool as_bool(const SettingValue& v) {
    if (const bool* b = std::get_if<bool>(&v)) return *b;
    if (const int* i = std::get_if<int>(&v)) return *i != 0;
    return false;
}

// Dispatch a single Live setting to the renderer: a feature toggle goes through
// set_feature_enabled(name, on); anything else (time-of-day, weather, exposure,
// music volume, per-frame quality knobs) goes through apply_live, which reads
// the value from the just-updated Active_Configuration. `stored` is the value
// the catalog actually wrote (post clamp/reject) so a feature toggle reflects
// the stored bool, not the raw request.
void dispatch_live(SettingId id, const SettingValue& stored,
                   const BenchConfig& active, RendererEffects& fx) {
    if (const char* feature = live_feature_name(id); feature != nullptr) {
        fx.set_feature_enabled(feature, as_bool(stored));
    } else {
        fx.apply_live(id, active);
    }
}

} // namespace

bool is_dirty(const BenchConfig& working, const BenchConfig& active) {
    // Working and Active diverge only on Reload_Settings: Live edits are written
    // to both configs the moment they are applied. Compare every Apply::Reload
    // entry's governed field via SettingValue's operator==.
    return std::ranges::any_of(settings_catalog(), [&](const CatalogEntry& entry) {
        return entry.apply == Apply::Reload &&
               entry.read(working) != entry.read(active);
    });
}

const char* live_feature_name(SettingId id) {
    // Live feature toggles that map 1:1 to a registered IRenderFeature::name().
    // Everything else (including the live shadow/reflection quality settings,
    // which are consumed per-frame by lighting_->setup / configure_post_processing
    // rather than a feature) has no runtime feature and returns nullptr.
    switch (id) {
        case SettingId::FeatureRain:             return "rain";
        case SettingId::FeatureWetReflections:   return "wet-reflections";
        case SettingId::FeatureSky:              return "sky";
        case SettingId::FeatureGrass:            return "grass";
        case SettingId::FeatureParticles:        return "particles";
        case SettingId::FeatureGodRays:          return "god_rays";
        case SettingId::FeatureVolumetricFog:    return "volumetric_fog";
        case SettingId::FeatureClouds:           return "clouds";
        case SettingId::FeatureLensFlares:       return "lens_flares";
        case SettingId::FeatureVolumetricClouds: return "volumetric-clouds";
        case SettingId::FeatureSsgi:             return "ssgi";
        case SettingId::FeatureMassiveFoliage:   return "massive-foliage";
        default:                                 return nullptr;
    }
}

ConfigOpLog apply_config_intents(BenchConfig&          working,
                                 BenchConfig&          active,
                                 ConfigUiState&        ui,
                                 const OverlayIntents&  intents,
                                 RendererEffects&       fx) {
    ConfigOpLog log;

    // A successful load requests a Benchmark_Reload for its Reload_Settings even
    // when the UI did not raise the reload intent.
    bool need_reload = false;
    bool run_cancelled = false;
    const auto cancel_before_change = [&] {
        if (!run_cancelled) {
            fx.cancel_active_run();
            run_cancelled = true;
        }
    };

    // ── EDITS ───────────────────────────────────────────────────
    // Every edit is applied before any save/load/reload. Each
    // edit always writes Working_Configuration; a Live edit additionally writes
    // Active_Configuration and dispatches the renderer effect, while a Reload
    // edit leaves Active untouched so Dirty_State diverges

    for (const ConfigurationEdit& edit : intents.edits) {
        log.push_back(ConfigOp::Edit);

        const CatalogEntry& entry = catalog_entry(edit.id);
        const SettingValue stored = entry.apply_to(working, edit.value);

        if (entry.apply == Apply::Live) {
            // Settings remain accessible during playback. Retire the current
            // score before changing its effective workload; rejected/no-op
            // edits and pending reload settings do not interrupt the run.
            if (entry.read(active) != stored) cancel_before_change();
            // Write the same value into Active so the two configs stay in lockstep
            // for this Live_Setting, then drive the renderer.
            entry.apply_to(active, edit.value);
            dispatch_live(edit.id, stored, active, fx);
        }
        // Apply::Reload: Active is intentionally left unchanged until a reload.
    }

    if (!intents.edits.empty()) {
        ui.dirty = is_dirty(working, active);
    }

    // ── SAVE ────────────────────────────────────────────────────
    // Serialize the Working_Configuration through the Config_File module. Each
    // operation first clears the prior diagnostics/success so messages never mix
    // across operations.
    if (intents.save) {
        log.push_back(ConfigOp::Save);
        ui.diagnostics.clear();
        ui.success = false;

        if (intents.save_path.empty()) {
            // Empty path → no I/O, surface the required-path diagnostic

            ui.diagnostics.emplace_back("a target path is required");
        } else {
            std::vector<std::string> diags = write_config_file(intents.save_path, working);
            if (diags.empty()) {
                // Zero diagnostics → success identifying the saved path

                ui.success        = true;
                ui.success_detail = intents.save_path;
            } else {
                // One or more diagnostics → surface them, no success

                ui.diagnostics = std::move(diags);
                ui.success     = false;
            }
        }
    }

    // ── LOAD ────────────────────────────────────────────────────
    // Read the source path onto the current Active_Configuration as the base
    // On success copy into Working, apply every Live_Setting
    // to the renderer, and request a reload for the Reload_Settings; on failure
    // leave both configs untouched and surface every diagnostic plus the
    // "configuration in effect is unchanged" note.
    if (intents.load) {
        log.push_back(ConfigOp::Load);
        ui.diagnostics.clear();
        ui.success = false;

        ConfigLoadResult r = load_config_file(intents.load_path, active);
        if (r.parsed_ok) {
            cancel_before_change();
            working = r.config;
            ui.current_config_name = intents.load_path.empty()
                ? std::string("config.json")
                : intents.load_path;

            // Apply all Live_Settings from the freshly loaded Working config:
            // write each into Active and dispatch its renderer effect

            for (const CatalogEntry& entry : settings_catalog()) {
                if (entry.apply != Apply::Live) continue;
                const SettingValue v = entry.read(working);
                entry.apply_to(active, v);
                dispatch_live(entry.id, v, active, fx);
            }

            // Reload_Settings take effect through a Benchmark_Reload

            need_reload = true;

            // Surface any non-fatal diagnostics (clamp/unknown-key notes) the
            // loader produced for a well-formed file.
            if (!r.diagnostics.empty()) {
                ui.diagnostics = std::move(r.diagnostics);
            }
        } else {
            // Parse failure / unopenable / non-object: Working and Active are
            // left unchanged; surface every diagnostic and the unchanged note

            ui.diagnostics = std::move(r.diagnostics);
            ui.diagnostics.emplace_back("configuration in effect is unchanged");
            ui.success = false;
        }

        ui.dirty = is_dirty(working, active);
    }

    // ── RELOAD ──────────────────────────────────────────────────
    // Run the Benchmark_Reload transactionally when the UI requested it or a
    // successful load demands one. Cancel any active run first, then rebuild;
    // advance Active to Working only on success.
    if (intents.reload || need_reload) {
        log.push_back(ConfigOp::Reload);

        cancel_before_change();
        const Result<void> rr = fx.rebuild();
        if (rr.has_value()) {
            // Success: advance Active to Working and clear dirty/error.
            active           = working;
            ui.dirty         = false;
            ui.reload_error  = false;
        } else {
            // Failure: leave Active unchanged, keep dirty, raise the error flag.
            ui.dirty         = is_dirty(working, active);
            ui.reload_error  = true;
        }
    }

    return log;
}

} // namespace heaven
