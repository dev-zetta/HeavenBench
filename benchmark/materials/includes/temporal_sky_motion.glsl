// A sky ray has no parallax. Homogeneous near/far subtraction removes camera
// translation without dividing by the infinite-far point's zero W.
float2 temporalSkyMotion(float2 currentUv, float4x4 worldFromCurrentClip,
        float4x4 previousClipFromWorld, bool historyValid) {
    if (!historyValid) return float2(0.0);
    float2 ndc = currentUv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 nearWorld = worldFromCurrentClip * float4(ndc, -1.0, 1.0);
    float4 farWorld = worldFromCurrentClip * float4(ndc, 1.0, 1.0);
    float3 direction = farWorld.xyz * nearWorld.w - nearWorld.xyz * farWorld.w;
    float4 previous = previousClipFromWorld * float4(direction, 0.0);
    if (!(previous.w > 1e-8) || !all(lessThan(abs(previous), float4(1e30))))
        return float2(0.0);
    float2 previousUv = previous.xy / previous.w * float2(0.5, -0.5) + 0.5;
    return previousUv - currentUv;
}

float2 temporalDepthCameraMotion(float2 currentUv, float depth,
        float4x4 worldFromCurrentClip, float4x4 previousClipFromWorld, bool historyValid) {
    if (!historyValid) return float2(0.0);
    float2 ndc = currentUv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 world = worldFromCurrentClip * float4(ndc, 1.0-2.0*depth, 1.0);
    float4 previous = previousClipFromWorld * world;
    if (!(previous.w > 1e-8) || !all(lessThan(abs(previous), float4(1e30))))
        return float2(0.0);
    return previous.xy / previous.w * float2(0.5, -0.5) + 0.5 - currentUv;
}
