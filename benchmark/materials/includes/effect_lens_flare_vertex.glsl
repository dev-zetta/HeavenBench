void effectVertex(inout MaterialVertexInputs material, float animationTime, float3 cameraRight, float3 cameraUp, float3 shadingCamera, float3 absoluteCamera) {
        // POSITION holds the lamp world position (same for all 4 verts).
        // UV0 is the corner id in [0,1].
        float3 lpos   = material.worldPosition.xyz;
        float2 corner = material.uv0.xy * 2.0 - 1.0;

        // Both positions must use Filament's shifted/rotated world frame.
        // A user-world camera uniform would apply the camera offset twice.
        float3 cpos    = shadingCamera;
        float3 toCam   = cpos - lpos;
        float  dist    = length(toCam);
        float3 dirCam  = toCam / max(dist, 1e-4);

        // Anchor the billboard AT the lamp position. The camera-ward push
        // is gone — soft depth occlusion replaces the hardware depth test,
        // so the quad no longer needs to be shoved in front of the wall to
        // avoid a hard clip. Keep the near-plane clamp so a lamp closer
        // than the 0.5 m near plane still anchors just in front of it and
        // stays visible. Moving along the camera→light ray would
        // not change the on-screen position; here D == dist for any lamp
        // beyond the near plane, so anchor == lpos.
        float D    = max(dist, 0.55);
        float3 anchor = cpos - dirCam * D;

        // Proximity ramp: tiny far away, larger and softer up close.
        float close_t = clamp((materialParams.fadeFar - dist) /
                              max(0.001, materialParams.fadeFar - materialParams.fadeNear),
                              0.0, 1.0);
        float near_t = close_t * close_t;

        // Lens flares are perceived in screen/angle space. Build a world-space
        // radius from an angular size so far lamps stay as small glints while
        // nearby lamps naturally grow on screen. maxRadius is a safety cap,
        // not the primary sizing curve.
        float angularRadius = mix(0.0045, 0.070, near_t);
        float radius = angularRadius * dist;
        radius = min(radius, mix(materialParams.maxRadius * 0.45,
                                 materialParams.maxRadius * 1.85,
                                 near_t));
        // Keep the billboard half-extent in front of the anchor.
        radius = min(radius, D * 0.85);

        float3 cam_right = cameraRight;
        float3 cam_up    = cameraUp;

        float3 world = anchor
                     + corner.x * cam_right * radius
                     + corner.y * cam_up    * radius;
        material.worldPosition = float4(world, 1.0);

        // Eye-space forward distance (metres) of the flare anchor, for the
        // soft depth-occlusion test in the fragment shader. The scene-depth
        // texture stores the same quantity (-viewZ of the nearest surface),
        // so the comparison is apples-to-apples.
        float4 anchorView = getViewFromWorldMatrix() * float4(anchor, 1.0);
        float quadDist = -anchorView.z;

        material.flareCtx = float4(close_t, dist, quadDist, 0.0);

        // Screen-space UV of the lamp ANCHOR (not this vertex), so the fragment
        // shader can fade the WHOLE flare by the lamp's own visibility.
        float4 anchorClip = getClipFromViewMatrix() * anchorView;
        float2 anchorNDC  = anchorClip.xy / max(anchorClip.w, 1e-4);
        material.anchorInfo = float4(anchorNDC * 0.5 + 0.5, 0.0, 0.0);
    }
