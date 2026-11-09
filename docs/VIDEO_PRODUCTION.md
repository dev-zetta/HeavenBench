# HeavenBench video production

The November 5, 2026 production refreshes the retained 1.0.0 showcase for the published 1.1.1 renderer and adds a narrated update covering the changes since 1.0.0. The versioned working project is `build/video-showcase/20261105-v1.1.1/`; the original `build/video-showcase/20261010/` production and [published 1.0.0 showcase](https://youtu.be/v-s5fNxlnVw) are preserved as archives.

## Published videos

The user reported uploading both videos to YouTube on November 9, 2026. YouTube's public metadata was checked that day to confirm both titles and their channel, Gabe "Zetta" Max (`@OpenArcanum`). The production scripts performed no upload; the retained export receipts and editing ZIP describe the pre-publication production checkpoint.

| Video | Local export duration | YouTube |
| --- | --- | --- |
| HeavenBench 1.1.1 feature showcase, benchmark and microprofiler | 9:30 | [Watch the 4K60 showcase](https://youtu.be/ChFH_zX_q4U) |
| HeavenBench 1.1.1: what changed since 1.0.0 | 4:24 | [Watch the narrated update](https://youtu.be/f4JlHzZyq1w) |

The README uses the showcase's published YouTube thumbnail, retained at `docs/images/heavenbench-1.1.1-showcase.jpg` from `https://i.ytimg.com/vi/ChFH_zX_q4U/maxresdefault.jpg`. The original 1.0.0 thumbnail remains unchanged. YouTube transcodes are separate from the hash-bound local exports below; metadata verification does not establish byte identity with those exports.

The README hero image is now `docs/images/heavenbench-1.1.1-village.png` (SHA-256 `2df7e69366fc0b060ed1daeb22e05bf20b7b2a1aa7705f6e1d2f74514038242a`), replacing the first-release screenshot. It is frame 300 of the released 1.1.1 `U01-village-12` master (SHA-256 `c9ee2167503b0a9bfe109087694c9bebcca8442d91d34f465ee699e03653c080`), captured with Intel Vulkan, Balanced System and original textures. FFmpeg extracts the actual rendered frame, downsamples 3840×2160 to 1920×1080 with Lanczos and converts limited Rec.709 to full-range sRGB for the web PNG. The capture receipt and frame-specific configuration/metadata remain in the versioned production directory; the old image is retained for historical references.

## Source and capture

All refreshed cinematic footage uses the ordinary released Linux portable 1.1.1 AppImage, SHA-256 `5781cf0f74974fc2fbed55af943811d9e1e3e377a25fc06ed9efebeb92942dc5`, bound to release source `7930b9eaae5feef8dfae7396e479d534848d721f`. The extracted executable is SHA-256 `1b10975c1b941a5023224624a8d0113bdb01c98a734bd1a9a05605c3990eacdf`. Current development sources are not substituted for that package. Original Heaven assets remain external and retain their original ownership and terms.

The native acquisition plan contains 36 showcase shots and three additional update views, each ten seconds at 3840 × 2160 with fixed 60 Hz simulation. The additional village, airship-deck and night-street views use Balanced System and original textures; per-clip labels identify their effective settings. The host uses Intel BMG G31 (`8086:e223`, PCI `0000:03:00.0`), Mesa 26.0.8 and Vulkan through X11. Every capture verifies the actual GPU, API, dimensions, simulation clock and full rendering scale. Separate provenance companions bind the unchanged native metadata to the released package; the native screenshot schema does not contain an application-version field. Offline playback is not a benchmark FPS measurement, and the older AMD R9700 footage is not used as a performance baseline.

The production retains the original camera recipes, quality settings, original/optional generated 4K texture comparisons, music, twelve chapters and 570-second showcase structure. The clear Moon view deliberately uses original sky textures and camera zoom. Comparison preflight checks camera and animation clocks, actual GPU/driver/API, source package and effective settings, allowing only the disclosed setting difference. Native full-range sRGB frames are explicitly converted to limited-range Rec.709; overlays retain geometric alpha while their RGB transfer is converted separately.

Desktop controls, browser reports and OnePlus tablet inserts retained from the earlier production carry a visible `Archive 1.0.0` label, including nested mixed-source edits. Their measurements describe the recorded older run and hardware. The tablet was disconnected for this production. The update video uses only freshly rendered 1.1.1 scene footage, with disclosed nonadjacent scene revisits and a clear five-second rain excerpt selected after foreground grass moves aside. Archived variable/30 FPS captures retain native cadence when placed on the 60 Hz showcase timeline; there is no optical-flow interpolation or slowed playback.

## Narrated update

The 527-word English script separates the 1.0.1 color/day/night lighting changes, the 1.1.0 experimental frame-generation integration and the 1.1.1 GPU, cache, ZIP loading, rain, depth-resume and Intel GLX corrections. Existing rain, HDR, vegetation and displacement features are not introduced as new additions. Persistent Android signing is a post-release CI change and is excluded from the released 1.1.1 feature narrative. Exact released-source references accompany the editable script.

The ordinary capture package has both frame-generation SDK integrations disabled. The update explains interpolation using a clearly labelled concept diagram over real-rendered background footage. It does not present that footage as generated images, a smoothness comparison, confirmed display FPS or latency evidence. Separate experimental packages, SDR/fixed-resolution constraints, default-Off behavior and diagnostic/unscored tours remain explicit. The optional Windows DLSS adapter requires separately supplied NVIDIA runtime plugins; native NVIDIA runtime acceptance is not claimed. See [FRAME_GENERATION.md](FRAME_GENERATION.md).

Narration uses the generic `af_heart` stock voice through locally installed Kokoro ONNX with CPU inference. The selected take contains 38 sentences in eight sections and lasts 242.100041667 seconds before the opening and credits. Its producer pins script, model, voice and package hashes, disables ONNX Runtime telemetry before import and retains sentence/section sample boundaries and explicit phrase gaps. Technical audio checks and listener approval are separate; the production receipts do not claim that automated waveform checks establish natural pronunciation or creative approval. Local model/tool licenses and notices accompany the editable production.

The soundtrack remains “Dreams Become Real” by Kevin MacLeod (incompetech.com), [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), [official track](https://incompetech.com/music/royalty-free/index.html?isrc=USUAN1500027). It is edited, repeated, crossfaded and level-adjusted; the update additionally ducks it under narration. Retain the exact attribution and modification notice, original Heaven author credits, extraction-tool credit to Luigi Auriemma and dependency acknowledgments when reusing either edit.

## Reproduction and checks

Run capture controls from the repository root. `python3 build/video-showcase/20261105-v1.1.1/scripts/batch_capture.py --plan` validates the final queue without GPU execution; omitting `--plan` runs the queue. The controller permits at most two clip processes and one native GPU renderer, bounds process duration and preserves failed attempts. Raw RGB frames are deleted only after metadata checks, successful encoding, exact frame-count checks, complete decode and review-still export. Package, input archives, configurations and per-attempt ownership markers remain bound by hashes.

`scripts/compose.py` inside the production defaults to planning. Its `--stage ready` mode composes available verified sources; `--stage mux`, `--stage review` and `--stage qa` finish and check the showcase. `update/compose_update.py` builds the separate narration-timed edit from its measured audio manifest and approved native-master receipt. Retain the editable EDL, captions, source manifests, commands, checksums, audio stems, credit notices and review receipts alongside the exports. Original asset archives, caches, bulk raw frames and local TTS environments are not part of the portable editing bundle.

The editing bundle carries media, stems, captions and EDLs for relinking. Its relocation helper updates selected editable JSON and leaves historical evidence unchanged; additional media relinking can be manual. Rerunning the strict update-export qualification still requires the original absolute production layout and the exact external AppImage, extracted runner/binary and original asset archives. Regenerating source-based coverage also requires the released Git checkout/object. The saved evidence remains readable without those regeneration dependencies.

Final export verification must bind the exact delivered files and check complete decoding, dimensions, frame count, duration, H.264/Rec.709 metadata, stereo 48 kHz audio, caption text/timing, chapter alignment, MP4 fast-start layout and audio loudness/true peak. Native-master and sampled composed-frame review have bounded visual scope. Render completion, technical verification, sampled visual review, user creative approval and public publication are recorded separately; copied historical acceptance is never current acceptance.

Production status: all 39 native masters and both films are rendered and technically verified, including 4K/60 masters and 1080p/60 review copies. The showcase contains 34,200 frames over 570 seconds, twelve chapters and 72 caption cues. The narrated update contains 15,847 frames over 264.116666667 seconds, nine chapters and 38 cues. All 55 showcase segments passed bounded review of 213 decoded rasters; the update cards and source selections received separate bounded still review. The subsequent user-reported YouTube publication is recorded above; historical creative-review and publication fields in the saved receipts retain their original checkpoint values.

The final delivery receipt is `qa/delivery/final.json` (SHA-256 `a0f3dbdfb35d817cb5a2763aa8dc170022fe2f1bac40feb662b098501f2a22d4`) under the production root. It binds complete counted/decode evidence to each delivered container, exact caption text/timing, chapter boundaries, SDR/H.264/stereo-audio metadata and fast-start layout. The optional-caption transaction preserves every encoded payload and changes only the subtitle track-enabled bit; its retained byte-comparison proofs bridge the pre-edit decoding evidence to the final container hashes. The music-only showcase measures −23.0 LUFS / −8.0 dBFS true peak; the approved narrated AAC measures −16.24 LUFS / −1.76 dBTP and remains packet-identical in both update exports.

| Final local export | Video frames | SHA-256 |
| --- | ---: | --- |
| `deliverables/HeavenBench-1.1.1-4K60-SDR.mp4` | 34,200 | `0162bff727c7d290f5a95efeb0016be4cc8e6091928189d30e75332a3b50c33f` |
| `deliverables/HeavenBench-1.1.1-1080p-review.mp4` | 34,200 | `1a0fac501c540c9ab8a343e5d2af75afad84fe939f92e1cfea9b44c009461355` |
| `update/exports/heavenbench-1.1.1-update-4k60.mp4` | 15,847 | `19879b0d9c3bd4ef0cf4b696fef03b476dbf486340a9fe1da231cc8dd839b46b` |
| `update/exports/heavenbench-1.1.1-update-1080p60.mp4` | 15,847 | `4a64b6f6f48177ddf0da52b6f525547d2d60a81f998ad83df9be750806cf6a93` |
