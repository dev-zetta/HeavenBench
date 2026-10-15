// Hash + value-noise + FBM kit. Standard real-time smoke pipeline:
    // we shape each particle's alpha by a 3-octave fbm of the UV plus
    // a domain-warp by a second fbm. This breaks the smooth gaussian
    // disc into a wispy organic puff that varies per particle (via
    // the slot seed) and drifts upward over its lifetime.
    float hash21(float2 p) {
        p = fract(p * float2(123.34, 456.21));
        p += dot(p, p + 45.32);
        return fract(p.x * p.y);
    }

    float vnoise(float2 p) {
        float2 i = floor(p);
        float2 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);              // smoothstep weights
        float a = hash21(i);
        float b = hash21(i + float2(1.0, 0.0));
        float c = hash21(i + float2(0.0, 1.0));
        float d = hash21(i + float2(1.0, 1.0));
        return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    }

    float fbm(float2 p) {
        float s = 0.0;
        float a = 0.5;
        for (int i = 0; i < 4; ++i) {
            s += a * vnoise(p);
            p *= 2.07;
            a *= 0.5;
        }
        return s;
    }

    void effectShade(inout MaterialInputs material) {
        prepareMaterial(material);

        float t      = variable_ageNorm.x;     // 0..1 age fraction
        float slot   = variable_ageNorm.y;     // per-particle seed
        float2 ouv   = (getUV0() - 0.5) * 2.0; // [-1, 1]

        // Slot-deterministic UV offset so each particle has a unique
        // noise pattern (otherwise every quad shows the same puff).
        float2 seed_off = float2(hash21(float2(slot, 0.0)),
                                  hash21(float2(0.0, slot))) * 13.0;

        // Domain-warp the UV by a low-frequency fbm that drifts upward
        // over the particle's life — this is what makes the smoke look
        // turbulent rather than a solid disc.
        float2 warp_uv = ouv * 1.5 + seed_off + float2(0.0, -t * 0.6);
        float2 warp = float2(fbm(warp_uv),
                              fbm(warp_uv + float2(5.2, 1.3))) - 0.5;

        // Sample the higher-frequency density fbm at the warped UV.
        float2 nuv = ouv * 2.2 + seed_off + warp * 0.8;
        float  density = fbm(nuv);

        // Soft circular mask. The exponential falloff approaches zero
        // smoothly so there's no visible disc edge.
        float r2   = dot(ouv, ouv);
        float disc = exp(-materialParams.particleShapeTuning.w * r2);

        // ── Smooth shape combination ──────────────────────────

        // The noise threshold is RAISED toward the quad edge so that
        // pixels near the quad boundary need an unattainable density
        // to remain opaque. This shifts the visible edge from the
        // hard quad rectangle into a soft fbm boundary inside the
        // disc, giving wispy edges instead of straight cuts.

        // threshold = 0.05 at centre, ~1.0 at quad edge.
        float threshold = mix(0.05, 1.05, smoothstep(0.0, 1.4, r2));
        float density_alpha =
            smoothstep(threshold,
                       threshold + materialParams.particleMotionTuning.w,
                       density);

        // Final shape uses the disc as a multiplicative envelope on
        // top — even if the noise barely passes threshold near the
        // edge, disc near zero makes it invisible.
        float shape = disc * density_alpha;

        // Life-curve alpha (slow fade-in, long fade-out).
        float fade_in  = clamp(t / 0.20, 0.0, 1.0);
        float fade_out = clamp(1.0 - (t - 0.30) / 0.70, 0.0, 1.0);
        float alpha = clamp(materialParams.alphaScale * fade_in * fade_out * shape, 0.0, 1.0);
        // The former self-bright unlit palette concealed very thin coverage.
        // Double optical depth: two identical densities have transmission
        // (1 - alpha)^2. Keep soft edges/life fades, partial transmission and
        // scene-lit color instead of adding emission or clipping the plume.
        alpha *= 2.0 - alpha;

        // ── Colour gradient ────────────────────────────────────

        // Drive colour by HEIGHT above the chimney mouth, not by
        // per-particle age. Particles overlap in screen-space so any
        // single pixel of the column shows particles at many ages —
        // an age-driven gradient gets smeared into uniform grey.
        // Height is monotonic along the column though, so the bottom
        // really is "young dense smoke" and the top really is
        // "dispersed old smoke", which the eye reads as a vertical
        // gradient.

        // We also vary chroma horizontally with the noise so each
        // puff has visible warm/cool internal structure.

        float rise   = variable_ageNorm.z;            // m above emitter
        float h_t    = clamp(rise / 8.0, 0.0, 1.0);   // 0..1 over 8m

        // These are diffuse albedos, not emitted light. Keep the plume's
        // density variation while allowing its color to follow illumination.
        float3 col_dark = materialParams.tint * 0.65;
        float3 col_mid  = materialParams.tint * 1.00;
        float3 col_top  = materialParams.tint * 1.20;

        float3 height_col = (h_t < 0.30)
            ? mix(col_dark, col_mid, smoothstep(0.0, 0.30, h_t))
            : mix(col_mid,  col_top, smoothstep(0.30, 1.0, h_t));

        // Interior light/dark variation from the noise — this is what
        // gives each puff the visible bumpy structure.
        float core   = smoothstep(0.40, 0.90, density);
        float3 rgb   = clamp(mix(height_col * 1.20, height_col * 0.55, core),
                             float3(0.0), float3(1.0));

        // Premultiplied output for `transparent` blending.
        material.baseColor = float4(rgb * alpha, alpha);
#if !defined(HEAVEN_TEMPORAL_EFFECT)
        material.roughness = 1.0;
        material.metallic = 0.0;
        material.reflectance = 0.0;
#endif
    }
