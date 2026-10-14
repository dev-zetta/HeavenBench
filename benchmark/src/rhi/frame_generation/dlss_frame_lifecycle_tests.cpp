#include "dlss_frame_lifecycle.h"

#include <atomic>
#include <cstdio>
#include <exception>
#include <vector>

namespace {
using namespace heaven;

bool check(bool value, const char* reason) {
    if (!value) std::fprintf(stderr, "DLSS frame lifecycle: FAIL: %s\n", reason);
    return value;
}

struct FrameToken {
    std::atomic<uint32_t> counter{0};
    operator uint32_t() const { return counter.load(); }
};
constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 6;

struct SdkRing {
    std::mutex mtxFrameHandle;
    uint32_t frameHandleIndex = 0;
    std::array<FrameToken, MAX_FRAMES_IN_FLIGHT> frameHandles;
    unsigned calls = 0;

    FrameToken* acquire(uint32_t index) {
        const std::scoped_lock lock(mtxFrameHandle);
        ++calls;
        // Independent oracle: a new ID rotates six mutable objects, while
        // another request for the current ID returns that same native object.
        if (frameHandles[frameHandleIndex].counter.load() != index) {
            frameHandleIndex = (frameHandleIndex + 1) % frameHandles.size();
            frameHandles[frameHandleIndex].counter.store(index);
        }
        return &frameHandles[frameHandleIndex];
    }
};

struct Marker { uint64_t source; DlssLatencyMarker kind; };
struct Fixture {
    SdkRing sdk;
    DlssTokenReservations reservations;
    std::vector<Marker> markers;
    unsigned sleeps = 0;
    bool simulation_start_fails = false;
    unsigned failed_markers = 0;

    explicit Fixture(uint64_t first_frame = 1) : reservations(first_frame) {}
    DlssFrameToken acquire() {
        const auto lease = reservations.reserve();
        if (!lease) return {};
        const auto source = lease->logical_frame;
        auto* native = sdk.acquire(static_cast<uint32_t>(source));
        ++sleeps;
        DlssFrameToken token{native, source, static_cast<uint32_t>(source), lease};
        mark(token, DlssLatencyMarker::SimulationStart);
        return token;
    }
    bool mark(const DlssFrameToken& token, DlssLatencyMarker marker) {
        markers.push_back({static_cast<uint32_t>(*static_cast<FrameToken*>(token.native_token)), marker});
        if (simulation_start_fails && marker == DlssLatencyMarker::SimulationStart) {
            ++failed_markers;
            return false;
        }
        return true;
    }
    unsigned count(uint64_t source, DlssLatencyMarker marker) const {
        unsigned result = 0;
        for (const auto& event : markers) if (event.source == source && event.kind == marker) ++result;
        return result;
    }
};

bool run() {
    bool ok = true;
    Fixture f;
    DlssProspectiveFrame prospective;
    const auto begin = [&] { return prospective.begin([&] { return f.acquire(); }); };
    const auto submit = [&] { prospective.begin_submission([&](const auto& token, auto marker) { f.mark(token, marker); }); };
    begin(); submit();
    auto packet_a = prospective.accept();
    auto pending_a = packet_a;
    auto last_tag_a = packet_a;
    f.mark(packet_a, DlssLatencyMarker::RenderSubmitEnd);
    begin(); submit();
    auto packet_b = prospective.accept();
    const auto first_c = begin();
    for (unsigned attempt = 0; attempt != 100; ++attempt) {
        const auto retry = begin();
        ok &= check(retry.native_token == first_c.native_token && retry.application_frame == 3,
                    "rejected attempt replaced the prospective logical ticket");
    }
    ok &= check(f.sdk.calls == 3 && f.sleeps == 3, "rejected attempts rotated SDK tokens or repeated Reflex sleep");
    ok &= check(static_cast<uint32_t>(*static_cast<FrameToken*>(packet_a.native_token)) == 1 &&
                static_cast<uint32_t>(*static_cast<FrameToken*>(packet_b.native_token)) == 2,
                "rejected attempts overwrote accepted queued SDK tokens");
    ok &= check(f.count(3, DlssLatencyMarker::SimulationStart) == 1 &&
                f.count(3, DlssLatencyMarker::SimulationEnd) == 0 &&
                f.count(3, DlssLatencyMarker::RenderSubmitStart) == 0,
                "retry markers prematurely closed or repeated the simulation interval");
    submit(); submit();
    auto packet_c = prospective.accept();
    ok &= check(packet_c.application_frame == packet_b.application_frame + 1,
                "rejected attempt IDs broke accepted temporal continuity");
    ok &= check(f.count(3, DlssLatencyMarker::SimulationEnd) == 1 &&
                f.count(3, DlssLatencyMarker::RenderSubmitStart) == 1,
                "accepted submission repeated Reflex boundaries");

    // Retiring the callback's copy is insufficient: pending presentation and
    // valid-until-present tag owners must continue pinning the native object.
    packet_a = {};
    for (uint64_t source = 4; source <= 6; ++source) {
        auto canceled = f.acquire();
        ok &= check(canceled.native_token && canceled.application_frame == source, "canceled reservation blocked a free SDK slot");
    }
    const auto calls_before_block = f.sdk.calls;
    ok &= check(!f.acquire().native_token && f.sdk.calls == calls_before_block,
                "presentation owner did not pin the SDK slot before rotation");
    ok &= check(static_cast<uint32_t>(*static_cast<FrameToken*>(pending_a.native_token)) == 1,
                "native callback retirement expired pending PresentStart/End token");
    f.mark(pending_a, DlssLatencyMarker::PresentStart);
    f.mark(pending_a, DlssLatencyMarker::PresentEnd);
    last_tag_a = {};
    pending_a = {};
    auto recovered = f.acquire();
    ok &= check(recovered.native_token && recovered.application_frame == 7,
                "PresentEnd retirement did not recover token capacity");
    std::vector<DlssLatencyMarker> chain;
    for (const auto& marker : f.markers) if (marker.source == 1) chain.push_back(marker.kind);
    ok &= check(chain == std::vector<DlssLatencyMarker>{DlssLatencyMarker::SimulationStart,
        DlssLatencyMarker::SimulationEnd, DlssLatencyMarker::RenderSubmitStart,
        DlssLatencyMarker::RenderSubmitEnd, DlssLatencyMarker::PresentStart, DlssLatencyMarker::PresentEnd},
        "accepted ticket did not retain one ordered complete Reflex chain");

    // Ordinary fallback, failure and drain cancellation relinquish only the
    // prospective owner; any outstanding packet remains independently pinned.
    Fixture canceled;
    DlssProspectiveFrame canceled_frame;
    canceled_frame.begin([&] { return canceled.acquire(); });
    canceled_frame.cancel();
    const auto next = canceled_frame.begin([&] { return canceled.acquire(); });
    ok &= check(next.native_token && next.application_frame == 2,
                "canceled logical frame reused an SDK index");
    canceled_frame.cancel();
    Fixture zero(0), overflow(uint64_t(UINT32_MAX) + 1), bounds(UINT32_MAX);
    ok &= check(!zero.acquire().native_token && !overflow.acquire().native_token &&
                !zero.sdk.calls && !overflow.sdk.calls, "invalid SDK frame index reached the native allocator");
    auto final = bounds.acquire();
    ok &= check(final.native_token && final.application_frame == UINT32_MAX && !bounds.acquire().native_token &&
                bounds.sdk.calls == 1 && bounds.reservations.exhausted(), "exhausted SDK identity wrapped onto an old frame");
    for (const bool rotated : {false, true}) {
        Fixture failed;
        auto uncertain = failed.reservations.reserve();
        if (rotated) failed.sdk.acquire(1);
        failed.reservations.disable();
        uncertain.reset();
        const auto before_retry = failed.sdk.calls;
        ok &= check(!failed.acquire().native_token && failed.sdk.calls == before_retry,
                    "failed SDK acquisition retried an uncertain ring position");
    }
    Fixture marker_failure;
    marker_failure.simulation_start_fails = true;
    DlssProspectiveFrame failed_marker_frame;
    const auto first_failed_marker = failed_marker_frame.begin([&] { return marker_failure.acquire(); });
    for (unsigned retry = 0; retry != 10; ++retry)
        failed_marker_frame.begin([&] { return marker_failure.acquire(); });
    ok &= check(first_failed_marker.native_token && marker_failure.sdk.calls == 1 && marker_failure.failed_markers == 1 &&
                marker_failure.count(1, DlssLatencyMarker::SimulationStart) == 1,
                "acquired marker-failure ticket blindly retried SDK allocation");
    Fixture reloaded;
    {
        DlssProspectiveFrame old_runtime;
        old_runtime.begin([&] { return reloaded.acquire(); });
        auto accepted = old_runtime.accept();
        ok &= check(accepted.application_frame == 1, "reload fixture did not seed first provider source");
    }
    DlssProspectiveFrame new_runtime;
    const auto after_reload = new_runtime.begin([&] { return reloaded.acquire(); });
    ok &= check(after_reload.native_token && after_reload.application_frame == 2 && reloaded.sdk.calls == 2,
                "runtime recreation restarted the provider token identity");
    return ok;
}
} // namespace

int main() try {
    if (!run()) return 1;
    std::puts("DLSS frame lifecycle: PASS (pinned SDK ring,100 rejected attempts,leases,markers,bounds)");
} catch (const std::exception& error) {
    std::fprintf(stderr, "DLSS frame lifecycle: ERROR: %s\n", error.what());
    return 2;
}
