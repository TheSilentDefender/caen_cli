#include "Acquisition.h"

#include <CAEN_FELib.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <stdexcept>

namespace
{
  std::string buildDataFormatJson(int nCh, int recordLength)
  {
    nlohmann::json format = nlohmann::json::array(
        {
            { {"name", "TRIGGER_ID"}, {"type", "U32"} },
            { {"name", "TIMESTAMP"}, {"type", "U64"} },
            { {"name", "WAVEFORM"}, {"type", "U16"}, {"dim", 2}, {"shape", nlohmann::json::array({nCh, recordLength})} },
            { {"name", "WAVEFORM_SIZE"}, {"type", "SIZE_T"}, {"dim", 1}, {"shape", nlohmann::json::array({nCh})} },
        });
    return format.dump();
  }

}

bool Acquisition::readDeviceConfiguration()
{
  try
  {
    nChannels_    = std::stoi(scope_.getParameter("/par/numch"));
    recordLength_ = std::stoi(scope_.getParameter("/par/recordlengths"));
    adcBits_      = std::stoi(scope_.getParameter("/par/adc_nbit"));

    const std::string trigSource  = scope_.getParameter("/par/acqtriggersource");
    const std::string startSource = scope_.getParameter("/par/startsource");
    useSoftwareTrigger_ = (trigSource.find("SwTrg") != std::string::npos);
    if (useSoftwareTrigger_ && startSource.find("SWcmd") == std::string::npos)
    {
      spdlog::warn("acqtriggersource contains SwTrg but /par/startsource=\"{}\" does not include SWcmd; software triggers may not start acquisition.",
                   startSource);
    }

    if (verbose_)
    {
      spdlog::debug("{}trigger source: {}, start source: {}", verbosePrefix_, trigSource, startSource);
    }
  }
  catch (const std::exception &e)
  {
    spdlog::error("Error reading device configuration: {}", e.what());
    return false;
  }

  return true;
}

bool Acquisition::validateDeviceConfiguration() const
{
  if (nChannels_ <= 0 || recordLength_ <= 0 || adcBits_ <= 0)
  {
    spdlog::error("Invalid device configuration: numch={} recordlengths={} adc_nbit={}",
                  nChannels_, recordLength_, adcBits_);
    return false;
  }

  return true;
}

void Acquisition::logSetupSummary() const
{
  if (!verbose_)
  {
    return;
  }

  spdlog::debug("Acquisition: {} channels, {} samples, {}-bit ADC", nChannels_, recordLength_, adcBits_);
  spdlog::debug("Trigger mode: {}",
                (useSoftwareTrigger_ ? "software trigger (press 'T' to trigger)" : "hardware/external trigger"));
  spdlog::debug("{}setup complete: nChannels={} recordLength={} adcBits={}",
                verbosePrefix_, nChannels_, recordLength_, adcBits_);
}

bool Acquisition::setup()
{
  if (!readDeviceConfiguration())
  {
    return false;
  }

  if (!validateDeviceConfiguration())
  {
    return false;
  }

  logSetupSummary();

  voltsBuf_.resize(recordLength_);

  if (!setupEndpoint())
  {
    return false;
  }

  if (!openOutputFiles())
  {
    return false;
  }

  return true;
}

bool Acquisition::setupEndpoint()
{
  int ret = CAEN_FELib_SetValue(
      scope_.getHandle(), "/endpoint/par/activeendpoint", "scope");
  if (ret != CAEN_FELib_Success)
  {
    spdlog::error("Failed to set active endpoint (error {})", ret);
    return false;
  }

  ret = CAEN_FELib_GetHandle(
      scope_.getHandle(), "/endpoint/scope", &endpointHandle_);
  if (ret != CAEN_FELib_Success)
  {
    spdlog::error("Failed to get scope endpoint handle (error {})", ret);
    return false;
  }

  const std::string fmt = buildDataFormatJson(nChannels_, recordLength_);
  if (verbose_)
  {
    spdlog::debug("{}endpoint handle={}", verbosePrefix_, endpointHandle_);
    spdlog::debug("{}set read format={}", verbosePrefix_, fmt);
  }

  ret = CAEN_FELib_SetReadDataFormat(endpointHandle_, fmt.c_str());
  if (ret != CAEN_FELib_Success)
  {
    spdlog::error("Failed to set read data format (error {})", ret);
    return false;
  }

  return true;
}

bool Acquisition::openOutputFiles()
{
  const std::filesystem::path filePath =
      std::filesystem::path(outputDirectory_) /
  (runToken_ + ".bin");

  file_ = fopen(filePath.string().c_str(), "ab");
  if (!file_)
  {
    spdlog::error("Failed to open output file: {}", filePath.string());
    return false;
  }

  setvbuf(file_, nullptr, _IOFBF, 16 << 20);

  if (verbose_)
  {
    spdlog::debug("{}openOutputFiles complete: {}", verbosePrefix_, filePath.string());
  }

  return true;
}

void Acquisition::closeOutputFiles()
{
  if (file_)
  {
    fflush(file_);
    fclose(file_);
    file_ = nullptr;
  }
}
