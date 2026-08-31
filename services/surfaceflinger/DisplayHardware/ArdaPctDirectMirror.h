#pragma once

#include <android-base/properties.h>
#include <gui/IGraphicBufferProducer.h>
#include <system/window.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <ui/Rect.h>
#include <utils/Errors.h>
#include <utils/Mutex.h>
#include <utils/Timers.h>

#include <algorithm>
#include <inttypes.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace android {

// ARDA_PCT_V2_2_TWO_INFLIGHT
// ARDA_PCT_V23_SINGLE_INFLIGHT_COMPAT: restore conservative known-responsive producer contract.
static constexpr size_t kArdaPctMaxInflight = 1;
static constexpr const char* kArdaVideoRouteSelectedProperty =
        "persist.sys.arda.video_route";
static constexpr const char* kArdaVideoRouteActiveProperty =
        "sys.arda.video_route.active";

inline bool ardaPctDirectRouteEnabled() {
    static const bool enabled = [] {
        const std::string selected =
                android::base::GetProperty(kArdaVideoRouteSelectedProperty, "hdmi");
        const bool direct = selected != "txp";
        const char* active = direct ? "hdmi" : "txp";
        if (!android::base::SetProperty(kArdaVideoRouteActiveProperty, active)) {
            ALOGW("ARDA_VIDEO_ROUTE_ACTIVE_PROPERTY_SET_FAIL selected=%s", active);
        }
        ALOGI("ARDA_VIDEO_ROUTE_BOOT selected=%s default=hdmi", active);
        return direct;
    }();
    return enabled;
}

struct ArdaPctDirectInflight {
    uint64_t sourceId = 0;
    uint64_t cloneId = 0;
};

struct ArdaPctDirectMirrorState {
    Mutex mutex;
    sp<IGraphicBufferProducer> sink;
    uint32_t sinkGeneration = 0;
    bool generationValid = false;
    bool directActive = false;
    bool txpBypassReady = false;
    std::vector<ArdaPctDirectInflight> inflight;
    uint64_t queueCount = 0;
    uint64_t reclaimCount = 0;
};

inline ArdaPctDirectMirrorState& ardaPctDirectMirrorState() {
    static ArdaPctDirectMirrorState state;
    return state;
}

inline void ardaPctDirectMirrorRegisterSink(const sp<IGraphicBufferProducer>& sink) {
    auto& s = ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    s.sink = sink;
    s.generationValid = false;
    s.directActive = false;
    s.txpBypassReady = false;
    s.inflight.clear();
    ALOGI("ARDA_PCT_DIRECT_SINK_REGISTER route=%s",
          ardaPctDirectRouteEnabled() ? "hdmi" : "txp");
}

inline void ardaPctDirectMirrorUnregisterSink(const sp<IGraphicBufferProducer>& sink) {
    auto& s = ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    if (s.sink == sink) {
        s.sink.clear();
        s.generationValid = false;
        s.directActive = false;
        s.txpBypassReady = false;
        s.inflight.clear();
        ALOGI("ARDA_PCT_DIRECT_SINK_UNREGISTER");
    }
}

inline void ardaPctDirectMirrorSetGeneration(const sp<IGraphicBufferProducer>& sink,
                                             uint32_t generation) {
    auto& s = ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    if (s.sink == sink) {
        if (!s.generationValid || s.sinkGeneration != generation) {
            ALOGI("ARDA_PCT_DIRECT_SINK_GENERATION generation=%u", generation);
        }
        s.sinkGeneration = generation;
        s.generationValid = true;
    }
}

inline bool ardaPctDirectMirrorShouldSuppressTxp(const sp<IGraphicBufferProducer>& sink) {
    if (!ardaPctDirectRouteEnabled()) return false;
    auto& s = ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    return s.sink == sink && s.directActive;
}

inline void ardaPctDirectMirrorArmTxpBypass(const sp<IGraphicBufferProducer>& sink) {
    if (!ardaPctDirectRouteEnabled()) return;
    auto& s = ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    if (s.sink == sink && s.directActive && !s.txpBypassReady) {
        s.txpBypassReady = true;
        ALOGI("ARDA_PCT_TXP_BYPASS_ARMED");
    }
}

inline bool ardaPctDirectMirrorTxpBypassReady() {
    if (!ardaPctDirectRouteEnabled()) return false;
    auto& s = ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    return s.txpBypassReady;
}

inline status_t ardaPctRestoreDetachedBuffersLocked(
        ArdaPctDirectMirrorState& s,
        const std::vector<std::pair<sp<GraphicBuffer>, sp<Fence>>>& detached) {
    for (const auto& entry : detached) {
        if (entry.first == nullptr) continue;
        entry.first->setGenerationNumber(s.sinkGeneration);
        int slot = -1;
        status_t err = s.sink->attachBuffer(&slot, entry.first);
        if (err != NO_ERROR) {
            ALOGE("ARDA_PCT_DIRECT_RESTORE_ATTACH_FAIL err=%d id=%" PRIu64, err,
                  entry.first->getId());
            return err;
        }
        err = s.sink->cancelBuffer(
                slot, entry.second != nullptr ? entry.second : Fence::NO_FENCE);
        if (err != NO_ERROR) {
            ALOGE("ARDA_PCT_DIRECT_RESTORE_CANCEL_FAIL err=%d id=%" PRIu64, err,
                  entry.first->getId());
            return err;
        }
    }
    return NO_ERROR;
}

inline status_t ardaPctDirectMirrorReclaimLocked(
        ArdaPctDirectMirrorState& s, uint64_t sourceId) {
    const auto target = std::find_if(
            s.inflight.begin(), s.inflight.end(),
            [sourceId](const ArdaPctDirectInflight& item) {
                return item.sourceId == sourceId;
            });
    if (target == s.inflight.end()) return NO_ERROR;

    const uint64_t cloneId = target->cloneId;
    std::vector<std::pair<sp<GraphicBuffer>, sp<Fence>>> detached;
    sp<Fence> targetFence = Fence::NO_FENCE;
    bool found = false;

    for (int attempt = 0; attempt < 5000 && !found; ++attempt) {
        sp<GraphicBuffer> buffer;
        sp<Fence> fence;
        status_t err = s.sink->detachNextBuffer(&buffer, &fence);
        if (err == NO_MEMORY) {
            usleep(1000);
            continue;
        }
        if (err != NO_ERROR) {
            ardaPctRestoreDetachedBuffersLocked(s, detached);
            ALOGE("ARDA_PCT_DIRECT_DETACHNEXT_FAIL err=%d source=%" PRIu64,
                  err, sourceId);
            return err;
        }
        if (buffer != nullptr && buffer->getId() == cloneId) {
            targetFence = fence != nullptr ? fence : Fence::NO_FENCE;
            found = true;
        } else {
            detached.emplace_back(buffer, fence);
        }
    }

    status_t restoreErr = ardaPctRestoreDetachedBuffersLocked(s, detached);
    if (restoreErr != NO_ERROR) return restoreErr;

    if (!found) {
        ALOGE("ARDA_PCT_DIRECT_RECLAIM_TIMEOUT source=%" PRIu64
              " clone=%" PRIu64, sourceId, cloneId);
        return TIMED_OUT;
    }

    if (targetFence != nullptr && targetFence->isValid()) {
        const int waitResult =
                targetFence->waitForever("ARDA_PCT_DIRECT_RECLAIM_ON_REUSE");
        if (waitResult != NO_ERROR) {
            ALOGE("ARDA_PCT_DIRECT_RELEASE_FENCE_WAIT_FAIL err=%d source=%" PRIu64,
                  waitResult, sourceId);
            return waitResult;
        }
    }

    const auto eraseTarget = std::find_if(
            s.inflight.begin(), s.inflight.end(),
            [sourceId, cloneId](const ArdaPctDirectInflight& item) {
                return item.sourceId == sourceId && item.cloneId == cloneId;
            });
    if (eraseTarget != s.inflight.end()) {
        s.inflight.erase(eraseTarget);
    }

    ++s.reclaimCount;
    ALOGI("ARDA_PCT_DIRECT_RECLAIM_OK frame=%" PRIu64
          " source=%" PRIu64 " reason=SOURCE_REUSE inflight_after=%zu",
          s.reclaimCount, sourceId, s.inflight.size());
    s.directActive = !s.inflight.empty();
    return NO_ERROR;
}

inline status_t ardaPctDirectMirrorQueue(const sp<GraphicBuffer>& source,
                                         const sp<Fence>& acquireFence) {
    if (!ardaPctDirectRouteEnabled()) return NO_ERROR;
    if (source == nullptr || source->handle == nullptr) return BAD_VALUE;
    if (source->width != 1920 || source->height != 1080 || source->stride != 1920) {
        return BAD_VALUE;
    }

    auto& s = ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    if (s.sink == nullptr || !s.generationValid) return NO_INIT;

    // ARDA_PCT_V23_SINGLE_INFLIGHT_COMPAT: keep at most one codec clone outstanding. V2.2's
    // source-keyed two-inflight policy can leave SurfaceFlinger polling
    // detachNextBuffer for seconds. Reclaim the exact prior clone, including
    // its real consumer release fence, before queueing the next physical source.
    if (!s.inflight.empty()) {
        const uint64_t priorSourceId = s.inflight.front().sourceId;
        const status_t reclaim = ardaPctDirectMirrorReclaimLocked(s, priorSourceId);
        if (reclaim != NO_ERROR) return reclaim;
    }

    if (s.inflight.size() >= kArdaPctMaxInflight) {
        ALOGW("ARDA_PCT_DIRECT_INFLIGHT_LIMIT source=%" PRIu64 " inflight=%zu",
              source->getId(), s.inflight.size());
        return WOULD_BLOCK;
    }

    sp<GraphicBuffer> clone = sp<GraphicBuffer>::make(
            source->handle, GraphicBuffer::CLONE_HANDLE, source->width, source->height,
            static_cast<PixelFormat>(source->format), source->layerCount, source->usage,
            source->stride);
    if (clone == nullptr || clone->initCheck() != NO_ERROR) {
        ALOGE("ARDA_PCT_DIRECT_CLONE_FAIL source=%" PRIu64, source->getId());
        return NO_MEMORY;
    }

    clone->setGenerationNumber(s.sinkGeneration);

    int slot = -1;
    status_t err = s.sink->attachBuffer(&slot, clone);
    if (err != NO_ERROR) {
        ALOGE("ARDA_PCT_DIRECT_ATTACH_FAIL err=%d gen=%u source=%" PRIu64, err,
              s.sinkGeneration, source->getId());
        return err;
    }

    IGraphicBufferProducer::QueueBufferOutput qbo;
    const sp<Fence> fence = acquireFence != nullptr ? acquireFence : Fence::NO_FENCE;
    err = s.sink->queueBuffer(
            slot,
            IGraphicBufferProducer::QueueBufferInput(
                    systemTime(), false, HAL_DATASPACE_UNKNOWN, Rect(1920, 1080),
                    NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, fence),
            &qbo);
    if (err != NO_ERROR) {
        s.sink->cancelBuffer(slot, fence);
        ALOGE("ARDA_PCT_DIRECT_QUEUE_FAIL err=%d slot=%d source=%" PRIu64, err,
              slot, source->getId());
        return err;
    }

    s.inflight.push_back(
            ArdaPctDirectInflight{source->getId(), clone->getId()});
    s.directActive = true;
    ++s.queueCount;
    ALOGI("ARDA_PCT_DIRECT_QUEUE_OK frame=%" PRIu64 " source=%" PRIu64
          " clone=%" PRIu64 " slot=%d inflight=%zu max=%zu",
          s.queueCount, source->getId(), clone->getId(), slot,
          s.inflight.size(), kArdaPctMaxInflight);
    return NO_ERROR;
}

} // namespace android
