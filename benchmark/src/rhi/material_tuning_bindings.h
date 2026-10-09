/*
 * HeavenBench — typed bindings for C++-owned Filament shader tuning
 *
 * Every parameter introduced for centralized shader tuning is initialized by
 * one of these family binders immediately after MaterialInstance creation.
 * Dynamic and authored parameters are intentionally applied by the caller
 * afterwards, so they retain precedence over these compile-time defaults.
 */
#ifndef HEAVENBENCH_SRC_RHI_MATERIAL_TUNING_BINDINGS_H_
#define HEAVENBENCH_SRC_RHI_MATERIAL_TUNING_BINDINGS_H_

namespace filament {
class MaterialInstance;
}

namespace heaven::material_tuning {

void bind_standard_surface(filament::MaterialInstance& instance);
void bind_ground_relief(filament::MaterialInstance& instance);
void bind_window(filament::MaterialInstance& instance);
void bind_glass(filament::MaterialInstance& instance);
void bind_sky(filament::MaterialInstance& instance);
void bind_grass_visible(filament::MaterialInstance& instance);
void bind_grass_depth(filament::MaterialInstance& instance);
void bind_clouds(filament::MaterialInstance& instance);
void bind_volumetric_clouds(filament::MaterialInstance& instance);
void bind_godrays(filament::MaterialInstance& instance);
void bind_volumetric_fog(filament::MaterialInstance& instance);
void bind_ssgi(filament::MaterialInstance& instance);
void bind_smoke(filament::MaterialInstance& instance);
void bind_flame(filament::MaterialInstance& instance);
void bind_glow(filament::MaterialInstance& instance);
void bind_firefly(filament::MaterialInstance& instance);
void bind_heat_haze(filament::MaterialInstance& instance);
void bind_rain(filament::MaterialInstance& instance);
void bind_splash(filament::MaterialInstance& instance);
void bind_lens_flare(filament::MaterialInstance& instance);

} // namespace heaven::material_tuning

#endif // HEAVENBENCH_SRC_RHI_MATERIAL_TUNING_BINDINGS_H_
