#pragma once

#include "CaenScope.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Acquisition {
public:
    Acquisition(CaenScope &scope,
                std::string outputDirectory,
                std::string runToken,
                bool verbose);

    ~Acquisition();

    bool setup();

    void start();


    void stop();

    void join();

    bool isRunning() const { return running_; }
    int channelCount() const { return nChannels_; }
    uint64_t triggerCount() const { return triggerCount_.load(std::memory_order_relaxed); }
    uint64_t eventsSaved() const { return eventsSaved_.load(std::memory_order_relaxed); }
    uint64_t bytesWritten() const { return bytesWritten_.load(std::memory_order_relaxed); }
    std::string address() const { return scope_.getAddress(); }

    uint64_t triggerCnt() const;

    uint64_t lostTriggerCnt() const;

    double elapsedSeconds() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime_).count();
    }


    void sendTrigger();

private:
    struct EventPacket {
        uint32_t trigId = 0;
        uint64_t timestamp = 0;
        std::vector<uint16_t> waveforms;
        std::vector<uint64_t> waveformSizes;
    };

    void acquisitionLoop();

    void savingLoop();

    bool readDeviceConfiguration();

    bool validateDeviceConfiguration() const;

    void logSetupSummary() const;

    bool initializeAcquisitionHardware();

    void finishProducer();

    EventPacket buildEventPacket(uint32_t trigId,
                                 uint64_t timestamp,
                                 const std::vector<std::vector<uint16_t> > &waveformPerChannel,
                                 const std::vector<size_t> &waveformSizeBuf) const;

    bool enqueueEventPacket(EventPacket &&packet);

    void computeEventShape(const EventPacket &packet,
                           int32_t &channelsWithData,
                           uint32_t &samplesPerChannel,
                           bool &sampleMismatch) const;

    bool writeChecked(FILE *f, const void *data, std::size_t elemSize, std::size_t elemCount) const;

    bool writeEventHeader(FILE *f,
                          const EventPacket &packet,
                          uint32_t samplesPerChannel) const;

    bool writeEventWaveforms(FILE *f,
                             const EventPacket &packet,
                             uint32_t samplesPerChannel,
                             double adcScale,
                             double adcOffset,
                             uint64_t &eventBytes);

    bool flushIfNeeded(FILE *f, uint64_t savedCount) const;

    bool setupEndpoint();

    bool openOutputFiles();

    void closeOutputFiles();

    CaenScope &scope_;
    std::string outputDirectory_;
    std::string runToken_;
    std::string verbosePrefix_;

    int nChannels_ = 0;
    int recordLength_ = 0; // samples
    int adcBits_ = 0;
    bool useSoftwareTrigger_ = true;
    bool verbose_ = false;

    uint64_t endpointHandle_ = 0;

    std::vector<float> voltsBuf_;

    FILE *file_ = nullptr;

    // Thread control
    std::thread acquisitionThread_;
    std::thread savingThread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> triggerCount_{0};
    std::atomic<uint32_t> pendingSoftwareTriggers_{0};
    std::atomic<uint64_t> bytesWritten_{0};
    std::atomic<uint64_t> eventsSaved_{0};

    std::chrono::steady_clock::time_point startTime_;

    std::mutex queueMutex_;
    std::condition_variable queueCvNotEmpty_;
    std::condition_variable queueCvNotFull_;
    std::deque<EventPacket> eventQueue_;
    bool producerDone_ = false;
    static constexpr std::size_t kMaxQueuedEvents = 4096;

    static constexpr uint64_t kTimeResolutionNs = 8;
    static constexpr uint64_t kEventHeaderBytes =
            sizeof(uint32_t) + // trigId
            sizeof(uint64_t) + // timestamp
            sizeof(uint32_t) + // samplesPerChannel
            sizeof(uint64_t) + // sampling period
            sizeof(int32_t); // channelsWithData
    static constexpr uint64_t kFlushEveryEvents = 10;

    static constexpr uint32_t kReadTimeoutMs = 100;
    static constexpr uint32_t kIdleLogPeriodLoops = 50;
    static constexpr uint32_t kMaxSwTriggerTimeouts = 20; // 2 s @ 100 ms timeout
    static constexpr auto kNoPendingSwTriggerSleep = std::chrono::milliseconds(10);
    static constexpr auto kPostSwTriggerDelay = std::chrono::microseconds(500);
};
