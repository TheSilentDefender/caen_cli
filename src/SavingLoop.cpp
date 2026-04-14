#include "Acquisition.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

void Acquisition::computeEventShape(const EventPacket &packet,
                                    int32_t &channelsWithData,
                                    uint32_t &samplesPerChannel,
                                    bool &sampleMismatch) const {
    channelsWithData = 0;
    samplesPerChannel = 0;
    bool sampleCountSet = false;
    sampleMismatch = false;

    for (int ch = 0; ch < nChannels_; ++ch) {
        const uint32_t nSamples = static_cast<uint32_t>(packet.waveformSizes[ch]);
        if (nSamples == 0) {
            continue;
        }

        ++channelsWithData;
        if (!sampleCountSet) {
            samplesPerChannel = nSamples;
            sampleCountSet = true;
        } else if (nSamples != samplesPerChannel) {
            sampleMismatch = true;
            samplesPerChannel = std::min(samplesPerChannel, nSamples);
        }
    }
}

bool Acquisition::writeChecked(FILE *f, const void *data, std::size_t elemSize, std::size_t elemCount) const {
    if (std::fwrite(data, elemSize, elemCount, f) != elemCount) {
        spdlog::error("{}file write failed: {}", verbosePrefix_, std::strerror(errno));
        return false;
    }
    return true;
}

bool Acquisition::writeEventHeader(FILE *f,
                                   const EventPacket &packet,
                                   uint32_t samplesPerChannel,
                                   int32_t channelsWithData) const {
    return writeChecked(f, &packet.trigId, sizeof(uint32_t), 1) &&
           writeChecked(f, &packet.timestamp, sizeof(uint64_t), 1) &&
           writeChecked(f, &samplesPerChannel, sizeof(uint32_t), 1) &&
           writeChecked(f, &kTimeResolutionNs, sizeof(uint64_t), 1) && writeChecked(
               f, &channelsWithData, sizeof(int32_t), 1);
}

bool Acquisition::writeEventWaveforms(FILE *f,
                                      const EventPacket &packet,
                                      uint32_t samplesPerChannel,
                                      double adcScale,
                                      double adcOffset,
                                      uint64_t &eventBytes) {
    for (int ch = 0; ch < nChannels_; ++ch) {
        const uint32_t nSamples = static_cast<uint32_t>(packet.waveformSizes[ch]);
        const uint16_t *raw = &packet.waveforms[static_cast<std::size_t>(ch) * recordLength_];
        const uint32_t writeSamples = std::min(samplesPerChannel, nSamples);

        const int16_t activeChannel = (nSamples > 0) ? static_cast<int16_t>(ch) : static_cast<int16_t>(-1);
        if (!writeChecked(f, &activeChannel, sizeof(int16_t), 1)) {
            return false;
        }

        std::fill(voltsBuf_.begin(), voltsBuf_.begin() + samplesPerChannel, 0.0f);
        for (uint32_t s = 0; s < writeSamples; ++s) {
            voltsBuf_[s] = static_cast<float>(raw[s] * adcScale + adcOffset);
        }

        if (!writeChecked(f, voltsBuf_.data(), sizeof(float), samplesPerChannel)) {
            return false;
        }

        eventBytes += sizeof(int16_t) + sizeof(float) * samplesPerChannel;
    }

    return true;
}

bool Acquisition::flushIfNeeded(FILE *f, uint64_t savedCount) const {
    if ((savedCount % kFlushEveryEvents) != 0) {
        return true;
    }

    if (std::fflush(f) != 0) {
        spdlog::error("{}file flush failed: {}", verbosePrefix_, std::strerror(errno));
        return false;
    }

    return true;
}

void Acquisition::savingLoop() {
    const double adcScale = 2.0 / static_cast<double>((1 << adcBits_) - 1); // convert to volts
    const double adcOffset = -1.0;

    uint64_t savedCount = 0;

    if (verbose_) {
        spdlog::debug("{}savingLoop entered", verbosePrefix_);
    }

    while (true) {
        EventPacket packet;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCvNotEmpty_.wait(lock, [this] { return stopRequested_ || producerDone_ || !eventQueue_.empty(); });

            if (eventQueue_.empty()) {
                if (producerDone_ || stopRequested_) {
                    break;
                }
                continue;
            }

            packet = std::move(eventQueue_.front());
            eventQueue_.pop_front();

            if (verbose_) {
                spdlog::debug("{}dequeued event, depth={}", verbosePrefix_, eventQueue_.size());
            }
        }
        queueCvNotFull_.notify_one();

        FILE *f = file_;
        if (!f) {
            spdlog::error("{}output file handle is null", verbosePrefix_);
            break;
        }

        const int32_t channelsWithData = nChannels_;
        const uint32_t samplesPerChannel = static_cast<uint32_t>(recordLength_);

        uint64_t eventBytes = kEventHeaderBytes;

        if (!writeEventHeader(f, packet, samplesPerChannel, channelsWithData)) {
            break;
        }

        if (!writeEventWaveforms(f, packet, samplesPerChannel, adcScale, adcOffset, eventBytes)) {
            break;
        }

        ++savedCount;

        if (!flushIfNeeded(f, savedCount)) {
            break;
        }

        bytesWritten_ += eventBytes;
        eventsSaved_++;

        if (verbose_) {
            spdlog::debug("{}saved event #{} trigger={}", verbosePrefix_, savedCount, packet.trigId);
        }
    }

    running_ = false;
}
