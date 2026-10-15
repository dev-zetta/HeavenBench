// ── Noise kit (hash → value noise → fbm) ────────────────────
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
        float s = 0.0, a = 0.5;
        for (int i = 0; i < 4; ++i) { s += a * vnoise(p); p *= 2.02; a *= 0.5; }
        return s;
    }

    void effectShade(inout MaterialInputs material) {
        prepareMaterial(material);

        float2 uv   = variable_flameUV.xy;     // 0..1, y=0 at wick
        float  seed = variable_flameUV.z;
        // Upward-scrolling time term: noise flows DOWN the UV so the
        // flame appears to rise. flameSpeed keeps it slow and
        // framerate-independent.
        float  t = materialParams.time * materialParams.flameSpeed + seed * 7.0;

        // Centre x around 0 so |x| is the distance from the flame axis.
        float2 p = float2(uv.x - 0.5, uv.y);

        // ── Horizontal turbulence (the flicker) ────────────────
        // Domain-warp the axis sideways by upward-scrolling noise.
        // The warp grows with height so the base stays anchored on
        // the wick and the tip whips around.
        float warp = (fbm(float2(p.x * 3.0 + seed, p.y * 2.5 - t)) - 0.5);
        p.x += warp * 0.35 * p.y;

        // ── Flame envelope ─────────────────────────────────────
        // Flickering top height (so the flame breathes up and down).
        float top = 0.82 + 0.16 * (fbm(float2(seed * 3.1, -t * 0.9)) - 0.5) * 2.0;

        // Width profile: a teardrop — pinched at the very base,
        // bulging low, tapering to a point at `top`. Narrower than
        // before so the flame is a slim tongue, not a fat blob.
        float yn = clamp(p.y / max(top, 1e-3), 0.0, 1.0);
        float width = 0.26 * sin(3.14159 * pow(yn, 0.65)) * (1.0 - yn * 0.15);
        width = max(width, 1e-3);

        // Distance from the axis, normalised by the local width.
        float d = abs(p.x) / width;

        // Core mask: 1 on the axis, 0 at the flame edge; cut above top.
        float body = (1.0 - d) * step(p.y, top);
        body = clamp(body, 0.0, 1.0);

        // High-frequency detail eats INTO the flame so it looks like
        // wispy licking tongues rather than a solid teardrop. We
        // subtract a noise field (rather than just modulating) so
        // holes and gaps open up through the body, especially toward
        // the edges and tip.
        float detail = fbm(float2(p.x * 8.0 + seed * 5.0, p.y * 6.0 - t * 1.8));
        // Erode: stronger erosion higher up and nearer the edge.
        float erode = (0.35 + 0.5 * d + 0.4 * yn) * (1.0 - detail);
        body = clamp(body - erode, 0.0, 1.0);

        // Intensity: sharpen the core so the centre reads white-hot.
        float intensity = smoothstep(0.04, 0.45, body);

        // ── Fire colour ramp ───────────────────────────────────
        // edge → deep red, mid → orange, inner → yellow, core → white
        float3 col = float3(0.55, 0.04, 0.0);
        col = mix(col, float3(1.0, 0.42, 0.05), smoothstep(0.05, 0.45, intensity));
        col = mix(col, float3(1.0, 0.85, 0.25), smoothstep(0.45, 0.75, intensity));
        col = mix(col, float3(1.0, 0.98, 0.85), smoothstep(0.80, 1.0, intensity));

        // Apply the per-emitter tint and push into HDR so bloom
        // catches the bright core. Per-plane brightness kept modest
        // because 3 overlapping planes accumulate additively.
        col *= materialParams.tint * 1.0;

        // Body mask with a soft silhouette fade. For additive
        // blending we premultiply the colour by this mask so masked
        // regions add nothing. Kept low so the flame stays airy and
        // translucent — you can see through the wisps rather than it
        // being a solid opaque tongue.
        float a = smoothstep(0.02, 0.40, body) *
                  materialParams.alphaScale *
                  materialParams.particleShapeTuning.w;

        // Additive output — RGB is added to the framebuffer; the
        // alpha channel is unused by `add` blending but we keep it
        // consistent.
        material.baseColor = float4(col * a, a);
    }
