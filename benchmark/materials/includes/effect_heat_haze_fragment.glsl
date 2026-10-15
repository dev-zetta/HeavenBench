float hash21(float2 p) {
        p = fract(p * float2(123.34, 456.21));
        p += dot(p, p + 45.32);
        return fract(p.x * p.y);
    }

    float vnoise(float2 p) {
        float2 i = floor(p);
        float2 f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
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

    float4 hazeSample() {

        float t01  = variable_ageInfo.x;
        float slot = variable_ageInfo.y;
        float2 ouv = (getUV0() - 0.5) * 2.0; // [-1,1]

        // Generous discard outside the disc.
        float r2 = dot(ouv, ouv);
        if (r2 > 1.0) discard;

        // ── Linear scene-depth occlusion ───────────────────────

        // The haze quad billboards in 3D world space, but we draw
        // them in a separate composite pass that doesn't share
        // Filament's automatic depth test against the main scene.
        // Sample the linear scene-depth texture (F, eye-space
        // forward distance in metres) produced by the depth pre-pass
        // heaven_depth proxy render, and discard wherever opaque
        // scene geometry sits in FRONT of this fragment — otherwise
        // the shimmer shows through walls.

        // Sampling convention is LOCKED (scene-depth-pipeline task
        // the offscreen RT is Y-flipped versus the quad UV
        // (Vulkan top-down framebuffer), so the depth is sampled at
        // float2(uv.x, 1 - uv.y) — the same convention as heaven_blit
        // godrays / volfog / lens flare. getNormalizedViewportCoord()
        // .xy == gl_FragCoord.xy * getResolution().zw.
        float2 fUV = getNormalizedViewportCoord().xy;
        float2 dUV = uvToRenderTargetUV(fUV);
        float sceneDist = texture(materialParams_sceneDepth, dUV).r;

        // Graceful fallback: a null/unbound depth texture reads
        // a non-positive value. Treat that as "far / no geometry" so
        // the haze is NOT occluded before the depth pre-pass produces
        // a valid texture.
        if (sceneDist <= 0.0) sceneDist = 2000.0;

        // Occlude when opaque geometry is nearer to the camera than
        // this haze fragment. A small bias in metres keeps the haze
        // from self-rejecting where it sits just in front of the
        // chimney it rises from (coplanar tolerance).
        float quadDist = variable_ageInfo.z;
        if (sceneDist < quadDist - 0.05) discard;

        // Per-slot UV offset so each quad has unique noise.
        float2 seed_off = float2(hash21(float2(slot, 0.0)),
                                 hash21(float2(0.0, slot))) * 17.0;

        // Scroll noise upward — that's the rising warm-air pattern.
        float2 nuv = ouv * 2.5 + seed_off +
                     float2(0.0,
                            -materialParams.time *
                                materialParams.hazeShapeTuning.w);

        // Two-channel fbm gives us a 2D distortion field.
        float n1 = fbm(nuv);
        float n2 = fbm(nuv + float2(7.3, 2.1));
        float2 dn = (float2(n1, n2) - 0.5) * 2.0;

        // Life envelope: refraction fades in quickly, plateaus,
        // fades out gently.
        float fade_in  = clamp(t01 / 0.20, 0.0, 1.0);
        float fade_out = clamp(1.0 - (t01 - 0.30) / 0.70, 0.0, 1.0);
        float env      = fade_in * fade_out;

        // Soft circular alpha falloff so the disc edge is invisible.
        float alpha_disc = smoothstep(1.0, 0.5, r2);

        // ── Distance fadeout ───────────────────────────────────

        // Heat haze in the original Heaven benchmark is only
        // visible from close range — at the world's scale the
        // shimmer just becomes screen-pixel noise across distant
        // terrain. We compute the distance from the camera to
        // this fragment in world units and fade alpha to zero
        // beyond `fadeFar`.
        float3 cam_pos = float3(frameUniforms.worldFromViewMatrix[3].xyz);
        float  dist    = length(getWorldPosition().xyz - cam_pos);
        float  dist_alpha = 1.0 - smoothstep(materialParams.fadeNear,
                                             materialParams.fadeFar,
                                             dist);
        if (dist_alpha <= 0.001) discard;

        // ── Sample the captured scene at screen_uv + offset ────

        // Filament gives us `getNormalizedViewportCoord()` which
        // is the fragment's NDC position in [0,1]; that's the
        // correct UV for sampling a fullscreen capture texture.
        // The Vulkan capture target is Y-flipped relative to the
        // texture's natural orientation; we flip V to compensate
        // (matches the heaven_blit shader and godrays/volfog).
        // Output the bent sample; alpha controls how strongly it
        // overrides the existing framebuffer (which already shows
        // the unbent scene from the blit pass).
        float alpha = alpha_disc * env * dist_alpha;
        return float4(dn * (materialParams.strength * env), 0.0, alpha);
    }
