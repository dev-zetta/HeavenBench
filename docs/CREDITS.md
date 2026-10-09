# Credits

Open **Credits** in HeavenBench's top bar to view these acknowledgments in the application. The dialog is available during exploration and benchmark playback; close it with **Close**, its title-bar close control, or Escape. It does not pause the scene or change benchmark settings.

## HeavenBench

**Gabriel Max** — benchmark development, renderer integration, asset-tool modernization and project maintenance. Contact: [dev@zetta.app](mailto:dev@zetta.app). This name and address come from the repository's Git author identity.

HeavenBench is an independent project built around the original Heaven scene. The original scene, artwork, music and engine work are credited to the Unigine team below.

## Original Heaven Benchmark

These names and roles are transcribed from the original Heaven 4.0 credits images, preserving their displayed spellings and aliases.

### Content

| Name | Role |
| --- | --- |
| Pluton (Andrey Kushner) | 3D artist |
| Andrewko (Andrey Kolchin) | 3D artist |
| Brrrr (Stanislav Zagniy) | 3D artist |
| Black_D_N (Dmitriy Cherniy) | GUI designer |
| Jane (Eugenia Shergina) | GUI designer |
| Paralyzah (Mike Kondratyev) | Musician |
| Manguste (Nadezhda Ovchinnikova) | Localization |

### Technology

| Name | Role |
| --- | --- |
| Frustum (Alexander Zapryagaev) | Engine & logic programmer |
| Johnatan (Evgeniy Tetenchuk) | Launcher programmer |
| Anna (Anna Chaplinskaya) | Build engineer |

### Quality Assurance

| Name | Role |
| --- | --- |
| Necris (Dmitry Kriventsov) | QA lead |
| Silent (Andrew Bayun) | QA engineer |
| Sebastianbah (Kate Popova) | QA engineer |

### Production

| Name | Role |
| --- | --- |
| Binstream (Denis Shergin) | Producer |
| Natalia (Natalia Ryukhova) | PM assistant |

### Thanks to

| Name | Role |
| --- | --- |
| TmifX (Ivan Gulyaev) | Logic programmer |
| FigoNEt (Pavel Kurinnoy) | Logic programmer |
| Jashik (Yakov Negrey) | GUI programmer |
| MrRed (Sergey Kanashchuk) | QA engineer |
| Watchvombat (Polina Ignatova) | QA engineer |
| Ilmarinen (Serge Larin) | PM assistant |

Original copyright notice: **Unigine Corp. © 2005-2013. All rights reserved.** Original Heaven assets remain external to this source repository and application packages; these acknowledgments do not change their ownership or license.

The original files are `heaven/splashes/credits_16x9.jpg` and `heaven/splashes/credits_4x3.jpg` inside the Heaven asset tree. The copies inspected after extraction have SHA-256 hashes `554f353b3a9b495cbb110e0f4dbf8c6fdeab51ed862ca39a1c03bfd31136e4f3` and `ff778d0264ec884a700c39f334b24fdbcd068a91272b9518fb52da6fc16dc480`, respectively. The images themselves are not redistributed here.

## Original extraction tool

**Luigi Auriemma** — original UNG archive extraction tool, copyright 2012–2019. The refactored [extractor source](../tools/extraction/uniginex.cpp) retains his copyright and GNU GPL version 2-or-later notice. HeavenBench's subsequent tool refactoring and asset preparation are credited separately to Gabriel Max above.

## Third-party software and assets

HeavenBench thanks the authors and contributors of its rendering, physics, interface, asset-processing and platform libraries. These acknowledgments complement the full license notices supplied in packages.

| Component | Acknowledgment |
| --- | --- |
| Filament | Google and Filament contributors; rendering and sky textures |
| Jolt Physics | Jorrit Rouwe and Jolt contributors; physics |
| OpenSubdiv | Pixar and OpenSubdiv contributors, including the organizations named in its [NOTICE](../lib/OpenSubdiv/NOTICE.txt); subdivision surface evaluation |
| SDL | Sam Lantinga and SDL contributors; windows, input and platform integration |
| Dear ImGui | Omar Cornut and Dear ImGui contributors; application interface |
| GLM | G-Truc Creation, Christophe Riccio and GLM contributors; mathematics |
| nlohmann/json | Niels Lohmann and contributors; JSON configuration and reports |
| LZ4 | Yann Collet and contributors; compression |
| zlib | Jean-loup Gailly and Mark Adler; compression |
| libzip | Dieter Baron, Thomas Klausner and contributors; ZIP storage |
| pocketfft | Martin Reinecke, Peter Bell, Max-Planck-Society and contributors; ground-height Fourier reconstruction |
| bcdec | Sergii Kudlai; block-compressed texture decoding |
| RapidCheck | Emil Eriksson and contributors; property testing |
| stb | Sean Barrett and contributors; image processing and Vorbis music decoding |
| MikkTSpace | Morten S. Mikkelsen; tangent-space generation |
| DeepBump | Hugo Tini; Frankot–Chellappa normal-to-height reconstruction adapted for ground relief |
| EnvironmentalVisualEnhancements / BoulderCo | Ryan Bray, Warwick Allison, Ghassen Lahmar and contributors; cloud texture references, retained with the [asset notice](../benchmark/assets/clouds/README.md) |

The package's `licenses/` directory contains collected repository and platform dependency notices and a `MANIFEST.json` inventory. The [license collector](../scripts/collect_licenses.py) preserves upstream texts and provenance; platform packages can contain additional dependencies beyond the overview above. The project source license is available in [LICENSE](../LICENSE). See the [sky asset notice](../benchmark/assets/sky/README.md) for the bundled sky textures.
