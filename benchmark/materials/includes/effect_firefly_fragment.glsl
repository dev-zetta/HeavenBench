void effectShade(inout MaterialInputs material) {
        prepareMaterial(material);

        float t       = variable_ageNorm.x;
        float twinkle = variable_ageNorm.y;
        float2 uv     = getUV0();

        // Sample the soft halo so the firefly has a smooth edge.
        float halo = texture(materialParams_glowTex, uv).r;

        // Lifetime envelope.
        float fade_in  = clamp(t / 0.10, 0.0, 1.0);
        float fade_out = clamp(1.0 - (t - 0.20) / 0.80, 0.0, 1.0);
        float life_alpha = fade_in * fade_out;

        // HDR additive — bloom turns the bright specks into glowing
        // dots. Twinkle modulates per-frame brightness so the swarm
        // appears alive.
        float intensity = halo * life_alpha * twinkle * materialParams.alphaScale;
        float3 rgb = materialParams.tint * intensity *
                     materialParams.particleMotionTuning.w;
        material.baseColor = float4(rgb, intensity);
    }
