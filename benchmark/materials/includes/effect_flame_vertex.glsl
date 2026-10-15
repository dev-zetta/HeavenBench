void effectVertex(inout MaterialVertexInputs material, float animationTime, float3 cameraRight, float3 cameraUp, float3 shadingCamera, float3 absoluteCamera) {
        // POSITION holds the wick world position (same for all 4
        // verts of a plane). UV0 is the corner id in [0,1].
        // CUSTOM0.x is the plane index (0..N-1), CUSTOM0.y the
        // authored base radius.
        float3 wick   = material.worldPosition.xyz;
        float2 uv01   = material.uv0.xy;
        float4 c0     = getCustom0();
        float  plane  = c0.x;
        float  base_radius = c0.y;

        float half_width =
            max(0.05, base_radius) * materialParams.particleShapeTuning.x;
        float height =
            max(0.05, base_radius) * materialParams.particleShapeTuning.y;

        // ── World-oriented cross-planes ────────────────────────
        // Each plane is a vertical quad rotated about the world Z
        // (up) axis by a fixed yaw. With 3 planes at 0/60/120° the
        // flame reads as a 3D volume from any side — orbiting the
        // torch shows the planes overlapping rather than one flat
        // card rotating to face you. kPlanes MUST match kFlamePlanes
        // in filament_backend.cpp.
        const float kPlanes = 3.0;
        float yaw = plane * (3.14159265 / kPlanes);
        float3 right_axis = float3(cos(yaw), sin(yaw), 0.0);
        float3 up_axis    = float3(0.0, 0.0, 1.0);

        // Drop the quad so its lower third sits BELOW the emitter
        // origin — the flame should wrap around and rise out of the
        // torch cup, not float a gap above it. The emitter origin
        // (from torch_light.node) sits a little above the cup, so
        // sinking the base by ~0.35× the height tucks the flame
        // root back down into the torch head.
        float anchor_drop = height * materialParams.particleShapeTuning.z;

        float cx = uv01.x * 2.0 - 1.0;       // [-1,1] across width
        float3 world = wick
                     + cx * right_axis * half_width
                     + (uv01.y * height - anchor_drop) * up_axis;
        material.worldPosition = float4(world, 1.0);

        // Per-plane seed offset so each plane flickers differently.
        material.flameUV = float4(uv01, materialParams.seed + plane * 1.37, 0.0);
    }
