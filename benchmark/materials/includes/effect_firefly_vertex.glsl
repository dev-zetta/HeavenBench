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
        float  velocity       = c0.w;
        float  lifetime       = c1.x;

        // Per-slot phase — fireflies don't all blink in sync.
        float total_cycle = lifetime + materialParams.spawnInterval;
        float slot_phase  = slot * materialParams.spawnInterval;
        float age = mod(animationTime - slot_phase, total_cycle);
        float alive = step(age, lifetime);

        float r1 = hash11(slot * 1.7);
        float r2 = hash11(slot * 3.1 + 4.0);
        float r3 = hash11(slot * 5.3 + 9.0);

        // Orbit: each firefly traces its own elliptical path with
        // slot-deterministic radius (0.6 - 1.6 m), tilt and speed.
        float orbit_r = materialParams.particleMotionTuning.x +
                        r1 * materialParams.particleMotionTuning.y;
        float orbit_speed = materialParams.particleMotionTuning.x +
                            r2 * materialParams.particleMotionTuning.y;
        float phase     = r3 * 6.28318530718;
        float t         = animationTime * orbit_speed + phase;
        // Centre the orbit around the emitter origin (which the
        // node tree already places near the lantern). A very small
        // Z bob keeps the swarm from looking like a flat plane.
        float3 orbit = float3(
            cos(t) * orbit_r,
            sin(t) * orbit_r,
            materialParams.particleMotionTuning.z *
                sin(t * 0.7 + phase));

        // Twinkle: smooth on/off envelope per particle so the swarm
        // appears to blink rather than glow constantly.
        float twinkle = 0.5 + 0.5 * sin(animationTime * 4.0 + slot * 7.7);

        // Camera-aligned billboard.
        float3 cam_right = cameraRight;
        float3 cam_up    = cameraUp;
        float radius = base_radius * alive;
        float3 right_v = cam_right * radius;
        float3 up_v    = cam_up    * radius;

        float3 world = emitter_origin + orbit
                     + corner.x * right_v + corner.y * up_v;
        material.worldPosition = float4(world, 1.0);

        material.ageNorm = float4(age / lifetime, twinkle, 0.0, 0.0);
    }
