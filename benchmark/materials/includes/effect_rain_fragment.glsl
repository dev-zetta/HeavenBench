void effectShade(inout MaterialInputs material) {
        prepareMaterial(material);

        float accept = variable_streakParams.x;   // 0 => collapsed, nothing to draw
        float bright = variable_streakParams.y;   // per-drop brightness scale ~[0.5,1]

        // Quad-local coordinates: x across the streak width, y along its length.
        float2 p      = getUV0() * 2.0 - 1.0;      // [-1, 1]
        float  across = abs(p.x);
        float  along  = abs(p.y);

        // Soft vertical streak: a bright thin core that falls off toward the
        // horizontal edges, tapered at the top and bottom ends so the streak
        // fades rather than ending in a hard rectangle.
        float core = 1.0 - smoothstep(0.0, 1.0, across);
        float ends = 1.0 - smoothstep(0.55, 1.0, along);

        // Modulate the whole streak by rain intensity: denser rain reads
        // as stronger streaks; intensity 0 contributes nothing. LOOK PASS: the
        // alpha gain dropped 0.55 -> 0.35 so streaks are clearly translucent
        // dashes rather than bright white lines, and each drop is scaled by its
        // own per-drop brightness (variable_streakParams.y) so drops differ.
        float alpha = accept * bright * core * ends *
                      materialParams.rainIntensity *
                      materialParams.rainShaderTuning.y;

        // Cool rain tint. LOOK PASS: cooled/darkened from (0.62, 0.68, 0.78)
        // toward a deeper, less-luminant grey-blue so streaks no longer read as
        // bright white against the overcast sky. Output premultiplied alpha for
        // Filament's `transparent` blender (same convention as heaven_unlit /
        // heaven_splash).
        float3 col = float3(materialParams.rainShaderTuning.z,
                            0.58,
                            materialParams.rainShaderTuning.w);
        material.baseColor = float4(col * alpha, alpha);
    }
