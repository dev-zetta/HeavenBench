float hash11(float x) {
        x = fract(x * 0.1031);
        x *= x + 33.33;
        x *= x + x;
        return fract(x);
    }

    void effectVertex(inout MaterialVertexInputs material, float animationTime, float3 cameraRight, float3 cameraUp, float3 shadingCamera, float3 absoluteCamera) {
        float3 emitter_origin = material.worldPosition.xyz;
        float2 corner         = material.uv0.xy * 2.0 - 1.0;
        float4 c0             = getCustom0();
        float4 c1             = getCustom1();
        float  slot           = c0.x;
        float  base_radius    = c0.y;
        float  growth         = c0.z;
        float  velocity       = c0.w;
        float  lifetime       = c1.x;

        float total_cycle = lifetime + materialParams.spawnInterval;
        float slot_phase  = slot * materialParams.spawnInterval;
        float age = mod(animationTime - slot_phase, total_cycle);
        float alive = step(age, lifetime);

        float r1 = hash11(slot * 1.7);
        float r2 = hash11(slot * 3.1 + 4.0);

        // Halo is anchored at the emitter — barely moves, just pulses.
        // A small upward drift keeps successive halos slightly offset
        // so the bloom doesn't sample the exact same pixel every time.
        float t01    = age / lifetime;
        float rise =
            velocity * age * materialParams.particleShapeTuning.x;
        float3 pos_local = float3(
            (r1 - 0.5) * 0.05,
            (r2 - 0.5) * 0.05,
            rise);

        // Radius grows generously over life — the glow expands as it
        // fades, that's the visual signature.
        float radius = (base_radius + growth * age) * alive;

        // Pure camera-aligned billboard (this is a halo, not a flame —
        // we don't want a vertical bias).
        float3 cam_right = cameraRight;
        float3 cam_up    = cameraUp;
        float3 right = cam_right * radius;
        float3 up    = cam_up    * radius;

        float3 world = emitter_origin + pos_local
                     + corner.x * right + corner.y * up;
        material.worldPosition = float4(world, 1.0);

        material.ageNorm = float4(t01, slot, rise, 0.0);
    }
