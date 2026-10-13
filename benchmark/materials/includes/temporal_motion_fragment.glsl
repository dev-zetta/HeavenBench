float4 temporalMotionFragment() {
    float4 current = variable_currentClip, previous = variable_previousClip;
    bool valid = materialParams.historyValid && current.w > 0.0 && previous.w > 0.0;
    float2 currentUv = current.xy / max(current.w, 1e-8) * float2(0.5, -0.5) + 0.5;
    float2 previousUv = previous.xy / max(previous.w, 1e-8) * float2(0.5, -0.5) + 0.5;
    float depth = saturate(0.5 - 0.5 * current.z / max(current.w, 1e-8));
    return float4(valid ? previousUv - currentUv : float2(0.0), depth, valid ? 1.0 : 0.0);
}
