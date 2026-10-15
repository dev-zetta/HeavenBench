// One SDK depth/vector cannot represent every transparent layer. This target
// deliberately retains the nearest dominant contributor, with signed
// coverage kept opaque (negative coverage must never enter alpha blending).
void temporalEffectFragment(inout MaterialInputs material) {
    effectShade(material);
    float coverage = clamp(material.baseColor.a, 0.0, 1.0);
#if defined(HEAVEN_TEMPORAL_ADDITIVE)
    coverage = clamp(max(max(material.baseColor.r, material.baseColor.g), material.baseColor.b), 0.0, 1.0);
#endif
    // Match the final resolver's eligibility before depth testing. A thin
    // foreground layer must not hide a dominant contributor behind it.
    if (coverage < 0.5) discard;
    float4 current = variable_currentClip;
    float4 previous = variable_previousClip;
#if defined(HEAVEN_TEMPORAL_CLOUD_PLANE) || defined(HEAVEN_TEMPORAL_CLOUD_VOLUME)
    // The source cloud law samples a camera-relative ray in its IBL-oriented
    // world. Convert the same intersection/extinction centroid into the user's
    // world before projection; no fullscreen-quad depth is exported.
    float3x3 currentWorldToUser = float3x3(getUserWorldFromWorldMatrix());
    float3 relative = effectTemporalWorld - materialParams.cameraPos;
#if defined(HEAVEN_TEMPORAL_CLOUD_VOLUME)
    float3 direction = normalize(float3(materialParams.cloudMotionTuning.yz, 0.0));
    float3 advection = direction * materialParams.cloudMotionTuning.x *
        (materialParams.time - materialParams.previousTime) / max(materialParams.noiseScale, 1e-8);
#endif
    float3 currentWorld = materialParams.cameraPos + currentWorldToUser * relative;
    // Plane scrolling combines distinct texture speeds and a fixed mask;
    // its representative geometry is fixed, not a claimed texture inverse.
    float3 previousWorld = currentWorld;
#if defined(HEAVEN_TEMPORAL_CLOUD_VOLUME)
    previousWorld = materialParams.previousCamera + materialParams.previousWorldToUser *
        (effectTemporalWorld + advection - materialParams.previousCamera);
#endif
    current = materialParams.currentClipFromUserWorld * float4(currentWorld, 1.0);
    previous = materialParams.previousClipFromUserWorld * float4(previousWorld, 1.0);
#endif
    bool valid = materialParams.historyValid && current.w > 0.0 && previous.w > 0.0;
    if (current.w <= 0.0) discard;
    float2 currentUv = current.xy / current.w * float2(0.5, -0.5) + 0.5;
    float2 previousUv = previous.xy / max(previous.w, 1e-8) * float2(0.5, -0.5) + 0.5;
    float depth = clamp(0.5 - 0.5 * current.z / current.w, 0.0, 1.0);
    gl_FragDepth = depth;
    material.baseColor = float4(valid ? previousUv - currentUv : float2(0.0),
        depth, valid ? coverage : -coverage);
}
