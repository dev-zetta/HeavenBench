void effectShade(inout MaterialInputs material) {
        prepareMaterial(material);

        // Both operands use Filament's camera-shifted world coordinates. The
        // true-world camera uniform remains the ray origin for the plane test.
        highp vec3 camPos = materialParams.cameraPos;
        highp vec3 dir = normalize(getWorldPosition().xyz - getWorldCameraPosition());

        // The cloud deck is a horizontal plane at a fixed world Z
        // (Z-up). In the original Heaven scene the cloud/sky volume sits
        // at Z = -1000, well below the floating islands, so the camera —
        // flying among the islands — looks *down* onto it. The plane may
        // be above or below the camera, so we intersect in both
        // directions and reject only rays that run parallel to it.
        float cloudH = materialParams.cloudHeight;
        if (abs(dir.z) <= 0.01) {
            material.baseColor = float4(0.0);
            return;
        }

        // Ray-plane intersection at the cloud height. A positive t means
        // the plane is in front of the view ray; negative means behind.
        float t = (cloudH - camPos.z) / dir.z;
        if (t < 0.0 || t > 20000.0) {
            material.baseColor = float4(0.0);
            return;
        }

        highp vec3 cloudPos = camPos + dir * t;
#if defined(HEAVEN_TEMPORAL_EFFECT)
        effectTemporalWorld = cloudPos;
#endif

        // Cloud UVs with scrolling animation
        float time = materialParams.time;
        float scale = materialParams.cloudMotionTuning.z;
        vec2 uv1 = cloudPos.xy * scale +
            vec2(time * materialParams.cloudMotionTuning.x,
                 time * materialParams.cloudMotionTuning.y);
        vec2 uv2 = cloudPos.xy * scale * materialParams.cloudDetailTuning.x +
            vec2(-time * scale,
                 time * materialParams.cloudDetailTuning.y);
        vec2 uvMask = cloudPos.xy * scale * 0.5;

        float cloud1 = texture(materialParams_cloudTex1, uv1).r;
        float cloud2 = texture(materialParams_cloudTex2, uv2).r;
        float mask = texture(materialParams_cloudMask, uvMask).r;

        // Combine cloud layers
        float density = cloud1 * cloud2 * mask;
        density = smoothstep(materialParams.cloudShapeTuning.y,
                             materialParams.cloudShapeTuning.x, density);

        // Fade toward the horizon. The deck is seen at grazing angles
        // where |dir.z| -> 0; clamp on the absolute vertical component so
        // it works whether the camera looks up or down at the plane.
        float horizonFade = smoothstep(
            materialParams.cloudDetailTuning.z,
            materialParams.cloudShapeTuning.z, abs(dir.z));
        density *= horizonFade;

        // Both colors carry the actual sun/moon illumination. A fixed white
        // term would make this unlit layer self-illuminate after sunset.
        vec3 sunDir = normalize(materialParams.sunDir);
        float sunAngle = max(dot(dir, sunDir), 0.0);
        vec3 cloudColor = mix(materialParams.skyColor,
                              materialParams.sunColor * 0.8,
                              pow(sunAngle, 3.0));

        // Slight darkening on the underside
        cloudColor *= materialParams.cloudShapeTuning.w +
                      (1.0 - materialParams.cloudShapeTuning.w) *
                          smoothstep(
                              materialParams.cloudDensityLightingTuning.x,
                              materialParams.cloudDensityLightingTuning.y,
                              density);

        // Premultiplied alpha: `blending:transparent` uses source-over and
        // assumes baseColor.rgb is already multiplied by alpha (Filament docs,
        // "Pre-multiplied alpha"). Multiply rgb by the same alpha here.
        float cloudAlpha = density * materialParams.cloudMotionTuning.w;
        material.baseColor = float4(cloudColor * cloudAlpha, cloudAlpha);
    }
