// Evaluate the same persistent effect geometry at the current and last
// accepted source times. Never connect a recycled particle/rain slot.
void temporalEffectVertex(inout MaterialVertexInputs material) {
#if defined(HEAVEN_TEMPORAL_CLOUD_PLANE) || defined(HEAVEN_TEMPORAL_CLOUD_VOLUME)
    effectVertex(material, materialParams.time,
        frameUniforms.worldFromViewMatrix[0].xyz,
        frameUniforms.worldFromViewMatrix[1].xyz,
        getWorldCameraPosition(), materialParams.cameraPos);
    material.currentClip = float4(0.0);
    material.previousClip = float4(0.0);
#else
    MaterialVertexInputs previous = material;
    float4x4 userFromWorld = getUserWorldFromWorldMatrix();
    float3 currentCamera = (userFromWorld * float4(getWorldCameraPosition(), 1.0)).xyz;
    float animationTime = 0.0;
#if !defined(HEAVEN_TEMPORAL_LENS_FLARE)
    animationTime = materialParams.time;
#endif
    effectVertex(material, animationTime,
        frameUniforms.worldFromViewMatrix[0].xyz,
        frameUniforms.worldFromViewMatrix[1].xyz,
        getWorldCameraPosition(), currentCamera);
    float4 currentWorld = userFromWorld * material.worldPosition;

    // Translation cancels in the effect's placement law. Evaluate the prior
    // shader in its accepted IBL-oriented world, then rotate into user world.
    // This preserves fixed world-axis wander and rain's absolute lattice even
    // when Filament's camera origin or IBL rotation changed since acceptance.
    float3x3 previousUserToWorld = transpose(materialParams.previousWorldToUser);
    float4 previousAnchor = materialParams.previousModel * float4(getRawPosition(), 1.0);
    previous.worldPosition = float4(previousUserToWorld * previousAnchor.xyz, previousAnchor.w);
    effectVertex(previous, materialParams.previousTime,
        previousUserToWorld * materialParams.previousRight,
        previousUserToWorld * materialParams.previousUp,
        previousUserToWorld * materialParams.previousCamera,
        materialParams.previousCamera);
    float4 previousWorld = float4(materialParams.previousWorldToUser * previous.worldPosition.xyz,
        previous.worldPosition.w);
    material.currentClip = materialParams.currentClipFromUserWorld * currentWorld;
    material.previousClip = materialParams.previousClipFromUserWorld * previousWorld;

    bool corresponding = materialParams.historyValid;
#if defined(HEAVEN_TEMPORAL_CYCLE)
    float cycle = getCustom1().x + materialParams.spawnInterval;
    float phase = getCustom0().x * materialParams.spawnInterval;
    corresponding = corresponding && cycle > 0.0 &&
        floor((animationTime - phase) / cycle) ==
        floor((materialParams.previousTime - phase) / cycle) &&
        mod(materialParams.previousTime - phase, cycle) <= getCustom1().x;
#endif
#if defined(HEAVEN_TEMPORAL_RAIN)
    // Match the authoritative world-anchored lattice and fall-cycle identity.
    // A camera-volume wrap changes the cell; a fall wrap changes the cycle.
    float4 currentCell = effectRainCell(getCustom0().xyz, currentCamera,
        materialParams.volumeExtent, materialParams.fallSpeed);
    float4 previousCell = effectRainCell(getCustom0().xyz, materialParams.previousCamera,
        materialParams.volumeExtent, materialParams.fallSpeed);
    corresponding = corresponding && all(equal(currentCell.xy, previousCell.xy)) &&
        floor((animationTime - currentCell.w) / currentCell.z) ==
        floor((materialParams.previousTime - previousCell.w) / previousCell.z);
#endif
    if (!corresponding || material.previousClip.w <= 0.0)
        material.previousClip.w = -max(abs(material.previousClip.w), 1e-8);
#endif
}
