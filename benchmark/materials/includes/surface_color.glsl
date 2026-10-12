// Heaven's unmarked diffuse maps use the original engine's gamma-2 color
// convention. Overlay/multiply detail operates on encoded texels, including
// its neutral 0.5, before the final RGB is squared. Alpha is never decoded.
// Flags: 1 = legacy base, 2 = legacy detail, 4 = hardware-decoded sRGB detail.
float3 composeSurfaceColor(float3 base, float3 detail, float blend,
                          bool hasDetail, int mode, int flags) {
    bool legacyBlend = (flags & 1) != 0 || (hasDetail && (flags & 2) != 0);
    // Mixed bindings enter the same gamma-2 composition domain. Signed/data
    // detail stays a raw relative offset; only decoded color detail is encoded.
    if (legacyBlend && (flags & 1) == 0) {
        base = sqrt(max(base, float3(0.0)));
    }
    if (hasDetail) {
        if (legacyBlend && (flags & 4) != 0) {
            detail = sqrt(max(detail, float3(0.0)));
        }
        if (mode <= 2) {
            base = clamp(base + (detail * 2.0 - 1.0) * blend, 0.0, 1.0);
        } else {
            base *= clamp(detail + 1.0 - blend, 0.0, 1.0);
        }
    }
    return legacyBlend ? base * base : base;
}
