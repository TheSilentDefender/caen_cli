#pragma once

#include "Acquisition.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class GuiThread {
public:
    explicit GuiThread(const std::vector<std::unique_ptr<Acquisition>> &acquisitions);
    ~GuiThread();

    bool start();
    void stop();
    void join();

private:
    void guiLoop();

    const std::vector<std::unique_ptr<Acquisition>> &acquisitions_;

    std::thread thread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};

    std::mutex startMutex_;
    std::condition_variable startCv_;
    bool startReady_ = false;
    bool startOk_ = false;
};
