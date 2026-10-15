void effectShade(inout MaterialInputs material) {
        prepareMaterial(material);

        float dist = variable_flareCtx.y;

        // Sample the flare halo (fire_glow_dr.dds — radial gradient).
        float2 uv = getUV0();
        float halo = texture(materialParams_flareTex, uv).r;

        // Radial coordinate: 0 at centre, 1 at edge midpoints.
        float2 ouv = (uv - 0.5) * 2.0;
        float r = length(ouv);

        // Radial starburst overlay. It grows with proximity: distant lamps are
        // mostly soft bloom, close lamps develop visible aperture streaks.
        float angle = atan(ouv.y, ouv.x);
        float streak = pow(max(0.0, cos(angle * 3.0)), 12.0)
                     + pow(max(0.0, cos(angle * 3.0 + 1.57)), 12.0);
        float streakMask = streak * exp(-1.35 * r);

        // Soft circular window so the additive quad never shows its
        // square silhouette — keeps the glint round.
        float window = smoothstep(1.0, 0.15, r);

        // Brightness ramp. Fade in from far distance and keep the close-up
        // lamp glint visible; the per-pixel depth trim below handles spill
        // against nearby opaque surfaces.
        float prox = clamp((materialParams.fadeFar - dist) /
                           max(0.001, materialParams.fadeFar - materialParams.fadeNear),
                           0.0, 1.0);
        float closeFade = smoothstep(0.20, 0.75, dist);
        float ramp = prox * closeFade;

        float nearShape = clamp((materialParams.fadeNear - dist) /
                                max(0.001, materialParams.fadeNear),
                                0.0, 1.0);
        float core = halo * mix(1.0, 1.45, nearShape)
                   + streakMask * mix(0.28, 0.80, nearShape);
        float intensity = core * window * ramp
                        * materialParams.brightness * mix(2.3, 3.2, nearShape);

        // ── Soft depth occlusion ───────────────────────────────────────
        // Anchor occlusion fades the whole flare when the lamp itself is
        // hidden. Pixel occlusion trims additive spill where the quad overlaps
        // nearby opaque geometry. Both use the locked render-target UV helper.
        float2 aUV = clamp(variable_anchorInfo.xy, 0.0, 1.0);
        float anchorDist = texture(materialParams_depthTexture,
                                   uvToRenderTargetUV(aUV)).r;
        float pixelDist = texture(materialParams_depthTexture,
                                  uvToRenderTargetUV(getNormalizedViewportCoord().xy)).r;

        // Graceful fallback: a null/unbound depth texture reads a
        // non-positive value. Treat that as "far / no geometry" so the
        // flare is NOT occluded before the depth pre-pass produces a valid
        // texture.
        if (anchorDist <= 0.0) anchorDist = materialParams.flareShaderTuning.x;
        if (pixelDist <= 0.0) pixelDist = materialParams.flareShaderTuning.x;

        // Occlude when opaque geometry sits in FRONT of the lamp anchor.
        // `tol` tolerates geometry within a small distance ahead of the
        // emitter (the lamp's own housing/glass); `band` gives a
        // soft fade so the flare has no hard pop as the lamp goes behind
        // geometry. When no geometry is in front (sceneDist >=
        // quadDist, including the far/sky value) occl == 0.
        float quadDist = variable_flareCtx.z;
        float anchorTol = materialParams.flareShaderTuning.y;
        float anchorBand = materialParams.flareShaderTuning.z;
        float anchorOccl = smoothstep(anchorTol, anchorTol + anchorBand,
                                      quadDist - anchorDist);

        const float pixelTol  = 0.15;
        const float pixelBand = 0.65;
        float pixelOccl = smoothstep(pixelTol, pixelTol + pixelBand,
                                     quadDist - pixelDist);
        float spillTrim = smoothstep(0.55, 1.05, r);
        intensity *=
            (1.0 - anchorOccl) *
            (1.0 - pixelOccl * spillTrim *
                       materialParams.flareShaderTuning.w);

        float3 rgb = materialParams.tint * intensity;
        material.baseColor = float4(rgb, 1.0);
    }
