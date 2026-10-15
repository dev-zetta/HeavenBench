void effectVertex(inout MaterialVertexInputs material, float animationTime, float3 cameraRight, float3 cameraUp, float3 shadingCamera, float3 absoluteCamera) {
        // The input quad has clip Z=0, so Filament reconstructs a finite
        // world position for its view ray. Rasterize it at the far plane:
        // device-domain GL Z=1 becomes reverse-Z=0 on every backend.
        material.clipSpaceTransform[3].z = 1.0;
    }
