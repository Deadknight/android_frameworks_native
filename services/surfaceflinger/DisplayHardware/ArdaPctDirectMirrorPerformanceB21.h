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
#include <chrono>
#include <condition_variable>
#include <inttypes.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace android {

static constexpr size_t perf__kArdaPctMaxInflight = 2;
static constexpr size_t perf__kArdaPctMaxDetachBurst = 16;
static constexpr int perf__kArdaPctWorkerPollIntervalMs = 2;
static constexpr size_t perf__kArdaPctMaxParkedUnknown = 2;
static constexpr size_t perf__kArdaPctMaxPendingSources = 2;
static constexpr const char* perf__kArdaVideoRouteSelectedProperty = "persist.sys.arda.video_route";
static constexpr const char* perf__kArdaVideoRouteActiveProperty = "sys.arda.video_route.active";

inline bool perf__ardaPctDirectRouteEnabled() {
    static const bool enabled = [] {
        const std::string selected =
                android::base::GetProperty(perf__kArdaVideoRouteSelectedProperty, "hdmi");
        const bool direct = selected != "txp";
        const char* active = direct ? "hdmi" : "txp";
        if (!android::base::SetProperty(perf__kArdaVideoRouteActiveProperty, active)) {
            ALOGW("ARDA_VIDEO_ROUTE_ACTIVE_PROPERTY_SET_FAIL selected=%s", active);
        }
        ALOGI("ARDA_VIDEO_ROUTE_BOOT selected=%s default=hdmi", active);
        return direct;
    }();
    return enabled;
}

struct perf__ArdaPctDirectInflight {
    uint64_t sourceId = 0;
    uint64_t cloneId = 0;
    bool returned = false;
    sp<GraphicBuffer> returnedBuffer;
    sp<Fence> releaseFence = Fence::NO_FENCE;
};

struct perf__ArdaPctDirectPending {
    bool valid = false;
    uint64_t sourceId = 0;
    uint64_t cloneId = 0;
    uint64_t sequence = 0;
    uint32_t generation = 0;
    sp<GraphicBuffer> clone;
    sp<Fence> acquireFence = Fence::NO_FENCE;
};

struct perf__ArdaPctDirectWorkerContext {
    std::mutex mutex;
    std::condition_variable cv;
    bool wakeRequested = false;
    bool stopRequested = false;
    std::mutex producerIoMutex;
    uint64_t sessionId = 0;
    sp<IGraphicBufferProducer> sink;
};

struct perf__ArdaPctDirectMirrorState {
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
    std::vector<perf__ArdaPctDirectInflight> inflight;
    std::vector<perf__ArdaPctDirectPending> pending;
    std::shared_ptr<perf__ArdaPctDirectWorkerContext> worker;
    uint64_t nextWorkerSessionId = 0;
    uint64_t queueCount = 0;
    uint64_t reclaimCount = 0;
    uint64_t returnCount = 0;
    uint64_t busyDropCount = 0;
    uint64_t fullDropCount = 0;
    uint64_t producerBusyDropCount = 0;
    uint64_t workerBatchCount = 0;
    uint64_t pendingAcceptCount = 0;
    uint64_t pendingReplaceCount = 0;
    uint64_t pendingEvictCount = 0;
    uint64_t stalePendingDropCount = 0;
    uint64_t pendingSequence = 0;
    uint64_t lastSubmittedSequence = 0;
    uint64_t asyncSubmitBlockedSourceCount = 0;
    uint64_t asyncSubmitBlockedFullCount = 0;
    uint64_t asyncSubmitFailCount = 0;
};

inline perf__ArdaPctDirectMirrorState& perf__ardaPctDirectMirrorState() {
    static perf__ArdaPctDirectMirrorState state;
    return state;
}

inline bool perf__ardaPctDirectMirrorWorkerSessionCurrentLocked(
        const perf__ArdaPctDirectMirrorState& s,
        const std::shared_ptr<perf__ArdaPctDirectWorkerContext>& worker) {
    return worker != nullptr && s.worker == worker && s.sink == worker->sink;
}

inline void perf__ardaPctDirectMirrorWakeWorker(
        const std::shared_ptr<perf__ArdaPctDirectWorkerContext>& worker) {
    if (!worker) return;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->wakeRequested = true;
    }
    worker->cv.notify_one();
}

inline void perf__ardaPctDirectMirrorStopWorker(
        const std::shared_ptr<perf__ArdaPctDirectWorkerContext>& worker) {
    if (!worker) return;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->stopRequested = true;
        worker->wakeRequested = true;
    }
    worker->cv.notify_one();
}

inline void perf__ardaPctDirectMirrorRetireSignaledLocked(perf__ArdaPctDirectMirrorState& s) {
    for (auto it = s.inflight.begin(); it != s.inflight.end();) {
        if (!it->returned) {
            ++it;
            continue;
        }
        const sp<Fence>& releaseFence = it->releaseFence;
        const bool ready = releaseFence == nullptr || !releaseFence->isValid() ||
                releaseFence->wait(0) == NO_ERROR;
        if (!ready) {
            ++it;
            continue;
        }
        ++s.reclaimCount;
        ALOGI("ARDA_PCT_V24_2_RECLAIM_OK frame=%" PRIu64
              " source=%" PRIu64 " clone=%" PRIu64 " inflight_before=%zu",
              s.reclaimCount, it->sourceId, it->cloneId, s.inflight.size());
        it = s.inflight.erase(it);
    }
    s.directActive = !s.inflight.empty();
}

inline void perf__ardaPctDirectMirrorRemoveReservation(
        uint64_t sourceId, uint64_t cloneId,
        const std::shared_ptr<perf__ArdaPctDirectWorkerContext>& worker);

inline status_t perf__ardaPctDirectMirrorRestoreUnknownBatch(
        const std::shared_ptr<perf__ArdaPctDirectWorkerContext>& worker,
        uint32_t generation,
        const std::vector<std::pair<sp<GraphicBuffer>, sp<Fence>>>& unknown) {
    if (!worker || worker->sink == nullptr) return NO_INIT;
    for (const auto& entry : unknown) {
        if (entry.first == nullptr) continue;
        entry.first->setGenerationNumber(generation);
        int slot = -1;
        status_t err = worker->sink->attachBuffer(&slot, entry.first);
        if (err != NO_ERROR) {
            ALOGE("ARDA_PCT_V24_2_RESTORE_ATTACH_FAIL session=%" PRIu64
                  " err=%d id=%" PRIu64,
                  worker->sessionId, err, entry.first->getId());
            return err;
        }
        err = worker->sink->cancelBuffer(
                slot, entry.second != nullptr ? entry.second : Fence::NO_FENCE);
        if (err != NO_ERROR) {
            ALOGE("ARDA_PCT_V24_2_RESTORE_CANCEL_FAIL session=%" PRIu64
                  " err=%d id=%" PRIu64,
                  worker->sessionId, err, entry.first->getId());
            return err;
        }
    }
    return NO_ERROR;
}

inline void perf__ardaPctDirectMirrorWorkerMain(
        const std::shared_ptr<perf__ArdaPctDirectWorkerContext>& worker) {
    if (!worker) return;

    ALOGI("ARDA_PCT_B_WORKER_START session=%" PRIu64
          " poll_ms=%d latest_wins=1",
          worker->sessionId, perf__kArdaPctWorkerPollIntervalMs);

    bool selfPoll = false;
    uint64_t selfPollCount = 0;
    uint64_t noMemoryCount = 0;
    std::vector<std::pair<sp<GraphicBuffer>, sp<Fence>>> parkedUnknown;

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(worker->mutex);
            if (selfPoll) {
                worker->cv.wait_for(
                        lock,
                        std::chrono::milliseconds(perf__kArdaPctWorkerPollIntervalMs),
                        [&] {
                            return worker->wakeRequested || worker->stopRequested;
                        });
            } else {
                worker->cv.wait(lock, [&] {
                    return worker->wakeRequested || worker->stopRequested;
                });
            }

            if (worker->stopRequested) break;
            worker->wakeRequested = false;
        }

        auto& s = perf__ardaPctDirectMirrorState();
        uint32_t generation = 0;
        bool bypassReady = false;
        bool shouldDrain = false;

        {
            Mutex::Autolock lock(s.mutex);
            if (!perf__ardaPctDirectMirrorWorkerSessionCurrentLocked(s, worker) ||
                !s.generationValid) {
                selfPoll = false;
                continue;
            }

            perf__ardaPctDirectMirrorRetireSignaledLocked(s);
            generation = s.sinkGeneration;
            bypassReady = s.txpBypassReady;
            shouldDrain =
                    bypassReady &&
                    std::any_of(
                            s.inflight.begin(), s.inflight.end(),
                            [](const perf__ArdaPctDirectInflight& item) {
                                return !item.returned;
                            });
        }

        if (shouldDrain) {
            std::unique_lock<std::mutex> producerLock(worker->producerIoMutex);

            std::vector<std::pair<sp<GraphicBuffer>, sp<Fence>>> unknownDetached;
            size_t matchedCount = 0;
            size_t detachedCount = 0;
            size_t parkedThisBatch = 0;
            status_t terminalStatus = NO_ERROR;
            bool sawNoMemory = false;

            for (size_t attempt = 0; attempt < perf__kArdaPctMaxDetachBurst; ++attempt) {
                sp<GraphicBuffer> buffer;
                sp<Fence> fence;
                const status_t err = worker->sink->detachNextBuffer(&buffer, &fence);

                if (err == NO_MEMORY) {
                    sawNoMemory = true;
                    ++noMemoryCount;
                    break;
                }

                if (err != NO_ERROR) {
                    terminalStatus = err;
                    ALOGE("ARDA_PCT_B_WORKER_DETACH_FAIL session=%" PRIu64
                          " err=%d",
                          worker->sessionId, err);
                    break;
                }

                if (buffer == nullptr) continue;

                ++detachedCount;
                const uint64_t returnedId = buffer->getId();
                bool matched = false;

                {
                    Mutex::Autolock lock(s.mutex);
                    if (!perf__ardaPctDirectMirrorWorkerSessionCurrentLocked(s, worker)) {
                        terminalStatus = NO_INIT;
                    } else {
                        const auto target = std::find_if(
                                s.inflight.begin(), s.inflight.end(),
                                [returnedId](const perf__ArdaPctDirectInflight& item) {
                                    return item.cloneId == returnedId;
                                });

                        if (target != s.inflight.end()) {
                            matched = true;
                            if (!target->returned) {
                                target->returned = true;
                                target->returnedBuffer = buffer;
                                target->releaseFence =
                                        fence != nullptr ? fence : Fence::NO_FENCE;
                                ++s.returnCount;
                                ++matchedCount;

                                ALOGI("ARDA_PCT_B_RETURNED frame=%" PRIu64
                                      " source=%" PRIu64 " clone=%" PRIu64
                                      " session=%" PRIu64,
                                      s.returnCount, target->sourceId,
                                      target->cloneId, worker->sessionId);
                            }
                        }
                    }
                }

                if (!matched) {
                    const auto unknownEntry = std::make_pair(
                            buffer,
                            fence != nullptr ? fence : Fence::NO_FENCE);

                    if (parkedUnknown.size() < perf__kArdaPctMaxParkedUnknown) {
                        parkedUnknown.emplace_back(unknownEntry);
                        ++parkedThisBatch;
                        ALOGI("ARDA_PCT_B_UNKNOWN_PARK session=%" PRIu64
                              " id=%" PRIu64 " parked=%zu cap=%zu",
                              worker->sessionId, buffer->getId(),
                              parkedUnknown.size(), perf__kArdaPctMaxParkedUnknown);
                    } else {
                        unknownDetached.emplace_back(unknownEntry);
                    }
                }

                if (terminalStatus != NO_ERROR) break;
            }

            const status_t restoreStatus =
                    perf__ardaPctDirectMirrorRestoreUnknownBatch(
                            worker, generation, unknownDetached);

            {
                Mutex::Autolock lock(s.mutex);
                ++s.workerBatchCount;
                perf__ardaPctDirectMirrorRetireSignaledLocked(s);

                if (s.workerBatchCount <= 10 ||
                    s.workerBatchCount % 60 == 0 ||
                    matchedCount != 0 ||
                    restoreStatus != NO_ERROR ||
                    terminalStatus != NO_ERROR) {
                    ALOGI("ARDA_PCT_B_WORKER_BATCH batch=%" PRIu64
                          " session=%" PRIu64
                          " detached=%zu matched=%zu unknown=%zu"
                          " restore=%d terminal=%d inflight=%zu"
                          " no_memory=%d parked_total=%zu parked_new=%zu",
                          s.workerBatchCount, worker->sessionId,
                          detachedCount, matchedCount, unknownDetached.size(),
                          restoreStatus, terminalStatus, s.inflight.size(),
                          sawNoMemory ? 1 : 0,
                          parkedUnknown.size(), parkedThisBatch);
                }
            }

            if (restoreStatus != NO_ERROR) {
                ALOGE("ARDA_PCT_B_WORKER_STOP_ON_RESTORE_FAILURE "
                      "session=%" PRIu64 " err=%d",
                      worker->sessionId, restoreStatus);
                break;
            }
        }

        perf__ArdaPctDirectPending submit;
        bool haveSubmit = false;

        {
            Mutex::Autolock lock(s.mutex);
            if (perf__ardaPctDirectMirrorWorkerSessionCurrentLocked(s, worker) &&
                s.generationValid) {
                perf__ardaPctDirectMirrorRetireSignaledLocked(s);

                for (auto it = s.pending.begin(); it != s.pending.end();) {
                    if (it->sequence <= s.lastSubmittedSequence) {
                        const uint64_t staleSequence = it->sequence;
                        const uint64_t staleSource = it->sourceId;
                        it = s.pending.erase(it);
                        ++s.stalePendingDropCount;

                        ALOGI("ARDA_PCT_B2_1_STALE_PENDING_DROP count=%" PRIu64
                              " sequence=%" PRIu64
                              " source=%" PRIu64
                              " last_submitted=%" PRIu64
                              " pending_sources=%zu",
                              s.stalePendingDropCount,
                              staleSequence, staleSource,
                              s.lastSubmittedSequence,
                              s.pending.size());
                    } else {
                        ++it;
                    }
                }

                if (!s.pending.empty()) {
                    if (s.inflight.size() >= perf__kArdaPctMaxInflight) {
                        ++s.asyncSubmitBlockedFullCount;
                        if (s.asyncSubmitBlockedFullCount <= 10 ||
                            s.asyncSubmitBlockedFullCount % 120 == 0) {
                            ALOGI("ARDA_PCT_B2_PENDING_FULL_WAIT count=%" PRIu64
                                  " pending_sources=%zu inflight=%zu",
                                  s.asyncSubmitBlockedFullCount,
                                  s.pending.size(), s.inflight.size());
                        }
                    } else {
                        auto eligible = s.pending.end();

                        for (auto it = s.pending.begin(); it != s.pending.end(); ++it) {
                            const bool busy =
                                    std::find_if(
                                            s.inflight.begin(), s.inflight.end(),
                                            [&it](const perf__ArdaPctDirectInflight& item) {
                                                return item.sourceId == it->sourceId;
                                            }) != s.inflight.end();
                            if (busy) continue;

                            if (eligible == s.pending.end() ||
                                it->sequence < eligible->sequence) {
                                eligible = it;
                            }
                        }

                        if (eligible != s.pending.end()) {
                            submit = std::move(*eligible);
                            s.pending.erase(eligible);
                            s.inflight.push_back(
                                    perf__ArdaPctDirectInflight{
                                            submit.sourceId, submit.cloneId, false,
                                            nullptr, Fence::NO_FENCE});
                            haveSubmit = true;
                        } else {
                            ++s.asyncSubmitBlockedSourceCount;
                            if (s.asyncSubmitBlockedSourceCount <= 10 ||
                                s.asyncSubmitBlockedSourceCount % 120 == 0) {
                                ALOGI("ARDA_PCT_B2_PENDING_SOURCE_WAIT count=%" PRIu64
                                      " pending_sources=%zu inflight=%zu",
                                      s.asyncSubmitBlockedSourceCount,
                                      s.pending.size(), s.inflight.size());
                            }
                        }
                    }
                }
            }
        }

        if (haveSubmit) {
            std::unique_lock<std::mutex> producerLock(worker->producerIoMutex);
            const nsecs_t submitStart = systemTime();

            bool sessionCurrent = false;
            {
                Mutex::Autolock lock(s.mutex);
                sessionCurrent =
                        perf__ardaPctDirectMirrorWorkerSessionCurrentLocked(s, worker) &&
                        s.generationValid &&
                        s.sinkGeneration == submit.generation;
            }

            if (!sessionCurrent) {
                perf__ardaPctDirectMirrorRemoveReservation(
                        submit.sourceId, submit.cloneId, worker);
            } else {
                submit.clone->setGenerationNumber(submit.generation);
                int slot = -1;
                status_t err = worker->sink->attachBuffer(&slot, submit.clone);

                if (err == NO_ERROR) {
                    IGraphicBufferProducer::QueueBufferOutput qbo;
                    err = worker->sink->queueBuffer(
                            slot,
                            IGraphicBufferProducer::QueueBufferInput(
                                    systemTime(), false, HAL_DATASPACE_UNKNOWN,
                                    Rect(submit.clone->width, submit.clone->height),
                                    NATIVE_WINDOW_SCALING_MODE_FREEZE,
                                    0,
                                    submit.acquireFence != nullptr
                                            ? submit.acquireFence
                                            : Fence::NO_FENCE),
                            &qbo);

                    if (err != NO_ERROR) {
                        worker->sink->cancelBuffer(
                                slot,
                                submit.acquireFence != nullptr
                                        ? submit.acquireFence
                                        : Fence::NO_FENCE);
                    }
                }

                const int64_t ioUs = (systemTime() - submitStart) / 1000;

                if (err != NO_ERROR) {
                    perf__ardaPctDirectMirrorRemoveReservation(
                            submit.sourceId, submit.cloneId, worker);

                    Mutex::Autolock lock(s.mutex);
                    ++s.asyncSubmitFailCount;
                    ALOGE("ARDA_PCT_B_ASYNC_SUBMIT_FAIL count=%" PRIu64
                          " sequence=%" PRIu64
                          " source=%" PRIu64
                          " clone=%" PRIu64
                          " err=%d io_us=%" PRId64,
                          s.asyncSubmitFailCount,
                          submit.sequence, submit.sourceId, submit.cloneId,
                          err, ioUs);
                } else {
                    uint64_t queueFrame = 0;
                    size_t inflightNow = 0;
                    bool bypassNow = false;

                    {
                        Mutex::Autolock lock(s.mutex);
                        if (perf__ardaPctDirectMirrorWorkerSessionCurrentLocked(s, worker)) {
                            s.directActive = true;
                            s.lastSubmittedSequence =
                                    std::max(s.lastSubmittedSequence,
                                             submit.sequence);
                            queueFrame = ++s.queueCount;
                            inflightNow = s.inflight.size();
                            bypassNow = s.txpBypassReady;
                        }
                    }

                    ALOGI("ARDA_PCT_DIRECT_QUEUE_OK frame=%" PRIu64
                          " source=%" PRIu64 " clone=%" PRIu64
                          " slot=%d inflight=%zu max=%zu bypass=%d"
                          " async=1 sequence=%" PRIu64 " io_us=%" PRId64,
                          queueFrame, submit.sourceId, submit.cloneId,
                          slot, inflightNow, perf__kArdaPctMaxInflight,
                          bypassNow ? 1 : 0, submit.sequence, ioUs);
                }
            }
        }

        {
            Mutex::Autolock lock(s.mutex);
            if (perf__ardaPctDirectMirrorWorkerSessionCurrentLocked(s, worker) &&
                s.generationValid) {
                perf__ardaPctDirectMirrorRetireSignaledLocked(s);
                selfPoll = !s.pending.empty() || !s.inflight.empty();
            } else {
                selfPoll = false;
            }
        }

        if (selfPoll) {
            ++selfPollCount;
            if (selfPollCount <= 10 || selfPollCount % 120 == 0) {
                ALOGI("ARDA_PCT_B_SELF_POLL count=%" PRIu64
                      " no_memory_total=%" PRIu64
                      " poll_ms=%d",
                      selfPollCount, noMemoryCount,
                      perf__kArdaPctWorkerPollIntervalMs);
            }
        }
    }

    ALOGI("ARDA_PCT_B_PARK_RELEASE_ON_STOP session=%" PRIu64
          " parked=%zu",
          worker->sessionId, parkedUnknown.size());
    parkedUnknown.clear();

    ALOGI("ARDA_PCT_B_WORKER_STOP session=%" PRIu64,
          worker->sessionId);
}
inline void perf__ardaPctDirectMirrorRegisterSink(const sp<IGraphicBufferProducer>& sink) {
    auto& s = perf__ardaPctDirectMirrorState();
    std::shared_ptr<perf__ArdaPctDirectWorkerContext> oldWorker;
    auto newWorker = std::make_shared<perf__ArdaPctDirectWorkerContext>();

    {
        Mutex::Autolock lock(s.mutex);
        oldWorker = s.worker;
        newWorker->sessionId = ++s.nextWorkerSessionId;
        newWorker->sink = sink;
        s.worker = newWorker;
        s.sink = sink;
        s.generationValid = false;
        s.directActive = false;
        s.txpBypassReady = false;
        s.sinkGeometryValid = false;
        s.sinkFallbackLogged = false;
        s.sinkWidth = 0;
        s.sinkHeight = 0;
        s.inflight.clear();
        s.pending.clear();
        s.lastSubmittedSequence = 0;
        ALOGI("ARDA_PCT_DIRECT_SINK_REGISTER route=%s",
              perf__ardaPctDirectRouteEnabled() ? "hdmi" : "txp");
        ALOGI("ARDA_PCT_B2_1_MONOTONIC_PER_SOURCE_FULLY_ASYNC_TWO_INFLIGHT max=%zu burst=%zu session=%" PRIu64,
              perf__kArdaPctMaxInflight, perf__kArdaPctMaxDetachBurst, newWorker->sessionId);
    }

    perf__ardaPctDirectMirrorStopWorker(oldWorker);
    std::thread([newWorker] { perf__ardaPctDirectMirrorWorkerMain(newWorker); }).detach();
}

inline void perf__ardaPctDirectMirrorUnregisterSink(const sp<IGraphicBufferProducer>& sink) {
    auto& s = perf__ardaPctDirectMirrorState();
    std::shared_ptr<perf__ArdaPctDirectWorkerContext> oldWorker;
    {
        Mutex::Autolock lock(s.mutex);
        if (s.sink == sink) {
            oldWorker = s.worker;
            s.worker.reset();
            s.sink.clear();
            s.generationValid = false;
            s.directActive = false;
            s.txpBypassReady = false;
            s.sinkGeometryValid = false;
            s.sinkFallbackLogged = false;
            s.sinkWidth = 0;
            s.sinkHeight = 0;
            s.inflight.clear();
        s.pending.clear();
        s.lastSubmittedSequence = 0;
            ALOGI("ARDA_PCT_DIRECT_SINK_UNREGISTER");
        }
    }
    perf__ardaPctDirectMirrorStopWorker(oldWorker);
}

inline void perf__ardaPctDirectMirrorSetGeneration(
        const sp<IGraphicBufferProducer>& sink, uint32_t generation) {
    auto& s = perf__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    if (s.sink == sink) {
        if (!s.generationValid || s.sinkGeneration != generation) {
            s.sinkGeometryValid = false;
            s.sinkFallbackLogged = false;
            s.sinkWidth = 0;
            s.sinkHeight = 0;
        }
        s.sinkGeneration = generation;
        s.generationValid = true;
        ALOGI("ARDA_PCT_DIRECT_SINK_GENERATION generation=%u", generation);
    }
}

inline bool perf__ardaPctDirectMirrorShouldSuppressTxp(
        const sp<IGraphicBufferProducer>& sink) {
    if (!perf__ardaPctDirectRouteEnabled()) return false;
    auto& s = perf__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    return s.sink == sink && s.directActive;
}

inline void perf__ardaPctDirectMirrorArmTxpBypass(
        const sp<IGraphicBufferProducer>& sink) {
    if (!perf__ardaPctDirectRouteEnabled()) return;
    auto& s = perf__ardaPctDirectMirrorState();
    std::shared_ptr<perf__ArdaPctDirectWorkerContext> worker;
    bool newlyArmed = false;
    {
        Mutex::Autolock lock(s.mutex);
        if (s.sink == sink && s.directActive && !s.txpBypassReady) {
            s.txpBypassReady = true;
            worker = s.worker;
            newlyArmed = true;
            ALOGI("ARDA_PCT_TXP_BYPASS_ARMED");
        }
    }
    if (newlyArmed) perf__ardaPctDirectMirrorWakeWorker(worker);
}

inline bool perf__ardaPctDirectMirrorTxpBypassReady() {
    if (!perf__ardaPctDirectRouteEnabled()) return false;
    auto& s = perf__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    return s.txpBypassReady;
}

inline void perf__ardaPctDirectMirrorRemoveReservation(
        uint64_t sourceId, uint64_t cloneId,
        const std::shared_ptr<perf__ArdaPctDirectWorkerContext>& worker) {
    auto& s = perf__ardaPctDirectMirrorState();
    Mutex::Autolock lock(s.mutex);
    const auto target = std::find_if(
            s.inflight.begin(), s.inflight.end(),
            [sourceId, cloneId](const perf__ArdaPctDirectInflight& item) {
                return item.sourceId == sourceId && item.cloneId == cloneId;
            });
    if (target != s.inflight.end()) s.inflight.erase(target);
    if (s.worker == worker) s.directActive = !s.inflight.empty();
}

// ARDA_PCT_DYNAMIC_SINK_GEOMETRY_V1
inline bool perf__ardaPctDirectMirrorSinkMatchesSourceLocked(
        perf__ArdaPctDirectMirrorState& s,
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

inline status_t perf__ardaPctDirectMirrorQueue(
        const sp<GraphicBuffer>& source, const sp<Fence>& acquireFence) {
    if (!perf__ardaPctDirectRouteEnabled()) return NO_ERROR;
    if (source == nullptr || source->handle == nullptr) return BAD_VALUE;

    auto& s = perf__ardaPctDirectMirrorState();
    const uint64_t sourceId = source->getId();

    {
        Mutex::Autolock lock(s.mutex);
        if (s.sink == nullptr || !s.generationValid || !s.worker) return NO_INIT;
        if (!perf__ardaPctDirectMirrorSinkMatchesSourceLocked(s, source)) {
            return NO_ERROR;
        }
    }

    sp<GraphicBuffer> clone = sp<GraphicBuffer>::make(
            source->handle, GraphicBuffer::CLONE_HANDLE, source->width,
            source->height, static_cast<PixelFormat>(source->format),
            source->layerCount, source->usage, source->stride);
    if (clone == nullptr || clone->initCheck() != NO_ERROR) return NO_MEMORY;

    const uint64_t cloneId = clone->getId();
    const sp<Fence> fence =
            acquireFence != nullptr ? acquireFence : Fence::NO_FENCE;

    std::shared_ptr<perf__ArdaPctDirectWorkerContext> worker;
    uint64_t sequence = 0;
    uint64_t acceptFrame = 0;
    uint64_t replaceTotal = 0;
    uint64_t evictTotal = 0;
    size_t pendingSources = 0;
    bool replaced = false;
    bool evicted = false;

    {
        Mutex::Autolock lock(s.mutex);
        if (s.sink == nullptr || !s.generationValid || !s.worker) return NO_INIT;

        worker = s.worker;
        sequence = ++s.pendingSequence;
        acceptFrame = ++s.pendingAcceptCount;

        perf__ArdaPctDirectPending newest{
                true,
                sourceId,
                cloneId,
                sequence,
                s.sinkGeneration,
                clone,
                fence};

        const auto samePending = std::find_if(
                s.pending.begin(), s.pending.end(),
                [sourceId](const perf__ArdaPctDirectPending& item) {
                    return item.valid && item.sourceId == sourceId;
                });

        if (samePending != s.pending.end()) {
            *samePending = std::move(newest);
            replaced = true;
            ++s.pendingReplaceCount;
        } else if (s.pending.size() < perf__kArdaPctMaxPendingSources) {
            s.pending.push_back(std::move(newest));
        } else {
            const auto oldest = std::min_element(
                    s.pending.begin(), s.pending.end(),
                    [](const perf__ArdaPctDirectPending& a,
                       const perf__ArdaPctDirectPending& b) {
                        return a.sequence < b.sequence;
                    });
            if (oldest != s.pending.end()) {
                *oldest = std::move(newest);
                evicted = true;
                ++s.pendingEvictCount;
            }
        }

        replaceTotal = s.pendingReplaceCount;
        evictTotal = s.pendingEvictCount;
        pendingSources = s.pending.size();
    }

    ALOGI("ARDA_PCT_B2_PENDING_ACCEPT frame=%" PRIu64
          " sequence=%" PRIu64
          " source=%" PRIu64 " clone=%" PRIu64
          " replaced_same_source=%d replace_total=%" PRIu64
          " evicted_other_source=%d evict_total=%" PRIu64
          " pending_sources=%zu",
          acceptFrame, sequence, sourceId, cloneId,
          replaced ? 1 : 0, replaceTotal,
          evicted ? 1 : 0, evictTotal,
          pendingSources);

    perf__ardaPctDirectMirrorWakeWorker(worker);
    return NO_ERROR;
}

} // namespace android

