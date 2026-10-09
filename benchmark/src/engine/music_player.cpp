/*
 * HeavenBench — Background music subsystem (MusicPlayer) implementation
 *
 * SKELETON. This TU implements the class shape and lifecycle:
 *   • the destructor (calls stop()),
 *   • start() — returns immediately and is a pure no-op when music is disabled
 * otherwise spawns the music thread running run_startup,
 *   • stop() — idempotent and safe after a no-op start(): clears playing_,
 *     winds down + joins the music thread, and closes the device if open,
 *   • volume(),
 *   • the SDL callback thunk + fill() (gap-free loop + volume via fill_block),
 *   • the defensive Packer_Mode guard.
 *
 * The resolve/decode body of run_startup is implemented here. The
 * device-open + unpause + failure handling body is implemented here.
 */
#include "engine/music_player.h"
#include "engine/asset_file_system.h"

#include "stb_vorbis.h"

#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>
#include <utility>

namespace heaven {

MusicPlayer::~MusicPlayer() {
    stop();
}

void MusicPlayer::start(const MusicConfig& config, const PathResolver& paths,
                        AssetFileSystemRef assets) {
    // Publish the configured (already CLI-clamped) volume so volume() reports
    // the active level even when music is disabled for this run.
    volume_.store(config.volume, std::memory_order_relaxed);

    // No-op when disabled: spawn no thread, open no device, leave
    // the track unplayed. Disabling music is not an error, so no diagnostic.
    if (!config.enabled) {
        return;
    }

    // All fallible work (resolve / decode / device open) happens on the music
    // thread so start() returns immediately and a music problem can never abort
    // Application::init(). Capture the resolved path by value so
    // the thread does not outlive a reference into the PathResolver.
    std::string music_path = paths.music_path();
    init_thread_ = std::thread([this, path = std::move(music_path), assets]() mutable {
        run_startup(path, assets);
    });
}

void MusicPlayer::stop() {
    // Signal the callback to emit silence first.
    playing_.store(false, std::memory_order_release);

    // Wind down and join the music thread before touching the device, so no
    // init work races the close below. Safe when start() was a no-op: an
    // unspawned thread is not joinable.
    if (init_thread_.joinable()) {
        init_thread_.join();
    }

    // Close the device once the threads that touch it are quiescent. Idempotent:
    // device_ == 0 after a no-op start() or a prior stop().
    if (device_ != 0) {
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }
}

float MusicPlayer::volume() const noexcept {
    return volume_.load(std::memory_order_relaxed);
}

void MusicPlayer::set_volume(float value) noexcept {
    // Mirror parse_music_volume's clamp semantics so the stored amplitude
    // scalar always stays in [0,1]: pull an out-of-range value to the nearest
    // bound and ignore a NaN (leave the previous volume in place). The audio
    // callback reads volume_ every block, so the new value applies on the next
    // callback with no device reopen.
    if (std::isnan(value)) {
        return;
    }
    if (value < 0.0f) {
        value = 0.0f;
    } else if (value > 1.0f) {
        value = 1.0f;
    }
    volume_.store(value, std::memory_order_relaxed);
}

void MusicPlayer::run_startup(const std::string& music_path,
                              AssetFileSystemRef assets) {
    // Defensive Packer_Mode guard: reject any open/play request that is
    // not on a Render_Run, leaving the device unopened and reporting that audio
    // is unavailable in Packer_Mode. Structurally the packer never constructs a
    // MusicPlayer, so this is belt-and-suspenders.
    if (!on_render_run()) {
        std::fprintf(stderr,
                     "MusicPlayer: audio is unavailable in packer mode; "
                     "refusing to open the audio device\n");
        return;
    }

    // ── Resolve-existence check  ────────────────────────────────
    // The path is already resolved (PathResolver::music_path(), captured in
    // start()); here we probe it on disk. Use the std::filesystem error_code
    // overloads so a transient filesystem error never throws on the music
    // thread. Distinguish a missing file from one that exists but cannot be
    // read, emitting exactly one diagnostic naming the resolved path and the
    // cause. On either failure leave playing_ false and return without opening
    // the device.
    if (!asset_exists(music_path, assets)) {
        std::fprintf(stderr,
                     "MusicPlayer: music asset not found at '%s'\n",
                     music_path.c_str());
        return;
    }

    // Existence alone does not guarantee readability (permissions, a directory
    // in the way, a vanished mount). Probe readability by actually opening the
    // file for binary reading — the same access the decoder needs.
    if (!assets) {
        std::ifstream probe(music_path, std::ios::binary);
        if (!probe.is_open()) {
            std::fprintf(stderr,
                         "MusicPlayer: music asset at '%s' exists but is not "
                         "readable\n",
                         music_path.c_str());
            return;
        }
    }

    // ── Decode the Ogg Vorbis track to interleaved 16-bit PCM
    //    ─────────────────────────────────────────────────────────────
    // stb_vorbis_decode_filename decodes the whole file to interleaved int16,
    // mallocs the output buffer, writes the channel count and sample rate, and
    // returns the number of sample-frames per channel (negative/zero or a null
    // output on failure). A failure here is a decode failure: emit one
    // diagnostic naming the path and return without opening the device.
    int channels = 0;
    int sample_rate = 0;
    short* output = nullptr;
    int frames = 0;
    auto encoded = read_asset_file(music_path, assets);
    if (!encoded ||
        encoded->size() > static_cast<std::size_t>(INT_MAX)) {
        std::fprintf(stderr,
                     "MusicPlayer: music asset at '%s' exists but is not "
                     "readable\n",
                     music_path.c_str());
        return;
    }
    frames = stb_vorbis_decode_memory(
        encoded->data(), static_cast<int>(encoded->size()),
        &channels, &sample_rate, &output);

    if (frames <= 0 || output == nullptr || channels <= 0) {
        if (output != nullptr) {
            std::free(output);
        }
        std::fprintf(stderr,
                     "MusicPlayer: failed to decode Ogg Vorbis music asset at "
                     "'%s' (decode failure)\n",
                     music_path.c_str());
        return;
    }

    // Copy the malloc'd interleaved samples into the owning pcm_ vector and
    // release the decoder's buffer. pcm_ holds frames * channels samples; it is
    // published to the SDL callback (and made immutable) by  before the
    // device unpauses.
    const std::size_t sample_count =
        static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);
    pcm_.assign(output, output + sample_count);
    std::free(output);

    channels_ = channels;
    sample_rate_ = sample_rate;

    // ── Open the SDL audio device under a 5 s deadline  ──────────
    // Request EXACTLY the decoded track's sample rate, channel count, and
    // AUDIO_S16SYS so the obtained spec matches what pcm_ already holds. We pass
    // allowed_changes = 0 so SDL is NOT permitted to substitute a different
    // rate/format/channel layout: if it cannot honor the request it fails the
    // open outright rather than handing back a spec whose frame math would
    // disagree with the decoded interleaved buffer. This keeps fill_block
    // correct by construction (frames computed from channels_ == the decoded
    // channels) and avoids any post-open resampling/relayout of pcm_.
    SDL_AudioSpec want{};
    want.freq = sample_rate_;
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(channels_);
    want.samples = 2048;  // device buffer in sample-frames (reasonable latency)
    want.callback = &MusicPlayer::sdl_audio_callback;
    want.userdata = this;

    // SDL_OpenAudioDevice is synchronous and fast, but  bounds
    // the whole open-and-start-playing attempt to 5 s. Capture a steady-clock
    // start before the open and re-check the elapsed time after the open (and
    // before unpausing): if the deadline has passed, abandon the attempt and
    // treat it as an open failure, closing the device if it happened to open.
    const auto open_start = std::chrono::steady_clock::now();
    const auto deadline = open_start + std::chrono::seconds(5);

    SDL_AudioSpec have{};
    SDL_AudioDeviceID device =
        SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);

    // A failed open (device == 0) is an open failure: emit one diagnostic that
    // includes the SDL-reported reason  and return with playing_ false.
    if (device == 0) {
        std::fprintf(stderr,
                     "MusicPlayer: failed to open audio device: %s\n",
                     SDL_GetError());
        return;
    }

    // Enforce the 5 s deadline: if the open took longer than the budget,
    // abandon it as an open failure — close the device and leave playing_
    // false. (Also covers a cooperative stop() requesting cancellation during a
    // slow open.)
    if (std::chrono::steady_clock::now() > deadline) {
        std::fprintf(stderr,
                     "MusicPlayer: audio device did not open within 5 s; "
                     "treating as an open failure\n");
        SDL_CloseAudioDevice(device);
        return;
    }

    // ── Publish before unpause  ─────────────────────────────────
    // pcm_ is already fully populated (above) and immutable from here on, so
    // the callback only ever reads a complete buffer. The device opened PAUSED
    // by default. Reset the read cursor, store the device id, then set playing_
    // with release ordering and unpause — the callback's acquire load of
    // playing_ pairs with this release, so it observes both playing_ == true
    // and the fully-written pcm_/cursor_ before it streams anything. Starting
    // the loop here begins gap-free playback via fill() → fill_block

    cursor_.store(0, std::memory_order_relaxed);
    device_ = device;
    playing_.store(true, std::memory_order_release);
    SDL_PauseAudioDevice(device_, 0);

}

void MusicPlayer::sdl_audio_callback(void* userdata, Uint8* stream, int len) {
    auto* self = static_cast<MusicPlayer*>(userdata);
    if (self == nullptr) {
        return;
    }

    // Translate the byte length SDL hands us into sample-frames. A defensive
    // guard against a zero channel count keeps the division safe.
    const int channels = self->channels_ > 0 ? self->channels_ : 1;
    const int frames =
        len / static_cast<int>(sizeof(int16_t) * static_cast<size_t>(channels));
    self->fill(reinterpret_cast<int16_t*>(stream), frames);
}

void MusicPlayer::fill(int16_t* out, int frames) {
    const int channels = channels_ > 0 ? channels_ : 1;
    const std::size_t samples = static_cast<std::size_t>(frames) *
                                static_cast<std::size_t>(channels);
    std::span<int16_t> out_span(out, samples);

    // Pre-start / post-stop, or with no decoded track, emit silence.
    if (!playing_.load(std::memory_order_acquire) || pcm_.empty()) {
        std::fill(out_span.begin(), out_span.end(), static_cast<int16_t>(0));
        return;
    }

    // Stream the immutable PCM through the pure, host-tested core: gap-free loop
    // + volume scaling (Properties 2 and 4). The cursor round-trips through the
    // atomic so the next callback resumes where this one stopped.
    std::size_t cursor = cursor_.load(std::memory_order_relaxed);
    fill_block(std::span<const int16_t>(pcm_), cursor,
               volume_.load(std::memory_order_relaxed), out_span);
    cursor_.store(cursor, std::memory_order_relaxed);
}

} // namespace heaven
