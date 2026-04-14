#include "Acquisition.h"

#include <CAEN_FELib.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <thread>

bool Acquisition::initializeAcquisitionHardware() {
    int ret = CAEN_FELib_SendCommand(scope_.getHandle(), "/cmd/cleardata");
    if (ret != CAEN_FELib_Success) {
        spdlog::error("[{}] ClearData failed (error {})", scope_.getAddress(), ret);
    }

    ret = CAEN_FELib_SendCommand(scope_.getHandle(), "/cmd/armacquisition");
    if (ret != CAEN_FELib_Success) {
        spdlog::error("[{}] ArmAcquisition failed (error {})", scope_.getAddress(), ret);
        return false;
    }

    if (scope_.getParameter("/par/startsource") == "SWcmd") {
        ret = CAEN_FELib_SendCommand(scope_.getHandle(), "/cmd/swstartacquisition");
        if (ret != CAEN_FELib_Success) {
            spdlog::error("[{}] SwStartAcquisition failed (error {}) trigsrc={} startsrc={}",
                          scope_.getAddress(), ret,
                          scope_.getParameter("/par/acqtriggersource"),
                          scope_.getParameter("/par/startsource"));
            CAEN_FELib_SendCommand(scope_.getHandle(), "/cmd/disarmacquisition");
            return false;
        }
    }

    return true;
}

void Acquisition::finishProducer() {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        producerDone_ = true;
    }
    queueCvNotEmpty_.notify_all();
}

Acquisition::EventPacket Acquisition::buildEventPacket(
    uint32_t trigId,
    uint64_t timestamp,
    const std::vector<std::vector<uint16_t> > &waveformPerChannel,
    const std::vector<size_t> &waveformSizeBuf) const {
    EventPacket packet;
    packet.trigId = trigId;
    packet.timestamp = timestamp;
    packet.waveforms.assign(static_cast<std::size_t>(nChannels_) * recordLength_, 0);
    packet.waveformSizes.assign(nChannels_, 0);

    for (int ch = 0; ch < nChannels_; ++ch) {
        const std::size_t n = std::min<std::size_t>(waveformSizeBuf[ch],
                                                    static_cast<std::size_t>(recordLength_));
        packet.waveformSizes[ch] = static_cast<uint64_t>(n);
        std::copy_n(
            waveformPerChannel[ch].data(),
            n,
            packet.waveforms.data() + static_cast<std::size_t>(ch) * recordLength_);
    }

    return packet;
}

bool Acquisition::enqueueEventPacket(EventPacket &&packet) {
    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCvNotFull_.wait(lock, [this] { return stopRequested_ || eventQueue_.size() < kMaxQueuedEvents; });

        if (stopRequested_) {
            return false;
        }

        eventQueue_.push_back(std::move(packet));

        if (verbose_) {
            spdlog::debug("{}queued event, depth={}", verbosePrefix_, eventQueue_.size());
        }
    }

    queueCvNotEmpty_.notify_one();
    return true;
}

void Acquisition::acquisitionLoop() {
    std::vector<std::vector<uint16_t> > waveformPerChannel(
        nChannels_, std::vector<uint16_t>(recordLength_));
    std::vector<uint16_t *> waveformPtrs(nChannels_, nullptr);
    for (int ch = 0; ch < nChannels_; ++ch) {
        waveformPtrs[ch] = waveformPerChannel[ch].data();
    }
    std::vector<size_t> waveformSizeBuf(nChannels_, 0);
    uint32_t trigId = 0;
    uint64_t timestamp = 0;
    bool waitingForSwTriggerEvent = false;
    uint32_t swTriggerTimeouts = 0;
    uint32_t idleLoopCounter = 0;

    if (verbose_) {
        const std::string trigSource = scope_.getParameter("/par/acqtriggersource");
        const std::string startSource = scope_.getParameter("/par/startsource");
        spdlog::debug("{}starting acquisition, trigger source: {}, start source: {}",
                      verbosePrefix_, trigSource, startSource);
    }

    startTime_ = std::chrono::steady_clock::now();
    bytesWritten_ = 0;
    eventsSaved_ = 0;

    if (!initializeAcquisitionHardware()) {
        finishProducer();
        running_ = false;
        return;
    }

    if (verbose_) {
        spdlog::debug("{}acquisitionLoop entered", verbosePrefix_);
    }

    while (!stopRequested_) {
        if (verbose_ && (++idleLoopCounter % kIdleLogPeriodLoops == 0)) {
            std::string queueDepth;
            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                queueDepth = std::to_string(eventQueue_.size());
            }
            spdlog::debug("{}idle loop, queued={} pending_sw={} waiting_event={}",
                          verbosePrefix_,
                          queueDepth,
                          pendingSoftwareTriggers_.load(std::memory_order_relaxed),
                          waitingForSwTriggerEvent ? "yes" : "no");
        }

        if (useSoftwareTrigger_) {
            const uint32_t pending = pendingSoftwareTriggers_.load(std::memory_order_relaxed);
            if (!waitingForSwTriggerEvent && pending == 0) {
                std::this_thread::sleep_for(kNoPendingSwTriggerSleep);
                continue;
            }

            if (!waitingForSwTriggerEvent) {
                const int sendRet = CAEN_FELib_SendCommand(scope_.getHandle(), "/cmd/sendswtrigger");
                if (sendRet != CAEN_FELib_Success) {
                    spdlog::error("[{}] SendSwTrigger error: {}", scope_.getAddress(), sendRet);
                    break;
                }

                pendingSoftwareTriggers_.fetch_sub(1, std::memory_order_relaxed);
                waitingForSwTriggerEvent = true;
                swTriggerTimeouts = 0;

                if (verbose_) {
                    spdlog::debug("{}software trigger sent from acquisition thread, remaining={}",
                                  verbosePrefix_,
                                  pendingSoftwareTriggers_.load(std::memory_order_relaxed));
                }

                std::this_thread::sleep_for(kPostSwTriggerDelay);
            }
        }

        const int ret = CAEN_FELib_ReadData(
            endpointHandle_, kReadTimeoutMs,
            &trigId,
            &timestamp,
            waveformPtrs.data(),
            waveformSizeBuf.data());

        if (ret == CAEN_FELib_Stop) {
            break;
        }

        if (ret == CAEN_FELib_Timeout) {
            if (useSoftwareTrigger_ && waitingForSwTriggerEvent) {
                ++swTriggerTimeouts;
                if (swTriggerTimeouts >= kMaxSwTriggerTimeouts) {
                    waitingForSwTriggerEvent = false;
                    swTriggerTimeouts = 0;
                    if (verbose_) {
                        spdlog::debug("{}no event returned after SW trigger (2s timeout window)", verbosePrefix_);
                    }
                }
            }
            continue;
        }

        if (ret != CAEN_FELib_Success) {
            spdlog::error("[{}] ReadData error: {}", scope_.getAddress(), ret);
            break;
        }

        if (useSoftwareTrigger_) {
            waitingForSwTriggerEvent = false;
            swTriggerTimeouts = 0;
        }
        idleLoopCounter = 0;

        EventPacket packet = buildEventPacket(trigId, timestamp, waveformPerChannel, waveformSizeBuf);

        if (verbose_) {
            spdlog::debug("{}event trig={} ts={}", verbosePrefix_, trigId, timestamp);
        }

        if (!enqueueEventPacket(std::move(packet))) {
            break;
        }

        ++triggerCount_;
    }

    finishProducer();

    CAEN_FELib_SendCommand(scope_.getHandle(), "/cmd/disarmacquisition");

    if (verbose_) {
        spdlog::debug("{}acquisitionLoop exiting", verbosePrefix_);
    }
}
