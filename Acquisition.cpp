#include "Acquisition.h"
#include "Utils.h"

#include <CAEN_FELib.h>

#include <thread>

namespace
{
}

Acquisition::Acquisition(CaenScope &scope,
                         const std::string &outputDirectory,
                         const std::string &runToken,
                         bool verbose)
    : scope_(scope), outputDirectory_(outputDirectory), runToken_(runToken), verbosePrefix_("[verbose][" + (scope_.getAddress().empty() ? std::string("unknown") : scope_.getAddress()) + "] "), verbose_(verbose)
{
}

Acquisition::~Acquisition()
{
  stop();
  join();
  closeOutputFiles();
}

void Acquisition::start()
{
  running_ = true;
  stopRequested_ = false;
  producerDone_ = false;
  pendingSoftwareTriggers_ = 0;
  startTime_ = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    eventQueue_.clear();
  }

  acquisitionThread_ = std::thread([this]
                                   { acquisitionLoop(); });
  savingThread_ = std::thread([this]
                              { savingLoop(); });

  if (verbose_)
  {
    util::print(verbosePrefix_, "threads started\n");
  }
}

void Acquisition::stop()
{
  if (!running_)
  {
    return;
  }
  stopRequested_ = true;
  // disarm acquisition to unblock acquisition thread
  CAEN_FELib_SendCommand(scope_.getHandle(), "/cmd/disarmacquisition");
  queueCvNotEmpty_.notify_all();
  queueCvNotFull_.notify_all();

  if (verbose_)
  {
    util::print(verbosePrefix_, "stop requested, disarm sent\n");
  }
}

void Acquisition::join()
{
  if (acquisitionThread_.joinable())
  {
    acquisitionThread_.join();
  }
  if (savingThread_.joinable())
  {
    savingThread_.join();
  }
}

void Acquisition::sendTrigger()
{
  if (!useSoftwareTrigger_)
  {
    // sw trigger not enabled; ignore
    util::printErr("Software trigger ignored: /par/acqtriggersource=\"",
                   scope_.getParameter("/par/acqtriggersource"),
                   "\". Set it to SwTrg (and /par/startsource to include SWcmd) to use key 'T'.\n");
    return;
  }
  // queue max one software trigger
  uint32_t queued = pendingSoftwareTriggers_.load(std::memory_order_relaxed);
  while (queued == 0 && !pendingSoftwareTriggers_.compare_exchange_weak(
                            queued, queued + 1, std::memory_order_relaxed))
  {
  }

  if (queued > 0)
  {
    if (verbose_)
    {
      util::print(verbosePrefix_, "software trigger dropped, pending=",
                  pendingSoftwareTriggers_.load(std::memory_order_relaxed), "\n");
    }
    return;
  }

  if (verbose_)
  {
    util::print(verbosePrefix_, "queued software trigger request, pending=1\n");
  }
}

uint64_t Acquisition::triggerCnt() const
{
  // need to read to update counts
  (void)scope_.getParameter("/par/RealtimeMonitor");
  return util::toUint64OrZero(scope_.getParameter("/par/TriggerCnt"));
}

uint64_t Acquisition::lostTriggerCnt() const
{
  // need to read to update counts
  (void)scope_.getParameter("/par/RealtimeMonitor");
  return util::toUint64OrZero(scope_.getParameter("/par/LostTriggerCnt"));
}
