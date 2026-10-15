// Scalar hash to [0,1). Mirrors weather_shader_ref::hash1 exactly:
    //   fract(sin(n) * 43758.5453123)
    float hash1(float n) {
        return fract(sin(n) * 43758.5453123);
    }

    // Vec3 hash to [0,1)^3. Mirrors weather_shader_ref::hash3 exactly, including
    // the dot constants — these MUST match the CPU reference byte-for-byte.
    float3 hash3(float3 seed) {
        return float3(
            hash1(dot(seed, float3(127.1, 311.7, 74.7))),
            hash1(dot(seed, float3(269.5, 183.3, 246.1))),
            hash1(dot(seed, float3(113.5, 271.9, 124.6))));
    }

    // ── Shared drop-lattice hashes (mirror weather_shader_ref::hash21/hash22 and
    //    heaven_splash.mat byte-for-byte) ──
    // These are the Dave-Hoskins style hashes the SPLASH shader uses to derive
    // the per-cell jitter / phase. The streak shader uses the SAME ones so a
    // streak column and the splash cell it lands in agree on the same world XY.
    float hash21(float2 p) {
        float3 p3 = fract(float3(p.xyx) * 0.1031);
        p3 += dot(p3, p3.yzx + 33.33);
        return fract((p3.x + p3.y) * p3.z);
    }
    float2 hash22(float2 p) {
        float3 p3 = fract(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
        p3 += dot(p3, p3.yzx + 33.33);
        return fract((p3.xx + p3.yz) * p3.zy);
    }

    // Geometry and temporal correspondence share the exact lattice/fall law.
    float4 effectRainCell(float3 seed, float3 absoluteCamera, float extent, float fallSpeed) {
        float2 seedXY = (hash3(seed).xy * 2.0 - 1.0) * extent;
        float2 relative = mod(seedXY - absoluteCamera.xy + extent, 2.0 * extent) - extent;
        float2 cell = floor((absoluteCamera.xy + relative) / 1.2);
        float period = (2.0 * extent / fallSpeed) * mix(0.8, 1.4, hash21(cell + 29.7));
        return float4(cell, period, hash21(cell + 5.3) * period);
    }

    void effectVertex(inout MaterialVertexInputs material, float animationTime, float3 cameraRight, float3 cameraUp, float3 shadingCamera, float3 absoluteCamera) {
        // Half thickness of a streak in world units (camera-facing width).
        // Issue 2 tuning: thinned from 0.08 -> 0.03 so each streak reads as a
        // fine rain line rather than a fat bright dash. Combined with the lower
        // fragment alpha gain and cooler/darker tint below, this keeps streaks
        // from blowing out to white against the bright overcast sky.
        float streakHalfWidth = materialParams.rainShaderTuning.x;

        // Shared drop-lattice constants. kDropSpacing MUST equal
        // weather_shader_ref::kDropSpacing AND the kRippleSpacing constant in
        // heaven_splash.mat so the streak column and the splash cell tile on the
        // SAME absolute world XY grid. The fall-period variation band and the
        // per-cell hash seeds below also mirror weather_shader_ref byte-for-byte.
        const float kDropSpacing       = 1.2;

        // ── Shared WIND constants (mirror weather_shader_ref::kWindDir /
        //    kWindStrength* / kWindGustFreq byte-for-byte) ──
        // The wind is a horizontal drift that grows with how far a drop has
        // fallen; the SAME model is applied by heaven_splash.mat so a slanted
        // drop splashes exactly where it lands. windStrength is a slow,
        // purely-time-driven gust (base > amp so it never reverses); at
        // strength 0 the drift is exactly zero (no-wind fall unchanged).
        const float2 kWindDir          = normalize(float2(1.0, 0.4));
        const float  kWindStrengthBase = 0.18;
        const float  kWindStrengthAmp  = 0.005;
        const float  kWindGustFreq     = 0.3;

        float4 c0    = getCustom0();
        float3 seed  = c0.xyz;        // per-streak seed (CUSTOM0.xyz)
        float  idx   = c0.w;          // streak slot index (CUSTOM0.w)
        float2 corner = material.uv0.xy * 2.0 - 1.0; // [0,1] -> [-1,1]

        // ── Two camera references (see file header) ──
        // Filament shades in CAMERA-RELATIVE world space, so the FINAL placement
        // of material.worldPosition must be relative to the SHADING-space camera
        // `camShading` (read from the view matrix). The per-drop lattice math is
        // done in ABSOLUTE world space against `absoluteCamera`
        // (camAbs); the final translate maps absolute world -> shading space via
        // `+ camShading - camAbs` (a constant per-frame offset), which makes the
        // drops world-static while keeping the volume correctly around the camera.
        float3 camShading = shadingCamera;
        float3 camAbs     = absoluteCamera;
        float  E          = materialParams.volumeExtent;

        // ── Shared lattice CELL for this streak slot (mirrors
        //    weather_shader_ref::streak_seed_cell byte-for-byte) ──
        // World-anchored: snap the tiled instance of this seed nearest the
        // camera to the kDropSpacing lattice, so the cell holds still in world
        // space as the camera moves (camera flies THROUGH the rain) and wraps by
        // one window when the seed drifts a full span away (density constant).
        float4 cellLaw = effectRainCell(seed, camAbs, E, materialParams.fallSpeed);
        float2 cell = cellLaw.xy;
        float fallPeriod = cellLaw.z;
        float vSpeed = (2.0 * E) / fallPeriod;
        float cellPhase = cellLaw.w;
        float  dropTopZ   = camAbs.z + E;                       // drop_top_z
        float  z          = dropTopZ - vSpeed * mod(animationTime - cellPhase, fallPeriod);

        // ── Jittered column world XY (mirrors drop_column_xy byte-for-byte) ──
        // This is the SAME world XY the splash shader blooms its ripple at,
        // BEFORE wind drift; with wind the landing point is offset (below) and
        // the splash applies the identical offset, so they still coincide.
        float2 jitter = hash22(cell * 1.37 + 3.1);
        float2 colXY  = (cell + jitter) * kDropSpacing;

        // ── WIND drift (mirrors wind_strength / drop_wind_offset / drop_xy_at_z
        //    byte-for-byte) ──
        // Horizontal drift grows with the distance fallen below the top of the
        // volume; windStrength is the slow time-varying gust. At strength 0 the
        // drift is exactly zero so this reduces to the pure vertical fall.
        float windStrength = kWindStrengthBase
                           + kWindStrengthAmp * sin(animationTime * kWindGustFreq);
        float fallenDist   = dropTopZ - z;                      // >= 0 in the band
        float2 windOffset  = kWindDir * windStrength * fallenDist;
        float2 dropXY      = colXY + windOffset;

        // Absolute world drop position; map into camera-relative shading space.
        float3 absWorld = float3(dropXY.x, dropXY.y, z);
        float3 center   = absWorld - camAbs + camShading;

        // ── Per-drop randomised appearance (distinct hash seeds so length,
        //    brightness and width do not correlate) ──
        // length in [streakLenMin, streakLenMax] (mirrors
        // weather_shader_ref::streak_length, hash1(seed.x)); brightness ~[0.5,1]
        // and a small width jitter ~[0.7,1.3] give each drop its own look.
        float len       = mix(materialParams.streakLenMin, materialParams.streakLenMax, hash1(seed.x));
        float halfLen   = 0.5 * len;
        float bright    = mix(0.5, 1.0, hash1(seed.y * 1.7 + 3.1));
        float widthRand = mix(0.7, 1.3, hash1(seed.z * 2.3 + 7.9));
        float halfWidth = streakHalfWidth * widthRand;

        // ── Camera-facing billboard, elongated along the drop's VELOCITY ──
        // The streak's long axis is the drop's velocity direction (down + the
        // per-second wind drift), so slanted rain visibly slants. Velocity =
        // (windOffsetPerSec, -vSpeed): the horizontal component is the wind
        // drift per second (windStrength * vSpeed along kWindDir, since the drop
        // falls vSpeed m/s and drifts windStrength per metre fallen) and the
        // vertical component is the downward fall speed. At windStrength 0 this
        // is exactly world -Z (pure vertical), matching the old billboard.
        float3 velDir   = float3(kWindDir * (windStrength * vSpeed), -vSpeed);
        float  vl       = length(velDir);
        float3 longAxis = (vl > 1e-5) ? (velDir / vl) : float3(0.0, 0.0, -1.0);

        // Width axis is horizontal, perpendicular to both the long axis and the
        // line of sight, so the flat face points at the camera (billboard).
        float3 toCam = camShading - center;
        float3 right = cross(longAxis, toCam);
        float  rl    = length(right);
        right = (rl > 1e-5) ? right / rl : float3(1.0, 0.0, 0.0);

        float3 world = center
                     + right    * (corner.x * halfWidth)
                     + longAxis * (corner.y * halfLen);

        // ── Active-streak cull  ──
        // idx >= activeStreaks, or rain intensity 0, collapses the whole quad to
        // a single point (zero area => renders nothing), keeping the active
        // count / density driven purely by activeStreaks.
        float active_  = step(idx + 0.5, float(materialParams.activeStreaks));
        float raining = step(0.0001, materialParams.rainIntensity);
        float accept  = active_ * raining;
        world = mix(camShading, world, accept);

        material.worldPosition = float4(world, 1.0);
        material.streakParams  = float4(accept, bright, 0.0, 0.0);
    }
