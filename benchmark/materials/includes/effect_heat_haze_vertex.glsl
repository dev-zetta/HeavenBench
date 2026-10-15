float hash11(float x) {
        x = fract(x * 0.1031);
        x *= x + 33.33;
        x *= x + x;
        return fract(x);
    }

    void hazeVertex(inout MaterialVertexInputs material) {
        float3 emitter_origin = material.worldPosition.xyz;
        float2 corner = material.uv0.xy * 2.0 - 1.0;
        float4 c0 = getCustom0();
        float4 c1 = getCustom1();
        float slot       = c0.x;
        float base_radius= c0.y;
        float growth     = c0.z;
        float velocity   = c0.w;
        float lifetime   = c1.x;

        float total_cycle = lifetime + materialParams.spawnInterval;
        float slot_phase  = slot * materialParams.spawnInterval;
        float age = mod(materialParams.time - slot_phase, total_cycle);

        float r1 = hash11(slot * 1.7);
        float r2 = hash11(slot * 3.1 + 4.0);
        float r3 = hash11(slot * 5.3 + 9.0);

        float alive = step(age, lifetime);
        float t01   = age / lifetime;

        // Heat haze rises slightly faster than the smoke does — the
        // distortion is the leading edge of the warm air column.
        float rise = (velocity * materialParams.hazeShapeTuning.x) * age +
                     materialParams.hazeShapeTuning.y * age * age;
        float wander =
            materialParams.hazeShapeTuning.z *
            sin(materialParams.time * 0.6 + slot * 2.7);
        float3 pos_local = float3(
            (r1 - 0.5) * 0.04 + wander * t01,
            (r2 - 0.5) * 0.04 + wander * 0.6 * t01,
            rise);

        // Quads start small at the chimney mouth and expand upward
        // as the heated air cools and disperses.
        float size_ramp = 0.30 + 1.20 * t01;
        float radius = (base_radius * size_ramp + growth * age * 0.5) * alive;

        float3 cam_right = float3(frameUniforms.worldFromViewMatrix[0].xyz);
        float3 cam_up    = float3(frameUniforms.worldFromViewMatrix[1].xyz);
        float roll = r3 * 6.28318530718;
        float cs = cos(roll), sn = sin(roll);
        float3 right = (cam_right *  cs + cam_up *  sn) * radius;
        float3 up    = (cam_right * -sn + cam_up *  cs) * radius;

        float3 world = emitter_origin + pos_local
                     + corner.x * right + corner.y * up;
        material.worldPosition = float4(world, 1.0);

        // Eye-space forward distance (metres) of this haze fragment, for
        // the linear scene-depth occlusion test in the fragment shader.
        // The scene-depth texture stores the same quantity (-viewZ of the
        // nearest opaque surface), so the comparison is apples-to-apples.
        float4 viewPos  = getViewFromWorldMatrix() * float4(world, 1.0);
        float  quadDist = -viewPos.z;
        material.ageInfo = float4(t01, slot, quadDist, 0.0);
    }
