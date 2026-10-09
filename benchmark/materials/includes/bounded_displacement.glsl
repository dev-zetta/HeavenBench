// Reconstruct the immutable source raster chord before bounded smoothing.
// CUSTOM1..3: original neutral corner / barycentric weight; CUSTOM4..6:
// actual packed-normal native-height displacement; CUSTOM7: world-metre
// correction / amplitude epoch. Static proper-uniform instances only.
    highp float parentChordFade(highp float4 rendererWorldPosition) {
        highp float3 userWorldPosition =
                (getUserWorldFromWorldMatrix() * rendererWorldPosition).xyz;
        highp float distance = length(userWorldPosition - getMaterialGlobal1().xyz);
        highp float normalizedDistance = clamp(
                (distance - materialParams.displacementParams.z)
                * materialParams.displacementParams.w, 0.0, 8.0);
        return pow(saturate(1.0 - normalizedDistance), materialParams.displacementParams.y);
    }

    void applyParentChord(inout MaterialVertexInputs material) {
        highp float4 source0 = getCustom1();
        highp float4 source1 = getCustom2();
        highp float4 source2 = getCustom3();
        highp float3 barycentric = float3(source0.w, source1.w, source2.w);
        highp float4x4 worldFromModel = getWorldFromModelMatrix();
        highp float4 parent0 = worldFromModel * float4(source0.xyz, 1.0);
        highp float4 parent1 = worldFromModel * float4(source1.xyz, 1.0);
        highp float4 parent2 = worldFromModel * float4(source2.xyz, 1.0);
        highp float3 parentFade = float3(parentChordFade(parent0),
                                       parentChordFade(parent1), parentChordFade(parent2));
        // Current displacement is in world metres. Remove uniform model scale
        // from these baked direction vectors, while retaining it for positions.
        highp float3x3 rotation = float3x3(normalize(worldFromModel[0].xyz),
                                         normalize(worldFromModel[1].xyz),
                                         normalize(worldFromModel[2].xyz));
        highp float3 sourceDisplacement =
                barycentric.x * parentFade.x * (rotation * getCustom4().xyz)
              + barycentric.y * parentFade.y * (rotation * getCustom5().xyz)
              + barycentric.z * parentFade.z * (rotation * getCustom6().xyz);
        highp float amplitude = materialParams.displacementParams.x;
        highp float4 fairing = getCustom7();
        highp float3 smoothing = (rotation * fairing.xyz) * (amplitude / fairing.w);
        highp float smoothingLength = length(smoothing);
        // Eligible instances retain their positive constant homogeneous W.
        // Bound physical movement after the same divide used by rasterization.
        highp float smoothingBound = 0.020 * min(1.0, material.worldPosition.w);
        if (smoothingLength > smoothingBound) {
            smoothing *= smoothingBound / smoothingLength;
        }
        // Only the bounded smoothing is clamped. The original chord is kept
        // exactly as an algebraic interpolation of its displaced parent corners.
        smoothing *= dot(barycentric, parentFade);
        material.worldPosition.xyz += amplitude * sourceDisplacement + smoothing;
    }
