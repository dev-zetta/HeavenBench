#ifndef HEAVEN_DLSS_STATUS_H
#define HEAVEN_DLSS_STATUS_H

#include "dlss_provider.h"
#include "engine/frame_generation.h"

namespace heaven {

// The caller holds the runtime state lock and supplies its current snapshot.
// A native SDK failure can arrive after prepare_present succeeded. Downgrade
// that previously usable capability without overriding stricter app coverage
// failures or promoting a provider whose temporal inputs are still unusable.
inline void apply_dlss_provider_status(FrameGenerationState& state,
        FrameGenerationStatus& snapshot, const DlssProviderStatus& provider) {
    if (snapshot.requested != FrameGenerationMode::Dlss) return;
    if (snapshot.capability.mode == FrameGenerationMode::Dlss &&
            snapshot.capability.available && !provider.available) {
        state.set_capability({FrameGenerationMode::Dlss, false, provider.reason});
        snapshot = state.status();
    }
    snapshot.counters.generated_frames_available = false;
    snapshot.counters.presentation_submissions_available = false;
    if (provider.initialized) snapshot.sdk_presented_frames = provider.total_sdk_presented_frames;
    else snapshot.sdk_presented_frames.reset();
}

} // namespace heaven

#endif // HEAVEN_DLSS_STATUS_H
