/*
 * HeavenBench — Background music subsystem (MusicPlayer)
 *
 * MusicPlayer is a GPU-free infrastructure subsystem (in the AGENTS.md sense): a
 * concrete, backend-independent collaborator owned by the Application via a
 * std::unique_ptr, NOT an IRenderFeature. It owns no GPU resources and does no
 * per-frame render work, so it lives entirely outside the Filament backend.
 *
 * On a normal Render_Run with music enabled it fully decodes the looping Heaven
 * Ogg Vorbis track to interleaved 16-bit PCM in memory once (on its own
 * background thread), opens the SDL audio device, and streams that buffer
 * through the SDL audio callback — on a path completely independent of the
 * render loop, so frame timing is unaffected. Every failure
 * (missing/unreadable file, decode failure, unavailable device, open timeout)
 * is non-fatal: the player logs one diagnostic and the renderer keeps rendering
 * silently.
 *
 * It depends only on SDL2 (already a dependency, for the audio device) and the
 * vendored stb_vorbis decoder (for Ogg Vorbis), so it builds in the engine tier
 * but is compiled ONLY into the renderer / smoke targets (never the headless
 * host test target — the pure value helpers it relies on live in types.h).
 *
 * This translation unit is the SKELETON: the class shape, the no-op-when-
 * disabled start(), the idempotent stop(), volume(), and the defensive
 * Packer_Mode guard are implemented here. The resolve/decode/device-open/
 * playback bodies (run_startup / fill / the callback) are stubbed and fleshed
 * out by tasks 6.2 and 6.3.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_MUSIC_PLAYER_H_
#define HEAVENBENCH_SRC_ENGINE_MUSIC_PLAYER_H_

#include "engine/asset_file_system.h"
#include "engine/path_resolver.h"
#include "engine/types.h"

#include <SDL2/SDL.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace heaven {

class MusicPlayer {
public:
    MusicPlayer() = default;
    ~MusicPlayer();  // calls stop()

    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // Begin asynchronous startup. Returns immediately. When config.enabled is
    // false this is a pure no-op: no thread is spawned, the device stays
    // unopened, and the track stays unplayed. When enabled it
    // spawns the music thread that resolves, decodes, opens the device (under a
    // 5 s timeout) and starts looping playback. Every failure is a logged
    // diagnostic; start() itself never fails.
    void start(const MusicConfig& config, const PathResolver& paths,
               AssetFileSystemRef assets = std::nullopt);

    // Stop playback, close the device, and join the music thread. Idempotent
    // and safe to call when start() was a no-op.
    void stop();

    // The active Music_Volume (post-clamp), exposed for the overlay/diagnostics.
    [[nodiscard]] float volume() const noexcept;

    // Set the active Music_Volume at runtime (Live_Setting;). Stores into
    // the existing atomic the SDL callback already reads, so the change is
    // applied on the next callback with no device reopen. Reuses the
    // parse_music_volume clamp semantics: a value below 0 or above 1 is pulled
    // to the nearest bound, and a NaN is ignored (the stored volume is left
    // unchanged), so the stored value always stays in [0,1].
    void set_volume(float value) noexcept;

    // True once the music thread has finished resolving + decoding the track,
    // opened the audio device, and unpaused playback; false before
    // startup completes, on any startup failure, and after stop(). A read-only
    // observer (like volume()) for the overlay/diagnostics and for smoke tests
    // to confirm playback actually started — it never mutates state.
    [[nodiscard]] bool playing() const noexcept {
        return playing_.load(std::memory_order_acquire);
    }

private:
    // Runs on the music thread: resolve → decode → open(5 s) → play.
    // Stubbed in the skeleton; filled by tasks 6.2 (resolve/decode) and 6.3
    // (device open / playback / failure handling).
    void run_startup(const std::string& music_path, AssetFileSystemRef assets);

    // SDL audio callback (static thunk + member). Streams decoded PCM with a
    // wrapping cursor and applies the volume scalar. Lock-free.
    static void sdl_audio_callback(void* userdata, Uint8* stream, int len);
    void fill(int16_t* out, int frames);

    // Defensive Packer_Mode guard. The structural guarantee is that
    // the packer path never constructs a MusicPlayer, so the audio device is
    // unreachable while packing. This belt-and-suspenders predicate gates every
    // device-open / playback request: a normally-constructed player reports
    // true (on a Render_Run); if a player ever found itself off a render run,
    // run_startup rejects the open, leaves the device unopened, and reports
    // audio-unavailable.
    [[nodiscard]] bool on_render_run() const noexcept { return render_run_; }

    // Decoded interleaved 16-bit stereo PCM, published before unpausing and
    // immutable thereafter (read only by the callback).
    std::vector<int16_t> pcm_;
    int                  channels_    = 2;
    int                  sample_rate_ = 44100;

    std::atomic<size_t>  cursor_{0};       // read position into pcm_ (samples)
    std::atomic<float>   volume_{1.0f};    // amplitude scalar
    std::atomic<bool>    playing_{false};  // callback emits silence when false

    SDL_AudioDeviceID    device_ = 0;
    std::thread          init_thread_;

    // True for a normally-constructed (Render_Run) player; see on_render_run().
    bool                 render_run_ = true;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_MUSIC_PLAYER_H_
