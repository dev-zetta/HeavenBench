# DeepBump normal-to-height adaptation

HeavenBench's deterministic village-ground height generator adapts the
Frankot-Chellappa normal-to-height stage from
`module_normals_to_height.py` in Hugo Tini's DeepBump, revision
`fad19ba87daed12b1d0410a57e74f3d79e82f78d`.

DeepBump is licensed under GPL-3.0. HeavenBench changes the implementation to
C++23, decodes the existing BC5 tangent-space normal map, uses the renderer's
UV orientation directly, performs a periodic double-precision transform with
pocketfft, applies deterministic robust normalization, and emits a complete
BC4 DDS mip chain. DeepBump's neural models and Python sources are not bundled.

pocketfft is fetched from its canonical upstream repository at
<https://gitlab.mpcdf.mpg.de/mtr/pocketfft>, pinned at revision
`c90e55b3d529f8efa40ed01a20de22405f45fc65`, and distributed under the
BSD-3-Clause license.
