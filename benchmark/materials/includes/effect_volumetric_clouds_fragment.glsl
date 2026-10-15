// ── Shared constants (mirror cloud_noise_ref.h byte-for-byte) ──────

    // kPi: reproduced locally so the Henyey-Greenstein normalisation matches
    // the reference literal exactly.
    const float kPi = 3.14159265358979323846;

    // Beer-Lambert extinction coefficient (per unit accumulated optical depth).
    const float kCloudExtinction = 1.0;

    // Cloud-cover density threshold.
    const float kCloudDensityThreshold = 0.5;

    // Henyey-Greenstein anisotropy (forward scattering toward the sun).
    const float kCloudPhaseG = 0.6;

    // Horizontal wind (world XY plane, Z-up world). kCloudWindDir mirrors the
    // reference's glm::normalize(vec3(1.0, 0.3, 0.0)); kCloudWindSpeed is world
    // units per second.
    // ── GLSL-portable hash (mirror cloud_noise_ref.h::hash33) ──────────
    vec3 hash33(vec3 p) {
        vec3 q = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
                      dot(p, vec3(269.5, 183.3, 246.1)),
                      dot(p, vec3(113.5, 271.9, 124.6)));
        return fract(sin(q) * 43758.5453123);
    }

    // ── 3D worley (cellular) noise (mirror cloud_noise_ref.h::worley3) ──
    // Returns the distance to the nearest per-cell feature point in the
    // 3x3x3 neighbourhood, clamped to [0,1]. Deterministic in `p`.
    float worley3(vec3 p) {
        vec3 pi = floor(p);
        vec3 pf = p - pi;

        float minDist = 1.0;   // also the clamp ceiling, so result is in [0,1]
        for (int z = -1; z <= 1; ++z) {
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    vec3 cell = vec3(float(x), float(y), float(z));
                    vec3 feature = cell + hash33(pi + cell);
                    vec3 diff = feature - pf;
                    minDist = min(minDist, length(diff));
                }
            }
        }
        return clamp(minDist, 0.0, 1.0);
    }

    // ── 3D perlin (gradient) noise (mirror cloud_noise_ref.h::perlin3) ──
    // Improved-Perlin gradient noise with the quintic fade, remapped to
    // [0,1] via value*0.5 + 0.5 and clamped. Deterministic in `p`.
    float perlin3(vec3 p) {
        vec3 pi = floor(p);
        vec3 pf = p - pi;

        // Quintic fade weights (component-wise).
        vec3 u = pf * pf * pf * (pf * (pf * 6.0 - 15.0) + 10.0);

        // Eight corner gradient contributions (random gradient in [-1,1]^3
        // dotted with the corner->p vector).
        vec3 g000 = hash33(pi + vec3(0.0, 0.0, 0.0)) * 2.0 - 1.0;
        vec3 g100 = hash33(pi + vec3(1.0, 0.0, 0.0)) * 2.0 - 1.0;
        vec3 g010 = hash33(pi + vec3(0.0, 1.0, 0.0)) * 2.0 - 1.0;
        vec3 g110 = hash33(pi + vec3(1.0, 1.0, 0.0)) * 2.0 - 1.0;
        vec3 g001 = hash33(pi + vec3(0.0, 0.0, 1.0)) * 2.0 - 1.0;
        vec3 g101 = hash33(pi + vec3(1.0, 0.0, 1.0)) * 2.0 - 1.0;
        vec3 g011 = hash33(pi + vec3(0.0, 1.0, 1.0)) * 2.0 - 1.0;
        vec3 g111 = hash33(pi + vec3(1.0, 1.0, 1.0)) * 2.0 - 1.0;

        float n000 = dot(g000, pf - vec3(0.0, 0.0, 0.0));
        float n100 = dot(g100, pf - vec3(1.0, 0.0, 0.0));
        float n010 = dot(g010, pf - vec3(0.0, 1.0, 0.0));
        float n110 = dot(g110, pf - vec3(1.0, 1.0, 0.0));
        float n001 = dot(g001, pf - vec3(0.0, 0.0, 1.0));
        float n101 = dot(g101, pf - vec3(1.0, 0.0, 1.0));
        float n011 = dot(g011, pf - vec3(0.0, 1.0, 1.0));
        float n111 = dot(g111, pf - vec3(1.0, 1.0, 1.0));

        // Trilinear interpolation with the faded weights.
        float nx00 = mix(n000, n100, u.x);
        float nx10 = mix(n010, n110, u.x);
        float nx01 = mix(n001, n101, u.x);
        float nx11 = mix(n011, n111, u.x);
        float nxy0 = mix(nx00, nx10, u.y);
        float nxy1 = mix(nx01, nx11, u.y);
        float value = mix(nxy0, nxy1, u.z);

        return clamp(value * 0.5 + 0.5, 0.0, 1.0);
    }

    // ── Beer-Lambert ground transmittance (mirror cloud_noise_ref.h) ───
    // T(d) = exp(-kCloudExtinction * d), with negative density clamped to 0.
    // T(0) == 1, T in (0,1], monotonically non-increasing in density.
    float transmittance(float density) {
        float d = max(density, 0.0);
        return exp(-kCloudExtinction * d);
    }

    // ── Single-scattering phase (mirror cloud_noise_ref.h) ─────────────
    // Henyey-Greenstein:
    //   p(theta) = (1 - g^2) / (4*pi * (1 + g^2 - 2*g*cosTheta)^{3/2})
    float henyey_greenstein(float cosTheta, float g) {
        float g2 = g * g;
        float base = max(1.0 + g2 - 2.0 * g * cosTheta, 1.0e-6);
        float denom = 4.0 * kPi * pow(base, 1.5);
        return (1.0 - g2) / denom;
    }

    // Convenience using the configured cloud anisotropy kCloudPhaseG.
    float cloud_phase(float cosTheta) {
        return henyey_greenstein(cosTheta, kCloudPhaseG);
    }

    // ── Time-driven cloud-motion offset (mirror cloud_noise_ref.h) ─────
    // offset(t) = kCloudWindDir * (kCloudWindSpeed * t). A pure linear
    // function of accumulated time — no per-frame state, so equal time
    // yields an identical offset regardless of frame cadence.
    vec3 cloud_motion_offset(float t) {
        // kCloudWindDir = normalize(vec3(1.0, 0.3, 0.0)), computed inline so
        // the constant matches the reference's glm::normalize result exactly.
        vec3 windDir =
            normalize(vec3(materialParams.cloudMotionTuning.yz, 0.0));
        return windDir * (materialParams.cloudMotionTuning.x * t);
    }

    float jitterHash(vec2 p) {
        vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        p3 += dot(p3, p3.yzx + 33.33);
        return fract((p3.x + p3.y) * p3.z);
    }

    // Cheap continuous value noise for the large-scale horizontal weather
    // map. Unlike a grid of explicit cloud primitives, this has no centres or
    // repeated silhouettes that the eye can identify.
    float hash12(vec2 p) {
        vec3 p3 = fract(vec3(p.xyx) * 0.1031);
        p3 += dot(p3, p3.yzx + 33.33);
        return fract((p3.x + p3.y) * p3.z);
    }

    float value_noise2(vec2 p) {
        vec2 cell = floor(p);
        vec2 f = fract(p);
        vec2 u = f * f * (3.0 - 2.0 * f);
        float a = hash12(cell);
        float b = hash12(cell + vec2(1.0, 0.0));
        float c = hash12(cell + vec2(0.0, 1.0));
        float d = hash12(cell + vec2(1.0, 1.0));
        return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
    }

    float hash13_fast(vec3 p) {
        p = fract(p * 0.1031);
        p += dot(p, p.zyx + 31.32);
        return fract((p.x + p.y) * p.z);
    }

    float value_noise3(vec3 p) {
        vec3 cell = floor(p);
        vec3 f = fract(p);
        vec3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
        float x00 = mix(hash13_fast(cell),
                        hash13_fast(cell + vec3(1.0, 0.0, 0.0)), u.x);
        float x10 = mix(hash13_fast(cell + vec3(0.0, 1.0, 0.0)),
                        hash13_fast(cell + vec3(1.0, 1.0, 0.0)), u.x);
        float x01 = mix(hash13_fast(cell + vec3(0.0, 0.0, 1.0)),
                        hash13_fast(cell + vec3(1.0, 0.0, 1.0)), u.x);
        float x11 = mix(hash13_fast(cell + vec3(0.0, 1.0, 1.0)),
                        hash13_fast(cell + vec3(1.0, 1.0, 1.0)), u.x);
        return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
    }

    float weather_fbm(vec2 p) {
        float n = 0.0;
        n += value_noise2(p) * 0.55;
        p = p * 2.03 + vec2(13.7, 7.1);
        n += value_noise2(p) * 0.29;
        p = p * 2.01 + vec2(8.3, 19.2);
        n += value_noise2(p) * 0.16;
        return n;
    }

    // ── Continuous stratocumulus density field ─────────────────────────

    // A warped 2D weather field lays out broad coherent banks. Its strength
    // controls locally varying cloud base/top heights, while anisotropic 3D
    // noise erodes the volume. There are deliberately no spheres, ellipsoids,
    // or per-cell cloud objects in this representation.
    float cloud_weather(vec3 worldPos, out float heightProfile) {
        float slabHeight = max(materialParams.cloudTop - materialParams.cloudBottom, 1.0);
        float heightFrac = clamp((worldPos.z - materialParams.cloudBottom)
                               / slabHeight, 0.0, 1.0);
        vec2 motion = cloud_motion_offset(materialParams.time).xy;
        vec2 weatherUv = worldPos.xy * (materialParams.noiseScale * 0.55)
                       + motion;

        vec2 warp = vec2(
            value_noise2(weatherUv * 1.31 + vec2(31.4, 17.2)),
            value_noise2(weatherUv * 1.27 + vec2(8.7, 43.1))) - 0.5;
        float weather = weather_fbm(weatherUv + warp * 0.72);

        // Coverage decides where cloud exists, but an independent field drives
        // its vertical development. Coupling top height directly to coverage
        // made every bank rise in the middle and slope down at the outline,
        // reading like an island protruding from a flat ocean.
        float verticalVariation = value_noise2(
            weatherUv * 0.73 + vec2(71.3, 22.8));
        float verticalDetail = value_noise2(
            weatherUv * 4.27 + vec2(24.6, 63.9));
        float verticalFine = value_noise2(
            weatherUv * 9.13 + vec2(49.2, 11.7));
        float threshold = mix(0.68, 0.40,
                              clamp(materialParams.coverage, 0.0, 1.0));
        float localBase = 0.045 + (verticalVariation - 0.5) * 0.08;
        float localTop = 0.68
                       + (verticalVariation - 0.5) * 0.36
                       + (verticalDetail - 0.5) * 0.32
                       + (verticalFine - 0.5) * 0.18;
        localTop = clamp(localTop, 0.38, 0.98);

        float baseFade = smoothstep(localBase, localBase + 0.075, heightFrac);
        float topFade = 1.0 - smoothstep(localTop - 0.095,
                                         localTop, heightFrac);
        heightProfile = max(baseFade * topFade, 0.0);
        return weather - threshold;
    }

    float cloud_density(vec3 worldPos, out float structureShade) {
        float heightProfile;
        float macroField = cloud_weather(worldPos, heightProfile);
        structureShade = 0.5;
        if (heightProfile <= 0.0 || macroField < -0.20) return 0.0;

        float slabHeight = max(materialParams.cloudTop - materialParams.cloudBottom, 1.0);
        float heightFrac = (worldPos.z - materialParams.cloudBottom) / slabHeight;
        vec2 motion = cloud_motion_offset(materialParams.time).xy;

        // Horizontal and vertical frequencies differ so the detail grows
        // through the depth of a bank instead of producing round noise balls.
        vec3 detailPos = vec3(
            worldPos.xy * (materialParams.noiseScale * 4.5) + motion * 4.5,
            heightFrac * 4.2);
        float body = value_noise3(detailPos);
        float erosion = value_noise3(detailPos * 2.31
                                    + vec3(9.2, 17.1, 4.8));
        structureShade = body * 0.67 + erosion * 0.33;

        // Coverage establishes coherent banks; the higher-frequency 3D terms
        // dominate their boundary so the outline breaks into turbulent lobes
        // and wisps instead of retaining a smooth map-contour coastline.
        float field = macroField * 0.82
                    + (body - 0.5) * 0.39
                    + (erosion - 0.5) * 0.17;
        // Keep the density boundary narrow and let ray integration provide
        // translucency. This preserves crisp irregular silhouettes.
        return smoothstep(-0.035, 0.075, field)
             * heightProfile
             * materialParams.densityScale;
    }

    float cloud_density_for_light(vec3 worldPos) {
        float heightProfile;
        float macroField = cloud_weather(worldPos, heightProfile);
        return smoothstep(-0.05, 0.10, macroField)
             * heightProfile
             * materialParams.densityScale;
    }

    float light_transmittance(vec3 worldPos, vec3 sunDir) {
        float slabHeight = max(materialParams.cloudTop - materialParams.cloudBottom, 1.0);
        float stepLength = slabHeight * 0.24;
        float opticalDepth = 0.0;
        for (int i = 1; i <= 2; ++i) {
            vec3 lightPos = worldPos + sunDir * (stepLength * float(i));
            opticalDepth += cloud_density_for_light(lightPos) * stepLength;
        }
        return exp(-opticalDepth * 1.15);
    }

    void effectShade(inout MaterialInputs material) {
        prepareMaterial(material);

        // ── Cloud appearance knobs (tweak freely; not part of the CPU
        // mirror / property tests) ─────────────────────────────────────
        // kSunGain compensates for the single-scattering model: real clouds
        // are lit largely by multiple scattering, so a sunlit face reads much
        // brighter than a single Beer-Lambert bounce predicts. kAmbientBase /
        // kAmbientTop sky-fill the shadowed base vs the bright tops so the
        // deck has vertical volume instead of a flat grey wash.
        const float kSunGain     = 2.8;
        const float kAmbientBase = 0.48;
        const float kAmbientTop  = 0.96;

        float2 uv = getUV0();

        // ── Linear scene-depth sample (LOCKED Y-convention) ─────

        // Sample the linear scene-depth texture (F, eye-space forward
        // distance in metres) produced by the depth pre-pass / heaven_depth
        // proxy render. The offscreen RT is Y-flipped versus the quad UV
        // (Vulkan top-down framebuffer), the same convention as heaven_blit /
        // godrays / volfog / heat haze / lens flare (scene-depth-pipeline

        float2 depthUV = uvToRenderTargetUV(uv);
        float sceneDist = texture(materialParams_depthTexture, depthUV).r;

        // Graceful fallback: a null/unbound depth texture reads a
        // non-positive value. Treat that as "far / no geometry" so the cloud
        // layer renders unoccluded (rather than fully occluded) before the
        // depth pre-pass produces a valid texture.
        if (sceneDist <= 0.0) sceneDist = 2000.0;

        // View ray from the camera toward this fragment. The DIRECTION is taken
        // from two camera-shifted quantities (getWorldPosition() and
        // getWorldCameraPosition()), so it is correct regardless of Filament's
        // world-space shift. The march ORIGIN stays the CPU true-world cameraPos
        // so the cloud slab Z test (cloudBottom/cloudTop, true world) is
        // consistent. (Originally this used getWorldPosition() - cameraPos,
        // mixing shifted and true world — a direction error that grew as the
        // camera moved away from the world origin.)
        float3 rayStart = materialParams.cameraPos;
        float3 rayDir   = normalize(getWorldPosition().xyz - getWorldCameraPosition());

        // `sceneDist` is the eye-space FORWARD distance to the nearest opaque
        // surface; convert it to a length along this (possibly off-axis) view
        // ray via the camera-forward cosine, so the cloud march can be capped
        // exactly where solid geometry occludes it.
        float3 camFwd      = -normalize(frameUniforms.worldFromViewMatrix[2].xyz);
        float  cosTheta    = max(dot(rayDir, camFwd), 1e-3);
        float  sceneRayLen = sceneDist / cosTheta;

        // Open sky / no-geometry pixels read the far clear value (~2000 m) —
        // march the full cloud slab there, bounded by maxMarchDistance so a
        // near-horizontal ray (which would otherwise traverse the slab for
        // many kilometres) keeps a small step length (no grazing-ray
        // undersampling / horizon aliasing). Pixels covered by nearer opaque
        // geometry cap the march at the geometry distance, so cloud samples
        // BEHIND that geometry never accumulate — the clouds are occluded

        bool isSky = sceneDist >= materialParams.cloudDensityTuning.x;
        float maxDist = isSky ? materialParams.maxMarchDistance
                              : min(sceneRayLen, materialParams.maxMarchDistance);

        // ── Intersect the view ray with the horizontal cloud slab ──────
        // World is Z-up, so the slab is bounded by z in [cloudBottom, cloudTop].
        float bottom = materialParams.cloudBottom;
        float top    = materialParams.cloudTop;

        // Solve rayStart.z + t*rayDir.z = {bottom, top} for the two slab planes.
        float tEnter = 0.0;
        float tExit  = 0.0;
        if (abs(rayDir.z) < 1e-5) {
            // Ray parallel to the slab: inside only if the camera Z is within.
            if (rayStart.z < bottom || rayStart.z > top) {
                material.baseColor = float4(0.0, 0.0, 0.0, 0.0);
                return;
            }
            tEnter = 0.0;
            tExit  = maxDist;
        } else {
            float t0 = (bottom - rayStart.z) / rayDir.z;
            float t1 = (top - rayStart.z) / rayDir.z;
            tEnter = min(t0, t1);
            tExit  = max(t0, t1);
        }

        // Clip the slab span to the visible ray segment [0, maxDist].
        tEnter = max(tEnter, 0.0);
        tExit  = min(tExit, maxDist);
        if (tExit <= tEnter) {
            material.baseColor = float4(0.0, 0.0, 0.0, 0.0);
            return;
        }

        // Fade rays before marching once the layer is too distant to resolve.
        // This also avoids spending extra samples on an invisible horizon.
        float fadeStart =
            materialParams.cloudDensityTuning.y *
            materialParams.maxMarchDistance;
        float fadeEnd =
            materialParams.cloudDensityTuning.z *
            materialParams.maxMarchDistance;
        float horizonFade = 1.0 - smoothstep(fadeStart, fadeEnd, tEnter);
        if (horizonFade <= 0.002) {
            material.baseColor = float4(0.0, 0.0, 0.0, 0.0);
            return;
        }

        // ── Raymarch the slab, accumulating density + scattering ───────
        // Quality supplies the baseline. Grazing rays can cross kilometres of
        // cloud, so enforce a maximum world-space step length as well; without
        // it, each sample becomes a visible vertical block regardless of how
        // smooth the density function is.
        int distanceSteps = int(
            ceil((tExit - tEnter) / materialParams.cloudDensityTuning.w));
        int numSteps = min(max(max(materialParams.numSteps, 1), distanceSteps), 128);
        float segLen = (tExit - tEnter) / float(numSteps);

        float jitter = fract(jitterHash(uv * 1024.0)
                           + materialParams.temporalJitter);

        // Phase toward the sun for this view ray (sunDir points scene->sun).
        // Looking toward the sun is the forward-scattering case: rayDir and
        // sunDir point the same way. The previous sign was reversed, putting
        // the silver lining on the anti-solar side.
        float cosThetaSun = dot(rayDir, materialParams.sunDir);
        float phase =
            mix(henyey_greenstein(cosThetaSun,
                                  materialParams.cloudLightingTuning.x),
                henyey_greenstein(cosThetaSun,
                                  materialParams.cloudLightingTuning.y),
                materialParams.cloudLightingTuning.z);
        phase += materialParams.cloudLightingTuning.w;

        float opticalDepth = 0.0;        // accumulated density * length
        float trans = 1.0;
#if defined(HEAVEN_TEMPORAL_EFFECT)
        float3 moment = float3(0.0);
        float momentWeight = 0.0;
#endif               // running view-ray transmittance
        float3 scattered = float3(0.0);  // accumulated in-scattered light

        for (int i = 0; i < numSteps; ++i) {
            float t = tEnter + (float(i) + jitter) * segLen;
            float3 samplePos = rayStart + rayDir * t;

            float structureShade;
            float density = cloud_density(samplePos, structureShade);
            if (density > 0.0) {
                float segOpticalDepth = density * segLen;
                opticalDepth += segOpticalDepth;

                // Shadow along the actual sample-to-sun ray. View-ray optical
                // depth cannot represent cloud self-shadowing because it
                // follows a different direction except at the solar disc.
                float sunTrans = light_transmittance(samplePos,
                                                     normalize(materialParams.sunDir));

                // Ambient sky fill, brighter on the cloud tops and dimmer in
                // the shadowed base so the deck has volume. heightFrac is 0 at
                // the slab bottom, 1 at the top.
                float heightFrac = clamp((samplePos.z - bottom) / max(top - bottom, 1e-3),
                                         0.0, 1.0);
                float topLight = smoothstep(0.38, 0.92, heightFrac);
                float powder = 1.0 - exp(-segOpticalDepth * 2.0);
                float coreShadow = mix(0.67, 1.0, topLight);

                // Multiple scattering keeps most of a real cloud pearl-white.
                // Sunlit regions lean warm, while shadowed pockets retain a
                // cooler sky tint, producing shade variation without making
                // the entire underside a uniform dark-grey mass.
                // The CPU supplies sun/moon-scaled radiance. Adding a fixed
                // white tint here would make clouds self-illuminate at night.
                float3 sunLight = materialParams.sunColor
                                * (phase * sunTrans * kSunGain)
                                * mix(0.72, 1.18, powder);

                float lightVariation = mix(0.72, 1.08,
                                           smoothstep(0.14, 0.94, sunTrans));
                float structureVariation = mix(0.82, 1.10,
                                                structureShade);
                float3 ambient = materialParams.skyColor
                               * mix(kAmbientBase, kAmbientTop, heightFrac)
                               * coreShadow
                               * lightVariation
                               * structureVariation;

                // Exact homogeneous-segment integration. Using optical depth
                // directly over-brightens long/grazing steps; segmentAlpha is
                // bounded and remains stable across quality step counts.
                float segmentAlpha = 1.0 - transmittance(segOpticalDepth);
                #if defined(HEAVEN_TEMPORAL_EFFECT)
                float weight = segmentAlpha * trans;
                moment += samplePos * weight;
                momentWeight += weight;
#endif
                scattered += (sunLight + ambient) * (segmentAlpha * trans);
                trans *= 1.0 - segmentAlpha;
            }

            if (trans < 1e-3) {
                break;
            }
        }

        // Opacity is one minus the surviving view-ray transmittance. Above
        // kCloudDensityThreshold of accumulated density the ground beneath
        // reads measurably darker (handled by the feature's ground-light
        // term); here we composite the cloud itself.
        float opacity = 1.0 - trans;

        // Filament's transparent mode uses premultiplied blending (ONE,
        // ONE_MINUS_SRC_ALPHA), matching the ray integration above.
        if (opacity < 0.004 || opticalDepth < 0.004) {
            material.baseColor = float4(0.0, 0.0, 0.0, 0.0);
            return;
        }

        // Horizon fade: dissolve the layer as the slab-entry distance grows
        // toward the march cap. Without this, a flat slab viewed from below
        // shows a hard razor seam exactly at the horizon (clouds for every ray
        // above horizontal, nothing below). Fading the contribution to zero as
        // the entry point approaches maxMarchDistance turns that knife edge
        // into a soft band that recedes into the distance like a real overcast
        // layer. tEnter is the distance at which this ray first reaches the
        // slab, so distant (grazing) rays fade out, overhead rays stay solid.
        opacity *= horizonFade;
        scattered *= horizonFade;

        #if defined(HEAVEN_TEMPORAL_EFFECT)
        effectTemporalWorld = moment / max(momentWeight, 1e-8);
#endif
        material.baseColor = float4(scattered, opacity);
    }
