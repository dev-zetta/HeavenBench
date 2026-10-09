/* HeavenBench — startup sampling of an already uploaded height texture. */
#ifndef HEAVENBENCH_SRC_RHI_HEIGHT_SAMPLE_BATCH_H_
#define HEAVENBENCH_SRC_RHI_HEIGHT_SAMPLE_BATCH_H_

#include "engine/types.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace filament { class Engine; class Texture; }

namespace heaven {

struct HeightSampleQuery {
    std::uint64_t id;
    std::array<float, 2> uv;
    // Maximum incident raw UV edge extents, before baseTransform.
    std::array<float, 2> footprint;
};

struct HeightSampleValue {
    std::uint64_t id;
    float height;
    float mesh_lod;
    float selected_lod;
};

struct HeightSampleBatchOptions {
    std::uint32_t max_queries_per_chunk = 16'384; // 1..65,536, width <=256
    std::uint32_t max_total_queries = 1'048'576;
    // One completion deadline for the entire sample() call, not per chunk.
    std::chrono::milliseconds completion_timeout{5'000};
};

// Engine/main-thread use only, outside any other beginFrame/endFrame scope.
// Engine must outlive the batch. No texture, mesh, scene or asset is owned or
// looked up here. The package span must contain heaven_height_sample.
class HeightSampleBatch final {
public:
    [[nodiscard]] static Result<std::unique_ptr<HeightSampleBatch>> create(
        filament::Engine& engine, std::span<const std::uint8_t> package,
        HeightSampleBatchOptions options = {});
    ~HeightSampleBatch();

    HeightSampleBatch(const HeightSampleBatch&) = delete;
    HeightSampleBatch& operator=(const HeightSampleBatch&) = delete;

    // The caller resolves an enabled geometry descriptor with positive fade
    // power, and keeps this actual selected texture alive for the call. This
    // API implements the textureLod vertex route only; manual window sampling
    // is ineligible until a matching sampler route exists.
    // IDs can be arbitrary uint64 values but must be unique. Result order is
    // input order; float readback carries local slots, never user IDs.
    [[nodiscard]] Result<std::vector<HeightSampleValue>> sample(
        filament::Texture* height, std::span<const HeightSampleQuery> queries,
        std::array<float, 4> transform);

private:
    struct Impl;
    explicit HeightSampleBatch(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven
#endif
