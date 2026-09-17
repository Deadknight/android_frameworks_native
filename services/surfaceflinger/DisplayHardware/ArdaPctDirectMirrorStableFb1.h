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
static constexpr size_t stable__kArdaPctMaxInflight = 1;
static constexpr const char* stable__kArdaVideoRouteSelectedProperty =
        "persist.sys.arda.video_route";
static constexpr const char* stable__kArdaVideoRouteActiveProperty =
        "sys.arda.video_route.active";

inline bool stable__ardaPctDirectRouteEnabled() {
    static const bool enabled = [] {
        const std::string selected =
                android::base::GetProperty(stable__kArdaVideoRouteSelectedProperty, "hdmi");
        const bool direct = selected != "txp";
        const char* active = direct ? "hdmi" : "txp";
        if (!android::base::SetProperty(stable__kArdaVideoRouteActiveProperty, active)) {
            ALOGW("ARDA_VIDEO_ROUTE_ACTIVE_PROPERTY_SET_FAIL selected=%s", active);
        }
        ALOGI("ARDA_VIDEO_ROUTE_BOOT selected=%s default=hdmi", active);
        return direct;
    }();
    return enabled;
}

struct stable__ArdaPctDirectInflight {
    uint64_t sourceId = 0;
    uint64_t cloneId = 0;
};

struct stable__ArdaPctDirectMirrorState {
    Mutex mutex;
    sp<IGraphicBufferProducer> sink;
    uint32_t sinkGeneration = 0;
    bool generationValid = false;
    bool directActive = false;
    bool txpBypassReady = false;
    // ARDA_PCT_DYNAMIC_SINK_GEOMETRY_V1
    bool sinkGeometryValid = false;
    bool sinkFallbackLogged = false;
    int32_t sinkWidth = 0;
    int32_t sinkHeight = 0;
    std::vector<stable__ArdaPctDirectInflight> inflight;
    uint64_t queueCount = 0;
    uint64_t reclaimCount = 0;
};

inline stable__ArdaPctDirectMirrorState& stable__ardaPctDirectMirrorState() {
    static stable__ArdaPctDirectMirrorState state;
    return state;
}

inline void stable__ardaPctDirectMirrorRegisterSink(const sp<IGraphicBufferProducer>& sink) {
    auto& s = stable__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    s.sink = sink;
    s.generationValid = false;
    s.directActive = false;
    s.txpBypassReady = false;
    s.sinkGeometryValid = false;
    s.sinkFallbackLogged = false;
    s.sinkWidth = 0;
    s.sinkHeight = 0;
    s.inflight.clear();
    ALOGI("ARDA_PCT_DIRECT_SINK_REGISTER route=%s",
          stable__ardaPctDirectRouteEnabled() ? "hdmi" : "txp");
}

inline void stable__ardaPctDirectMirrorUnregisterSink(const sp<IGraphicBufferProducer>& sink) {
    auto& s = stable__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    if (s.sink == sink) {
        s.sink.clear();
        s.generationValid = false;
        s.directActive = false;
        s.txpBypassReady = false;
        s.sinkGeometryValid = false;
        s.sinkFallbackLogged = false;
        s.sinkWidth = 0;
        s.sinkHeight = 0;
        s.inflight.clear();
        ALOGI("ARDA_PCT_DIRECT_SINK_UNREGISTER");
    }
}

inline void stable__ardaPctDirectMirrorSetGeneration(const sp<IGraphicBufferProducer>& sink,
                                             uint32_t generation) {
    auto& s = stable__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    if (s.sink == sink) {
        if (!s.generationValid || s.sinkGeneration != generation) {
            ALOGI("ARDA_PCT_DIRECT_SINK_GENERATION generation=%u", generation);
            s.sinkGeometryValid = false;
            s.sinkFallbackLogged = false;
            s.sinkWidth = 0;
            s.sinkHeight = 0;
        }
        s.sinkGeneration = generation;
        s.generationValid = true;
    }
}

inline bool stable__ardaPctDirectMirrorShouldSuppressTxp(const sp<IGraphicBufferProducer>& sink) {
    if (!stable__ardaPctDirectRouteEnabled()) return false;
    auto& s = stable__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    return s.sink == sink && s.directActive;
}

inline void stable__ardaPctDirectMirrorArmTxpBypass(const sp<IGraphicBufferProducer>& sink) {
    if (!stable__ardaPctDirectRouteEnabled()) return;
    auto& s = stable__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    if (s.sink == sink && s.directActive && !s.txpBypassReady) {
        s.txpBypassReady = true;
        ALOGI("ARDA_PCT_TXP_BYPASS_ARMED");
    }
}

inline bool stable__ardaPctDirectMirrorTxpBypassReady() {
    if (!stable__ardaPctDirectRouteEnabled()) return false;
    auto& s = stable__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    return s.txpBypassReady;
}

inline status_t stable__ardaPctRestoreDetachedBuffersLocked(
        stable__ArdaPctDirectMirrorState& s,
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

inline status_t stable__ardaPctDirectMirrorReclaimLocked(
        stable__ArdaPctDirectMirrorState& s, uint64_t sourceId) {
    const auto target = std::find_if(
            s.inflight.begin(), s.inflight.end(),
            [sourceId](const stable__ArdaPctDirectInflight& item) {
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
            stable__ardaPctRestoreDetachedBuffersLocked(s, detached);
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

    status_t restoreErr = stable__ardaPctRestoreDetachedBuffersLocked(s, detached);
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
            [sourceId, cloneId](const stable__ArdaPctDirectInflight& item) {
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

// ARDA_PCT_DYNAMIC_SINK_GEOMETRY_V1:
// Direct clone is zero-copy only when the physical FB and encoder input
// surface have identical geometry. Geometry mismatch leaves directActive
// false, so normal VDS composes/scales into codec-owned sink buffers.
inline bool stable__ardaPctDirectMirrorSinkMatchesSourceLocked(
        stable__ArdaPctDirectMirrorState& s,
        const sp<GraphicBuffer>& source) {
    if (s.sink == nullptr || source == nullptr) return false;

    if (!s.sinkGeometryValid) {
        int width = 0;
        int height = 0;
        const status_t widthErr = s.sink->query(NATIVE_WINDOW_WIDTH, &width);
        const status_t heightErr = s.sink->query(NATIVE_WINDOW_HEIGHT, &height);
        if (widthErr != NO_ERROR || heightErr != NO_ERROR || width <= 0 || height <= 0) {
            if (!s.sinkFallbackLogged) {
                ALOGW("ARDA_PCT_DYNAMIC_FALLBACK reason=sink-query source=%ux%u "
                      "width_err=%d height_err=%d sink=%dx%d",
                      source->width, source->height, widthErr, heightErr, width, height);
                s.sinkFallbackLogged = true;
            }
            return false;
        }
        s.sinkWidth = width;
        s.sinkHeight = height;
        s.sinkGeometryValid = true;
        ALOGI("ARDA_PCT_DYNAMIC_SINK_GEOMETRY sink=%dx%d source=%ux%u",
              s.sinkWidth, s.sinkHeight, source->width, source->height);
    }

    const bool exact =
            s.sinkWidth == static_cast<int32_t>(source->width) &&
            s.sinkHeight == static_cast<int32_t>(source->height);
    if (!exact && !s.sinkFallbackLogged) {
        ALOGI("ARDA_PCT_DYNAMIC_FALLBACK reason=geometry-mismatch "
              "source=%ux%u stride=%u sink=%dx%d backend=normal-vds",
              source->width, source->height, source->stride,
              s.sinkWidth, s.sinkHeight);
        s.sinkFallbackLogged = true;
    }
    return exact;
}

inline status_t stable__ardaPctDirectMirrorQueue(const sp<GraphicBuffer>& source,
                                         const sp<Fence>& acquireFence) {
    if (!stable__ardaPctDirectRouteEnabled()) return NO_ERROR;
    if (source == nullptr || source->handle == nullptr) return BAD_VALUE;

    auto& s = stable__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    if (s.sink == nullptr || !s.generationValid) return NO_INIT;
    if (!stable__ardaPctDirectMirrorSinkMatchesSourceLocked(s, source)) {
        return NO_ERROR;
    }

    // ARDA_PCT_V23_SINGLE_INFLIGHT_COMPAT: keep at most one codec clone outstanding. V2.2's
    // source-keyed two-inflight policy can leave SurfaceFlinger polling
    // detachNextBuffer for seconds. Reclaim the exact prior clone, including
    // its real consumer release fence, before queueing the next physical source.
    if (!s.inflight.empty()) {
        const uint64_t priorSourceId = s.inflight.front().sourceId;
        const status_t reclaim = stable__ardaPctDirectMirrorReclaimLocked(s, priorSourceId);
        if (reclaim != NO_ERROR) return reclaim;
    }

    if (s.inflight.size() >= stable__kArdaPctMaxInflight) {
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
                    systemTime(), false, HAL_DATASPACE_UNKNOWN,
                    Rect(source->width, source->height),
                    NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, fence),
            &qbo);
    if (err != NO_ERROR) {
        s.sink->cancelBuffer(slot, fence);
        ALOGE("ARDA_PCT_DIRECT_QUEUE_FAIL err=%d slot=%d source=%" PRIu64, err,
              slot, source->getId());
        return err;
    }

    s.inflight.push_back(
            stable__ArdaPctDirectInflight{source->getId(), clone->getId()});
    s.directActive = true;
    ++s.queueCount;
    ALOGI("ARDA_PCT_DIRECT_QUEUE_OK frame=%" PRIu64 " source=%" PRIu64
          " clone=%" PRIu64 " slot=%d inflight=%zu max=%zu",
          s.queueCount, source->getId(), clone->getId(), slot,
          s.inflight.size(), stable__kArdaPctMaxInflight);
    return NO_ERROR;
}

} // namespace android
