# HeavenBench documentation

Start with [Getting started](GETTING_STARTED.md) for asset preparation, platform launch, controls and saved reports. [Building HeavenBench](BUILDING.md) covers source builds and packages; [Development and measurement](DEVELOPMENT.md) covers architecture, scoring, testing, profiling and extraction research. The [main README](../README.md) introduces the application.

## Using HeavenBench

- [Getting started](GETTING_STARTED.md): scene assets, launchers, controls, settings and reports.
- [Rendering features and presets](FILAMENT_RENDERING_FEATURES.md): available effects and preset behavior.
- [Graphics drivers](GRAPHICS_DRIVERS.md): startup advisories and compatible upgrade guidance.
- [Android guide](../android/README.md): installation, touch controls, preview configuration and device-validation limits.
- [Known issues](KNOWN_BUGS.md): current bugs and reproduction notes.
- [Credits and third-party notices](CREDITS.md): contributors, libraries, tools and asset attribution.
- [Changelog](../CHANGELOG.md): daily development history and the 1.0.0 source baseline.

## Building and developing

- [Building HeavenBench](BUILDING.md): release helpers, Linux development, CPU variants, AOCC, Windows, macOS, Android and material compilation.
- [HeavenBench on Apple Silicon](MACOS.md): macOS 15+ download, native M1+ workflow, app bundles, launch and validation scope.
- [Shared release builds](DOCKER_BUILDS.md): Docker pipelines, caches, provenance and validation scope.
- [Development and measurement](DEVELOPMENT.md): source layout, score/report internals, automated checks and supporting extraction tools.
- [Asset tools](../tools/README.md): installer acquisition, extraction, packing and offline preparation.
- [ZIP asset storage](ZIP_ASSET_STORAGE.md): runtime archive layout and packer behavior.
- [High-resolution texture pipeline](HIRES_TEXTURE_PIPELINE.md): optional offline texture generation.
- [File formats](FILE_FORMATS.md): UNG, mi08, XML0 and transform conventions.
- [Original shader research](EXTRACTED_SHADERS.md): extracted engine shader behavior.
- [Filament patch notes](../patches/filament/README.md): retained vendor changes and reproducible dependency pins.

## Rendering and performance investigations

- [Rendering test scenes](RENDER_TEST_SCENES.md): procedural and real-mesh fixtures, feature pairs and capture replay.
- [Visual audit](VISUAL_AUDIT.md): full-scene findings, evidence and acceptance limits.
- [Microprofiler](MICROPROFILER.md): live controls, trace schema, clock domains and offline analysis.
- [Experimental frame generation](FRAME_GENERATION.md): optional FSR/DLSS builds, temporal input contracts, presentation counters and native acceptance limits.
- [Performance optimizations](PERFORMANCE_OPTIMIZATIONS.md): measured changes and feature costs.
- [Instancing and culling audit](CULLING_AUDIT.md): batching, bounds, draw-command oracles and optimization limits.
- [Release readiness](RELEASE_READINESS.md): platform qualification, outstanding checks and release procedure.

## Design and historical records

- [Benchmark design](BENCHMARK_PLAN.md)
- [Filament migration plan](FILAMENT_MIGRATION_PLAN.md)
- [Earlier rendering checklist](RENDERING_FEATURES.md)
- [Historical 0.4.0 release notes](RELEASE_NOTES_0.4.0.md)
