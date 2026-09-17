#pragma once

// ARDA_DUAL_CAPTURE_PROFILE_FB1_B21_V1_2

#include "ArdaPctDirectMirrorStableFb1.h"
#include "ArdaPctDirectMirrorPerformanceB21.h"

namespace android {

enum class ArdaCaptureProfile {
    StableFb1,
    PerformanceB21,
};

static constexpr const char* kArdaCaptureProfileSelectedProperty =
        "persist.sys.arda.capture_profile";
static constexpr const char* kArdaCaptureProfileActiveProperty =
        "sys.arda.capture_profile.active";

inline ArdaCaptureProfile ardaCaptureProfile() {
    static const ArdaCaptureProfile profile = [] {
        const std::string selected =
                android::base::GetProperty(
                        kArdaCaptureProfileSelectedProperty,
                        "stable");

        const bool performance = selected == "performance";
        const char* active = performance ? "performance" : "stable";

        if (!android::base::SetProperty(
                    kArdaCaptureProfileActiveProperty,
                    active)) {
            ALOGW("ARDA_CAPTURE_PROFILE_ACTIVE_PROPERTY_SET_FAIL active=%s",
                  active);
        }

        ALOGI("ARDA_CAPTURE_PROFILE_BOOT selected_raw=%s active=%s "
              "default=stable stable_backend=V23_FB1 "
              "performance_backend=B2_1",
              selected.c_str(), active);

        return performance
                ? ArdaCaptureProfile::PerformanceB21
                : ArdaCaptureProfile::StableFb1;
    }();

    return profile;
}

inline bool ardaCapturePerformanceProfileEnabled() {
    return ardaCaptureProfile() == ArdaCaptureProfile::PerformanceB21;
}

inline bool ardaPctDirectRouteEnabled() {
    return ardaCapturePerformanceProfileEnabled()
            ? perf__ardaPctDirectRouteEnabled()
            : stable__ardaPctDirectRouteEnabled();
}

inline void ardaPctDirectMirrorRegisterSink(
        const sp<IGraphicBufferProducer>& sink) {
    if (ardaCapturePerformanceProfileEnabled()) {
        perf__ardaPctDirectMirrorRegisterSink(sink);
    } else {
        stable__ardaPctDirectMirrorRegisterSink(sink);
    }
}

inline void ardaPctDirectMirrorUnregisterSink(
        const sp<IGraphicBufferProducer>& sink) {
    if (ardaCapturePerformanceProfileEnabled()) {
        perf__ardaPctDirectMirrorUnregisterSink(sink);
    } else {
        stable__ardaPctDirectMirrorUnregisterSink(sink);
    }
}

inline void ardaPctDirectMirrorSetGeneration(
        const sp<IGraphicBufferProducer>& sink,
        uint32_t generation) {
    if (ardaCapturePerformanceProfileEnabled()) {
        perf__ardaPctDirectMirrorSetGeneration(sink, generation);
    } else {
        stable__ardaPctDirectMirrorSetGeneration(sink, generation);
    }
}

inline bool ardaPctDirectMirrorShouldSuppressTxp(
        const sp<IGraphicBufferProducer>& sink) {
    return ardaCapturePerformanceProfileEnabled()
            ? perf__ardaPctDirectMirrorShouldSuppressTxp(sink)
            : stable__ardaPctDirectMirrorShouldSuppressTxp(sink);
}

inline void ardaPctDirectMirrorArmTxpBypass(
        const sp<IGraphicBufferProducer>& sink) {
    if (ardaCapturePerformanceProfileEnabled()) {
        perf__ardaPctDirectMirrorArmTxpBypass(sink);
    } else {
        stable__ardaPctDirectMirrorArmTxpBypass(sink);
    }
}

inline bool ardaPctDirectMirrorTxpBypassReady() {
    return ardaCapturePerformanceProfileEnabled()
            ? perf__ardaPctDirectMirrorTxpBypassReady()
            : stable__ardaPctDirectMirrorTxpBypassReady();
}

inline status_t ardaPctDirectMirrorQueue(
        const sp<GraphicBuffer>& source,
        const sp<Fence>& acquireFence) {
    return ardaCapturePerformanceProfileEnabled()
            ? perf__ardaPctDirectMirrorQueue(source, acquireFence)
            : stable__ardaPctDirectMirrorQueue(source, acquireFence);
}

} // namespace android
