#pragma once

#include <CAEN_FELib.h>
#include <string>

class CaenScope
{
public:
  CaenScope();
  CaenScope(const std::string &address);
  ~CaenScope();

  int open();
  int close();

  // Read parameters
  std::string getParameter(const std::string &param) const;
  std::string getChanParameter(int channel, const std::string &param) const;

  // Write parameters
  int setParameter(const std::string &param, const std::string &value) const;
  int setParameter(const std::string &param, int value) const;
  int setChanParameter(int channel, const std::string &param, const std::string &value) const;
  int setChanParameter(int channel, const std::string &param, int value) const;

  // Commands
  int sendCommand(const std::string &command) const;

  // Info
  uint64_t getHandle() const { return handle; }
  const std::string &getAddress() const { return address; }
  bool connected() const { return isConnected; }

private:
  uint64_t handle = 0;
  std::string address;

  uint32_t serial = 0;
  uint32_t CUPVersion = 0;
  uint8_t nChannels = 0;
  uint8_t samplerate = 0;

  bool isConnected = false;
};