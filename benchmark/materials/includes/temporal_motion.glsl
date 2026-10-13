// Temporal inputs use the same uploaded vertices, UV footprint and bounded
// parent-chord reconstruction as visible/depth geometry. Both evaluations are
// in the user's world coordinates, independent of Filament's camera origin.
float motionFade(float3 world, float3 camera) {
    float d = clamp((length(world - camera) - materialParams.displacementParams.z)
            * materialParams.displacementParams.w, 0.0, 8.0);
    return pow(saturate(1.0 - d), materialParams.displacementParams.y);
}

float4 motionDisplace(float4 world, float3 normal, float4x4 model, float2 sourceUv,
        float3 camera) {
    if (!materialParams.geometryDisplacement) return world;
    if (getCustom7().w > 0.0) {
        float4 a = getCustom1(), b = getCustom2(), c = getCustom3();
        float3 bary = float3(a.w, b.w, c.w);
        float4 pa = model * float4(a.xyz, 1.0);
        float4 pb = model * float4(b.xyz, 1.0);
        float4 pc = model * float4(c.xyz, 1.0);
        float3 fades = float3(motionFade(pa.xyz, camera), motionFade(pb.xyz, camera),
                motionFade(pc.xyz, camera));
        float3x3 rotation = float3x3(normalize(model[0].xyz), normalize(model[1].xyz),
                normalize(model[2].xyz));
        float3 displacement = bary.x * fades.x * (rotation * getCustom4().xyz)
                + bary.y * fades.y * (rotation * getCustom5().xyz)
                + bary.z * fades.z * (rotation * getCustom6().xyz);
        float4 fairing = getCustom7();
        float amplitude = materialParams.displacementParams.x;
        float3 smoothing = (rotation * fairing.xyz) * (amplitude / fairing.w);
        float smoothingLength = length(smoothing);
        // Preserve the authored positive constant homogeneous W, matching
        // applyParentChord's physical-space correction bound exactly.
        float smoothingBound = 0.020 * min(1.0, world.w);
        if (smoothingLength > smoothingBound) smoothing *= smoothingBound / smoothingLength;
        world.xyz += amplitude * displacement + smoothing * dot(bary, fades);
        return world;
    }
    float distanceLod = clamp((length(world.xyz - camera) - materialParams.displacementParams.z)
            * materialParams.displacementParams.w, 0.0, 8.0);
    float2 uv = sourceUv * materialParams.baseTransform.xy + materialParams.baseTransform.zw;
    float2 footprint = getCustom0().yz * abs(materialParams.baseTransform.xy)
            * float2(textureSize(materialParams_geometryHeightMap, 0));
    float meshLod = log2(max(max(footprint.x, footprint.y), 1.0));
    float height = textureLod(materialParams_geometryHeightMap, uv, max(distanceLod, meshLod)).r;
    float offset = (height - 127.0 / 255.0) * materialParams.displacementParams.x
            * motionFade(world.xyz, camera) * getCustom0().x;
    world.xyz += normalize(normal) * offset;
    return world;
}

float4x4 previousBone(uint index) {
    float4 a = texelFetch(materialParams_previousBones, int2(0, int(index)), 0);
    float4 b = texelFetch(materialParams_previousBones, int2(1, int(index)), 0);
    float4 c = texelFetch(materialParams_previousBones, int2(2, int(index)), 0);
    return transpose(float4x4(a, b, c, float4(0.0, 0.0, 0.0, 1.0)));
}

void temporalMotionVertex(inout MaterialVertexInputs material) {
    float4x4 userFromWorld = getUserWorldFromWorldMatrix();
    float4x4 currentModel = userFromWorld * getWorldFromModelMatrix();
    float4 currentWorld = userFromWorld * material.worldPosition;
    float3 currentNormal = float3x3(userFromWorld) * material.worldNormal;
    float3 previousLocal = getRawPosition();
    float3 previousNormal = getRawNormal();
    if (materialParams.hasPreviousPositions) {
        int vertex = getVertexIndex();
        int width = materialParams.previousPositionWidth;
        previousLocal = texelFetch(materialParams_previousPositions,
                int2(vertex % width, vertex / width), 0).xyz;
    } else if (materialParams.hasPreviousBones) {
        uvec4 indices = getBoneIndices();
        float4 weights = getBoneWeights();
        float4x4 skin = previousBone(indices.x) * weights.x
                + previousBone(indices.y) * weights.y
                + previousBone(indices.z) * weights.z
                + previousBone(indices.w) * weights.w;
        previousLocal = (skin * float4(previousLocal, 1.0)).xyz;
        previousNormal = transpose(inverse(float3x3(skin))) * previousNormal;
    }
    float4 previousWorld = materialParams.previousModel * float4(previousLocal, 1.0);
    previousNormal = transpose(inverse(float3x3(materialParams.previousModel))) * previousNormal;
    float3 currentCamera = (userFromWorld * float4(getWorldCameraPosition(), 1.0)).xyz;
    currentWorld = motionDisplace(currentWorld, currentNormal, currentModel, material.uv0,
            currentCamera);
    previousWorld = motionDisplace(previousWorld, previousNormal, materialParams.previousModel,
            material.uv0, materialParams.previousCamera);
    material.worldPosition = inverse(userFromWorld) * currentWorld;
    material.currentClip = materialParams.currentClipFromUserWorld * currentWorld;
    material.previousClip = materialParams.previousClipFromUserWorld * previousWorld;
}
