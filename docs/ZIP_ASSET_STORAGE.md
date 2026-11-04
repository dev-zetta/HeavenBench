# ZIP asset and tessellation storage

HeavenBench can load the extracted scene from a loose `Data_Root` directory, a
single asset ZIP, or a directory containing multiple `*.zip` packs. ZIP support
is provided by pinned libzip 1.11.4; the runtime builds one indexed,
case-sensitive logical view across all archives and exposes the same paths to
all loaders.

## Packaging assets

To start directly from `Unigine_Heaven-4.0.run`, run `./scripts/build_assets.sh`. It downloads the official installer if absent, verifies its pinned fingerprint, builds the CPU tools, extracts the source assets into an ignored work directory, and writes the validated base pack to `assets/assets.zip`. Use `--installer PATH --no-download` for an existing installer, or `--overwrite` to replace an existing output. See [asset tools](../tools/README.md) for offline and native Windows commands. Base packs contain original Heaven payloads and remain ignored by Git; generated-only extension packs and individual generated images/textures may be tracked.

To pack an already extracted tree:

```bash
./scripts/build_asset_tools.sh --jobs 2
./build/asset-tools/heaven_asset_packer --data /path/to/extracted/heaven \
  --pack-assets assets/assets.zip
```

The standalone packer shares the renderer's CPU loaders and needs no display or GPU libraries. Its implementation lives under `tools/asset_packer/`; `heaven_benchmark --pack-assets` remains a compatible entry point into the same library. See [`tools/README.md`](../tools/README.md) for the separation between extraction, ZIP packing, and offline texture generation.

`--pack-overwrite` atomically replaces an existing archive.
`--pack-move` removes the selected loose source files only after the new ZIP has
closed and every entry passes validation. The archive contains
`heavenbench-assets.json`, which declares the internal `Data_Root` prefix.
Already-compressed media is stored; material, XML, track, mesh, and text data is
deflated. Entry order, timestamps, and attributes are deterministic.

Schema-v2 hi-res extension archives add validated Original/2K/4K mappings
without copying authored originals. Build one with `--pack-texture-set
PATH/texture-set.json`; place it beside the base archive. The complete offline
bake, validation, runtime selection, and attribution workflow is documented in
[`HIRES_TEXTURE_PIPELINE.md`](HIRES_TEXTURE_PIPELINE.md).

Before discovery, the packer derives
`heaven/generated/ground/ruins_ground_h.dds` from the authored BC5
`ruins_ground_n.dds`. The manifest records both fingerprints, generator and
dependency revisions, and DeepBump attribution. Loose trees and older ZIPs
generate the same immutable asset in memory at runtime. This derivative and its
attributions belong to the base pack, so texture extensions do not duplicate
them.

BC4/BC5 CPU channel decoding uses pinned `bcdec`; BC4 encoding uses the
Filament-vendored `stb_dxt`. Compressed DDS mip levels are immutable views into
one shared asset buffer, so ZIP extraction is not followed by a second payload
copy for Filament upload.

The normal runtime layout is:

```text
assets/
  assets.zip
  assets-hires.zip
```

Archives are discovered non-recursively in lexical filename order. Every pack
must carry `heavenbench-assets.json` and declare the same `Data_Root` prefix.
Duplicate payload paths are rejected; the per-pack manifest name is the sole
permitted duplicate. Texture-set mappings are validated against the combined
view, so their originals may reside only in the base pack.

Run the combined mount with:

```bash
heaven_benchmark --data /path/to/assets
```

`assets/` is the normal runtime default. A single ZIP remains supported for
compatibility. Pass `--data` or set `HEAVEN_DATA` to use another pack directory,
one archive, or a loose asset tree during development.

Config files and benchmark result reports remain native filesystem files.

## Tessellation cache

Tessellated static and skinned surfaces are stored together in `cache/tessellation-v37.zip`. The archive has one `manifest.json` and one `models/<stable-key>.bin` entry per surface. Keys include normalized logical asset paths, content fingerprints, the original source-surface ordinal, topology controls, UV transforms, the surface kind, and the algorithm version. The ordinal distinguishes same-name surfaces independently of material lookup names. Model and manifest reads require logical EOF/CRC validation before committing decoded data; a corrupted model is a cache miss.

Set `tessellation.cache_file` to override the writable archive. When it is empty, a directory data root uses `<Data_Root>/cache/tessellation-v37.zip`, while an asset ZIP or ZIP folder uses `<asset-path-parent>/cache/tessellation-v37.zip`. Cache writes are staged during scene loading and published once with libzip's archive replacement. A write failure is logged but does not abort scene loading. Version 36 and earlier ZIP identities must be regenerated; default-path migration leaves the old files in place.

The preceding ZIP semantic version 36 and current loose semantic version 34 invalidated neutral geometry generated with order-dependent midpoint rounding. Version 37 retains that correction and adds source-surface identity. Position and UV midpoints agree when duplicate tangent-seam edges are traversed in reverse, allowing exact position/UV duplicates to share their height footprint. Raw-normal/tangent interpolation, separate seam vertices and connectivity are preserved; binary layouts are unchanged. The native reverse-edge regression passes with the correction and fails with the old interpolation. This is a bounded seam correction, not acceptance of the remaining low-sun contact-shadow appearance.

Compatibility import accepts only current `v34/*.bin` plus JSON entries whose directory, metadata and payload versions match the current loose version and whose source fingerprints agree. Import additionally requires a unique matching name in the source mi08 headers so its original ordinal can be recovered. Duplicate names, invalid source headers and unavailable source identity remain misses. Imported and rejected legacy files are retained. Old `v33` loose entries are ordinary cache misses and must be regenerated; the importer does not re-encode obsolete or ambiguous geometry under a new ZIP identity. [KNOWN_BUGS.md](KNOWN_BUGS.md#current-implementation) records the current geometry and visual limits.

Sequential binary loaders share a lazy 64 KiB heap read-ahead buffer per `AssetBinaryReader`; large arrays read directly after draining cached bytes. Logical seek/tell offsets account for unread bytes, and failed reads never publish cached data. Directory, memory and ZIP streams retain their existing format/error contracts and the ZIP stream's EOF/CRC validation. The buffer follows reader ownership across moves and does not retain entire assets by default.

To prewarm all eligible surfaces without opening a window:

```bash
heaven_benchmark --data /path/to/assets \
  --build-tessellation-cache
```

An optional ZIP path after the flag overrides the output.

## Loading benchmark

Given a loose data root, this action creates equivalent stored and fast-deflate
archives and compares them with loose loading:

```bash
heaven_benchmark --data /path/to/extracted/heaven \
  --benchmark-loading loading-results
```

The action alternates method order across four runs, verifies content hashes
and NullBackend scene counts, compares the legacy loose tessellation cache with
the versioned ZIP archive, and writes `loading-benchmark.json` plus
`loading-benchmark.html`. Reports distinguish the first observed pass from warm
median/p95 values; they do not describe the first pass as an OS-cold-cache
measurement and contain no CI timing threshold.
