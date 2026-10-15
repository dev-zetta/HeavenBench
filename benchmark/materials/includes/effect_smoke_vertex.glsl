// Cheap deterministic hashes from a single float seed.
    float hash11(float x) {
        x = fract(x * 0.1031);
        x *= x + 33.33;
        x *= x + x;
        return fract(x);
    }

    void effectVertex(inout MaterialVertexInputs material, float animationTime, float3 cameraRight, float3 cameraUp, float3 shadingCamera, float3 absoluteCamera) {
        // Decode per-particle attributes.
        float3 emitter_origin = material.worldPosition.xyz;  // POSITION
        float2 corner         = material.uv0.xy * 2.0 - 1.0; // [0,1] → [-1,1]
        float4 c0             = getCustom0();
        float4 c1             = getCustom1();
        float  slot           = c0.x;
        float  base_radius    = c0.y;
        float  growth         = c0.z;
        float  velocity       = c0.w;
        float  lifetime       = c1.x;

        // Per-slot phase: spawn events are uniformly staggered across
        // (lifetime + spawnInterval) so the emitter always has flowing
        // smoke. Slot N spawns at t = N * spawnInterval (modulo cycle).
        float total_cycle = lifetime + materialParams.spawnInterval;
        float slot_phase  = slot * materialParams.spawnInterval;
        float age = mod(animationTime - slot_phase, total_cycle);

        // Slot-deterministic randoms for variety inside the column.
        float r1 = hash11(slot * 1.7);
        float r2 = hash11(slot * 3.1 + 4.0);
        float r3 = hash11(slot * 5.3 + 9.0);

        // Particle inside its lifetime? Once age >= lifetime we collapse
        // the quad to a point (zero radius) so it contributes nothing.
        float alive = step(age, lifetime);

        // Vertical rise from the chimney mouth, with mild lateral wander
        // that gets stronger with age (smoke disperses as it cools).
        float rise   = velocity * age + 0.15 * age * age;     // m
        float t01    = age / lifetime;
        float wander = 0.10 * sin(animationTime * 0.4 + slot * 2.0)
                     + 0.05 * (r1 - 0.5);
        float3 pos_local = float3(
            (r1 - 0.5) * 0.06 + wander * t01,
            (r2 - 0.5) * 0.06 + wander * 0.6 * t01,
            rise);

        // Radius: starts SMALL at the chimney mouth (the original
        // benchmark shows a very thin warm exhaust column) and grows
        // gently with age. The previous formula (0.6× radius at
        // spawn ramping to 1.4×) made the puff already much wider
        // than the chimney itself before it rose at all. Now we
        // start at 25% of the authored radius and ramp linearly to
        // ~110% over the full lifetime; the explicit `growth` term
        // adds the dispersion as the smoke cools.
        float size_ramp = materialParams.particleMotionTuning.x +
                          materialParams.particleMotionTuning.y * t01;
        float radius =
            (base_radius * size_ramp +
             growth * age * materialParams.particleMotionTuning.z) * alive;

        // Camera-aligned billboard. Filament's worldFromViewMatrix
        // gives the camera basis vectors; columns 0 and 1 are right/up.
        float3 cam_right = cameraRight;
        float3 cam_up    = cameraUp;
        float  roll  = r3 * 6.28318530718;
        float  cs    = cos(roll), sn = sin(roll);
        float3 right = (cam_right *  cs + cam_up *  sn) * radius;
        float3 up    = (cam_right * -sn + cam_up *  cs) * radius;

        // Final world position of this corner.
        float3 world = emitter_origin + pos_local
                     + corner.x * right + corner.y * up;
        material.worldPosition = float4(world, 1.0);
        // Light the camera-facing puff with the scene's ambient and direct
        // light. The static mesh tangent frame cannot follow this billboard.
#if defined(HAS_ATTRIBUTE_TANGENTS)
        material.worldNormal = normalize(cross(cam_right, cam_up));
#endif

        // Pass age fraction, slot seed, and rise (height above the
        // emitter origin) to the fragment shader.
        material.ageNorm  = float4(t01, slot, rise, 0.0);
        material.worldPos = float4(world, 1.0);
    }
