# Experimental Off/FSR acceptance

Status: bounded software checks pass, and general qualitative user acceptance was recorded on October 16, 2026: “It appears to work properly.” The detailed Off/FSR protocol and individual observations were not attested.

On October 16 the user accepted software measurements plus a manual Off/FSR comparison because no high-speed camera is available. This replaces the mandatory camera recording gate in the older qualification receipts. No phone, tablet or USB connection is needed. Physical input-to-photon latency and exact display cadence remain unmeasured; optional hardware testing can follow later. Both SDKs and frame generation remain off by default, NVIDIA runtime execution is excluded, and this review covers experimental SDR at fixed resolution.

## Software evidence and source

The verified `5316360ef95af77d90b64d09af4a085d21b466ba` CI/package checkpoint has bounded native/input/lifecycle/quality/endurance/suspend evidence and accepted ordinary/SDK Linux and Windows package audits. The archived [qualification summary](../build/frame-generation/20261015/finish-gaps/qualification-summary-v2.md) and [v4 matrix](../build/frame-generation/20261015/finish-gaps/original-goal-completion-matrix-v4.json) retain the previous scope and historical pending-camera status. Their submission counters and KHR completion bounds do not measure physical latency or prove each image was displayed.

New input instrumentation is later source work; those accepted packages do not contain it. Its report must identify the measured source/binary and associate consumed input with the accepted real source carrying its response. Preserve event processing, render submission and GPU-completion observation as distinct endpoints with explicit clocks and unavailable samples. Software timing excludes keyboard/mouse electrical delay, compositor/scanout and pixel response. Existing profiles can describe frame intervals, stalls and CPU/GPU observations, but cannot retroactively supply missing input timestamps. A software report does not substitute for the user's observations below.

An instrumented build enables the transient diagnostic with `HEAVEN_INPUT_RESPONSE_TRACE=/absolute/writable/input-response.json`; choose a different output file for each Off/FSR take. It is off by default. Actual consumed F6/camera inputs use CPU `steady_clock` timestamps and correlate to the first accepted real source; rejected attempts preserve pending inputs. The report separates consume-to-`render_frame`-return time from consume-to-matching-`FrameInfo.gpuFrameComplete` host fence-return time. The latter is an upper bound including queue/provider pacing and worker scheduling; it is not a GPU clock timestamp or screen measurement. Device/OS delay before consumption is excluded. Collection adds no ordinary-frame waits and caps observations at 4,096 events and 64 outstanding frame probes. Coalesced, cancelled, net-zero, missing and unresolved observations remain visible; last GPU results may not publish before exit. Atomic export occurs on exit, with raw IDs/stages/statuses, counters, per-kind percentiles and build/hardware/start/end settings. Traced tours are diagnostic/unscored even with Off.

The helper application/host build and focused 2/2 CTest selection pass. The [local native receipt](../build/frame-generation/20261016/software-acceptance/native-04/receipt.json) records Off and FSR with six consumed UI and fifteen consumed camera responses each, all with frontend/GPU observations and no invalid input, event/probe overflow or association ambiguity. Both runs exit zero and FSR reports zero native presentation failures. Median consume-to-GPU-fence observed upper bounds are Off UI 149.828 ms/camera 164.777 ms and FSR UI 172.053 ms/camera 179.819 ms. This single small cohort uses automated XTest through `SDL_MOUSE_RELATIVE_MODE_WARP=1` on X11/XWayland; raw device delivery and physical delay are excluded. Expected rejected/startup Filament ID gaps appear in `history_missing`, while all 42 recorded input responses resolve. Two earlier synthetic raw-relative delivery attempts with successful UI and zero camera responses remain preserved. The receipt binds later local source-diff/binary identities, without extending the source `5316360` CI result. These descriptive software results do not establish a clean performance or physical latency comparison; general user acceptance is recorded separately below.

The [final-controls-02 receipt](../build/frame-generation/20261016/software-acceptance/final-controls-02/receipt.json) additionally verifies byte-identical traced/untraced Off PPM output for thirty fixed 640×360 sources using `--test-scene material-models`, and explicit export-failure exit 1 with the sentinel preserved and no temporary output left. This is a bounded fixture/export check; the earlier invalid-flag attempt remains failed evidence.

## Manual comparison

Use one FSR-enabled executable and the same assets/settings for both takes. The accepted Linux package from source `5316360` is suitable for the qualitative comparison; use an instrumented build only when its own identity and measurement readiness are recorded. Keep the same SDR resolution, monitor refresh/VRR, window mode, initial camera, scene/lighting and effective graphics settings. Disable dynamic resolution. Use ordinary exploration and verify the effective provider in Settings → Frame generation; an unavailable FSR request is not an FSR take. Keep diagnostic image readback, Vulkan validation, profiles, Microprofiler recording and the KHR observer off for this qualitative view. Opening the Microprofiler panel with F6 does not start recording. If also collecting the input trace, record that fact and preserve its exit report; it is diagnostic evidence rather than a clean performance comparison.

From the repository root, first write a reusable common configuration for the helper-built instrumented renderer, then run Off and FSR with it. These commands select Visual Ultra, original textures and 960×540, but do not claim byte-identical configuration to the software cohort. Confirm SDR and the other common settings before reviewing. Close the first run before starting the second; after each run exits its separate trace is exported. The automated test's SDL warp override is not required for ordinary manual input.

```bash
mkdir -p build/frame-generation/manual-review
review_config="$PWD/build/frame-generation/manual-review/common.config.json"
./scripts/run.sh --preset visual-ultra --texture-quality original --width 960 --height 540 \
  --frame-generation off --no-dynamic-resolution --no-music --write-config "$review_config"
HEAVEN_INPUT_RESPONSE_TRACE="$PWD/build/frame-generation/manual-review/off.json" \
  ./scripts/run.sh --x11 --config "$review_config" --data "$PWD/assets/assets.zip" \
  --frame-generation off --camera-tour-time 100 --no-dynamic-resolution --no-music
HEAVEN_INPUT_RESPONSE_TRACE="$PWD/build/frame-generation/manual-review/fsr3.json" \
  ./scripts/run.sh --x11 --config "$review_config" --data "$PWD/assets/assets.zip" \
  --frame-generation fsr3 --camera-tour-time 100 --no-dynamic-resolution --no-music
```

1. Start with **Off**, finish loading and allow a short settling interval. For about 30 seconds, hold the right mouse button and look slowly left/right across a railing, lattice or lamp, then make a few short direction reversals. Release the button. Observe motion steadiness, doubled/dragging edges and whether camera movement follows your hand promptly.
2. With the camera stopped, press F6 a few separate times to open/close the panel. Observe whether the panel follows your presses promptly; keep this UI observation separate from camera response. Leave the panel closed and exit.
3. Restart with **FSR**, restore the same starting camera and repeat the same motions and panel toggles for about 30 seconds. Confirm FSR is effective after loading. Record any obvious stall, persistent smear, disappearance, fallback or control lag, including what action triggered it. Exit after reviewing.
4. Record the comparison below. If a difference is uncertain, say so; optionally repeat in reverse order. Report unacceptable behaviour with the scene/action that reproduces it. Pacing changes require a concrete issue supported by these observations or software evidence.

This is a qualitative comparison on the observed display. It cannot label every generated image, provide exact halfway-time ground truth, measure milliseconds of physical latency or establish a universal frame-rate threshold.

## Observation record

The user reported “It appears to work properly” on October 16, 2026. This closes the revised manual item as general qualitative acceptance. It does not attest execution of the detailed protocol or supply separate mode, motion, camera or UI assessments; those fields stay explicitly unspecified.

| Field | Record |
| --- | --- |
| Review date and reviewer | October 16, 2026; user in this conversation |
| Executable/source or package identity | Not supplied for the manual review; local software checks separately bind `2921dfb` |
| Assets and settings/configuration | Not supplied |
| GPU/driver and display refresh/VRR | Not supplied |
| Resolution, window mode and starting camera | Not supplied |
| Take order and effective modes | Not supplied |
| Motion steadiness: FSR better / same / worse / uncertain | Not supplied |
| Ghosting/doubled edges and affected objects | Not supplied |
| Camera response: acceptable / laggy / uncertain | Not supplied |
| F6 panel response: acceptable / laggy / uncertain | Not supplied |
| Stalls, fallback or other reproducible issue | No issue described in the acceptance message; individual cases not supplied |
| Software cohort and measured source identity | `native-04/receipt.json`; later local source-diff and binary bound there |
| Manual-take trace paths, if collected | Not supplied |
| User decision: acceptable experimentally / needs correction / uncertain | General qualitative acceptance: “It appears to work properly.” |
| Physical input-to-photon latency | Unmeasured; optional follow-up |

The revised experimental, default-off acceptance is complete within the bounded software evidence and general user report. The unspecified fields are not inferred or turned into additional gates. Exact protocol execution, physical latency and unrestricted image quality are not established. NVIDIA runtime execution remains excluded; the local diagnostic commit is separate from the CI/package checkpoint at `5316360` and has not been pushed or packaged.
