/*
 * HeavenBench — HDR10 output encode (PQ / ST.2084)
 *
 * Final swap-chain stage for the --hdr path. The main view is tone-mapped by
 * GT7 and rendered (in LINEAR Rec.2020 display-referred values) into an fp16
 * scene target owned here. This subsystem then runs one fullscreen pass with
 * the heaven_hdr_encode material that applies the SMPTE ST.2084 (PQ) OETF and
 * writes the result to the HDR10 swap chain.
 *
 * It is the HDR analogue of SceneColorCapture's blit, minus the ping-pong: a
 * single capture RT plus a composite view/scene with one encode quad. The
 * backend, when HDR is active, points the main view (and any SSGI/heat-haze
 * composite that would otherwise target the swap chain) at scene_rt(), then
 * calls encode() last to produce the PQ swap-chain image.
 *
 * Lifetime: GPU resources are created lazily in ensure() and freed in the
 * destructor, which must run before the engine is torn down (the backend resets
 * the unique_ptr in its destructor, like the other subsystems).
 */
#ifndef HEAVENBENCH_SRC_RHI_HDR_OUTPUT_H_
#define HEAVENBENCH_SRC_RHI_HDR_OUTPUT_H_

#include "rhi/backend_core.h"

namespace filament {
class View;
class Scene;
class RenderTarget;
class Texture;
class Material;
class MaterialInstance;
class VertexBuffer;
class IndexBuffer;
class Skybox;
}

#include <utils/Entity.h>

namespace heaven {

class HdrOutput {
public:
    explicit HdrOutput(const BackendCore& core) : core_(core) {}
    ~HdrOutput();

    // Lazily build the encode pipeline (once) and (re)create the fp16 scene
    // target for the given full-resolution viewport. No-op when the size is
    // unchanged. Returns false if the encode material is unavailable (the
    // backend then cannot do HDR output and should report it).
    bool ensure(int w, int h);

    // True once the encode pipeline + target exist.
    [[nodiscard]] bool valid() const {
        return encode_mi_ && rt_ && sdr_layer_rt_ && sdr_background_;
    }
    [[nodiscard]] bool needs_resize(int w, int h) const {
        return w != w_ || h != h_;
    }

    // The render target the main view (and redirected composites) should render
    // into this frame — the linear Rec.2020 fp16 scene texture.
    [[nodiscard]] filament::RenderTarget* scene_rt() const { return rt_; }
    [[nodiscard]] filament::RenderTarget* sdr_layer_rt() const { return sdr_layer_rt_; }
    [[nodiscard]] filament::Skybox* sdr_layer_background() const { return sdr_background_; }

    // Call after ensure(), on a view with a scene. Replace the isolated layer
    // each frame; translucent view composition would retain old target pixels
    // under its transparent background. Individual UI materials still blend.
    // The caller detaches the target and restores its prior view mode afterward.
    void bind_sdr_layer(filament::View& view);

    // Set the luminance mapping. reference_nits is the absolute luminance a
    // display-referred value of 1.0 maps to (GT7 reference == 100 cd/m^2);
    // peak_nits is the display peak used to clamp highlights.
    void set_luminance(float reference_nits, float peak_nits);

    // Bind both layers before beginFrame. With has_scene=false the SDR layer
    // is encoded over black, used by the startup splash before scene loading.
    void prepare(int viewport_w, int viewport_h, bool has_scene, bool has_sdr_layer);

    // Optional caller-owned destination for encoded-signal capture/testing.
    // nullptr restores the HDR swapchain. Detach before destroying the target.
    void set_output_target(filament::RenderTarget* target);

    // Render the encode quad to the swap chain (call after the main view render,
    // inside begin/endFrame).
    void encode();
    // The splash owns a renderer so loading duration cannot change benchmark
    // frame history. Encode on the same renderer whose frame is active.
    void encode(filament::Renderer& renderer);

private:
    void build_pipeline();
    void destroy_target();

    BackendCore core_;

    filament::Texture*      color_tex_ = nullptr;
    filament::Texture*      depth_tex_ = nullptr;
    filament::RenderTarget* rt_        = nullptr;
    filament::Texture* sdr_layer_tex_ = nullptr;
    filament::Texture* sdr_layer_depth_ = nullptr;
    filament::RenderTarget* sdr_layer_rt_ = nullptr;
    filament::Skybox* sdr_background_ = nullptr;
    int                     w_ = 0;
    int                     h_ = 0;

    float reference_nits_ = 100.0f;
    float peak_nits_      = 1000.0f;
    bool  params_dirty_   = true;

    filament::Material*         encode_material_ = nullptr;
    filament::MaterialInstance* encode_mi_       = nullptr;
    filament::VertexBuffer*     encode_vb_       = nullptr;
    filament::IndexBuffer*      encode_ib_       = nullptr;
    utils::Entity               encode_entity_;
    filament::Scene*            composite_scene_ = nullptr;
    filament::View*             composite_view_  = nullptr;
    filament::Texture*          bound_src_       = nullptr;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_HDR_OUTPUT_H_
