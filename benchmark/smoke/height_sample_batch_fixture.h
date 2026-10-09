#ifndef HEAVENBENCH_SMOKE_HEIGHT_SAMPLE_BATCH_FIXTURE_H_
#define HEAVENBENCH_SMOKE_HEIGHT_SAMPLE_BATCH_FIXTURE_H_
#include "engine/types.h"
#include <cstdint>
#include <span>
namespace filament { class Engine; }
namespace heaven::smoke {
// Invoke with an initialized Engine and the compiled standalone sampler package.
// Uses only controlled generated textures and its own temporary DDS archive;
// no scene, proprietary assets or external environment settings are consulted.
[[nodiscard]] Result<void> verify_height_sample_batch_fixture(
    filament::Engine& engine, std::span<const std::uint8_t> package);
}
#endif
