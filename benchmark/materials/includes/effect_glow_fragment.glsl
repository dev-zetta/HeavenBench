void effectShade(inout MaterialInputs material) {
        prepareMaterial(material);

        float t    = variable_ageNorm.x;
        float2 uv  = getUV0();

        // Fetch the soft halo. The texture is a radial gradient — bright
        // at the centre, fading to zero at the edges — so we just use
        // the red channel directly as the alpha shape.
        float halo = texture(materialParams_glowTex, uv).r;

        // Lifetime envelope: gentle fade in and out, peak in the middle.
        float fade_in  = clamp(t / 0.20, 0.0, 1.0);
        float fade_out = clamp(1.0 - (t - 0.30) / 0.70, 0.0, 1.0);
        float life_alpha = fade_in * fade_out;

        float a = halo * life_alpha * materialParams.alphaScale;

        // HDR colour for bloom response.
        float3 rgb =
            materialParams.tint * materialParams.particleShapeTuning.w;

        material.baseColor = float4(rgb * a, a);
    }
