// No-op vertex stage. The CPU positions the quad in world space (same as
    // godrays / volfog); we just need UV0 to flow through to the fragment.
    void effectVertex(inout MaterialVertexInputs material, float animationTime, float3 cameraRight, float3 cameraUp, float3 shadingCamera, float3 absoluteCamera) {}
