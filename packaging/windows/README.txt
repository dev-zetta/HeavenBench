HeavenBench for Windows x64

Extract the whole ZIP to a normal user-writable directory. Install a current
Vulkan-capable graphics driver, then launch Launch-HeavenBench.cmd. Runtime
DLLs and the renderer's resources are included. Original Heaven scene data is
supplied separately: see assets/README.txt. The application needs those assets
to render; --help and asset preparation commands work without opening a window.

The launcher stores configuration, results and cache beneath
%LOCALAPPDATA%\HeavenBench. Set HEAVEN_USER_DIR to choose another writable
location, or HEAVEN_DATA to select an existing asset pack/directory. Additional
arguments are forwarded, for example:
  Launch-HeavenBench.cmd --benchmark
  Launch-HeavenBench.cmd --data "D:\HeavenBench-assets"

The resources/ directory contains redistributable sky/cloud/loading assets.
Keep resources beside the executable. The standalone heaven_asset_packer.exe discovers them automatically, and HEAVEN_RESOURCE_DIR can override that location. The launcher's existing --pack-assets command remains available. The included uniginex.exe and mesh2obj.exe are command-line extraction tools; assemble_scene.py additionally requires Python 3 when used.

This ZIP targets 64-bit Windows with a portable x86-64 CPU baseline. It does
not require a separately installed Visual C++ runtime; project libraries use
the static CRT. Vulkan device drivers are not included. The build is unsigned.
Windows GPU/display behavior still requires validation on real hardware;
headless build and CLI tests do not establish rendering acceptance.

licenses/ contains license texts and a provenance/hash manifest.
package-manifest.json records SHA-256 hashes of the shipped files.
