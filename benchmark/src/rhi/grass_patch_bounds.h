#ifndef HEAVENBENCH_SRC_RHI_GRASS_PATCH_BOUNDS_H_
#define HEAVENBENCH_SRC_RHI_GRASS_PATCH_BOUNDS_H_

#include "engine/grass_bounds.h"
#include "rhi/filament_bounds.h"

#include <filament/RenderableManager.h>
#include <utils/Entity.h>

#include <cstdio>
#include <cstdlib>

namespace heaven {

// Filament adapter around the GPU-independent geometry/wind bounds cache.
struct GrassPatchBounds : GrassBoundsCache {
    [[nodiscard]] filament::Box box() const {
        if (!local) return uncullable_box();
        return box_from_min_max(
            {local->min.x, local->min.y, local->min.z},
            {local->max.x, local->max.y, local->max.z});
    }

    void apply(filament::RenderableManager& manager, utils::Entity entity) const {
        if (const auto instance = manager.getInstance(entity)) {
            manager.setAxisAlignedBoundingBox(instance, box());
            manager.setCulling(instance, local.has_value());
        }
    }

    void log(const char* category, size_t instance_index, size_t index_count,
             size_t owner_vertex_count, size_t index_offset) const {
        if (!std::getenv("HEAVEN_PROFILE_BOUNDS")) return;
        if (!local) {
            std::printf("  Foliage bounds: %s instance=%zu draw_indices=%zu "
                        "owner_vertices=%zu index_offset=%zu culling=off\n",
                        category, instance_index, index_count, owner_vertex_count, index_offset);
            return;
        }
        std::printf("  Foliage bounds: %s instance=%zu draw_indices=%zu "
                    "owner_vertices=%zu index_offset=%zu "
                    "min=(%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f) "
                    "previous=(-200,-200,-200)..(200,200,200)\n",
                    category, instance_index, index_count, owner_vertex_count, index_offset,
                    local->min.x, local->min.y, local->min.z,
                    local->max.x, local->max.y, local->max.z);
    }
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_GRASS_PATCH_BOUNDS_H_
