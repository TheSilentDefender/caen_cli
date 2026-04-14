#include "CaenScope.h"
#include "Utils.h"

namespace {
    std::string buildChannelParamPath(int channel, const std::string &param) {
        return "/ch/" + std::to_string(channel) + "/par/" + param;
    }
}

CaenScope::CaenScope() {
    handle = 0;
    address = "";
    serial = 0;
    CUPVersion = 0;
    nChannels = 0;
    samplerate = 0;
    isConnected = false;
}

CaenScope::CaenScope(const std::string &address) {
    handle = 0;
    this->address = address;
    serial = 0;
    CUPVersion = 0;
    nChannels = 0;
    samplerate = 0;
    isConnected = false;
}

CaenScope::~CaenScope() {
    close();
}

int CaenScope::open() {
    int ret = CAEN_FELib_Open(address.c_str(), &handle);

    if (ret != CAEN_FELib_Success) {
        handle = 0;
        return ret;
    }

    isConnected = true;

    return CAEN_FELib_Success;
}

int CaenScope::close() {
    if (handle != 0) {
        CAEN_FELib_Close(handle);

        handle = 0;
        isConnected = false;
    }

    return CAEN_FELib_Success;
}

std::string CaenScope::getParameter(const std::string &param) const {
    char value[256];

    int ret = CAEN_FELib_GetValue(
        handle,
        param.c_str(),
        value);

    if (ret != CAEN_FELib_Success) {
        util::printErr("Error reading parameter: ", param, "\n");
        return "";
    }

    return std::string(value);
}

std::string CaenScope::getChanParameter(int channel, const std::string &param) const {
    return getParameter(buildChannelParamPath(channel, param));
}

int CaenScope::setParameter(const std::string &param, const std::string &value) const {
    return CAEN_FELib_SetValue(
        handle,
        param.c_str(),
        value.c_str());
}

int CaenScope::setParameter(const std::string &param, int value) const {
    return setParameter(param, std::to_string(value));
}

int CaenScope::setChanParameter(int channel,
                                const std::string &param,
                                const std::string &value) const {
    return setParameter(buildChannelParamPath(channel, param), value);
}

int CaenScope::setChanParameter(int channel,
                                const std::string &param,
                                int value) const {
    return setParameter(buildChannelParamPath(channel, param), value);
}

int CaenScope::sendCommand(const std::string &command) const {
    return CAEN_FELib_SendCommand(
        handle,
        command.c_str());
}
