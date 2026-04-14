#include "Acquisition.h"
#include "CaenScope.h"
#include "Utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <sys/select.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <CLI/CLI.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>
#include <functional>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
    struct Options {
        std::string address;
        std::string settingsFile;
        std::string outputDirectory = ".";
        std::vector<std::string> getPaths;
        bool testConnection = false;
        bool reboot = false;
        bool verbose = false;
    };

    bool isChannelSection(const std::string &section) {
        if (section.empty()) {
            return false;
        }

        for (unsigned char character: section) {
            if (!std::isdigit(character)) {
                return false;
            }
        }

        return true;
    }

    bool isChannelSelectorSyntax(const std::string &selector) {
        if (selector.empty()) {
            return false;
        }

        for (unsigned char character: selector) {
            if (!std::isdigit(character) && character != ',' && character != '-') {
                return false;
            }
        }

        if (selector.front() == ',' || selector.back() == ',' || selector.front() == '-' || selector.back() == '-') {
            return false;
        }

        return true;
    }

    bool parseChannelSelectorSpec(const std::string &spec,
                                  int numChannels,
                                  std::vector<int> &channels,
                                  std::string &error) {
        std::set<int> selected;

        if (spec.rfind("selector:", 0) == 0) {
            const std::string selector = spec.substr(std::string("selector:").size());
            std::size_t cursor = 0;

            while (cursor < selector.size()) {
                std::size_t nextComma = selector.find(',', cursor);
                if (nextComma == std::string::npos) {
                    nextComma = selector.size();
                }

                const std::string token = selector.substr(cursor, nextComma - cursor);
                if (token.empty()) {
                    error = "empty channel token in selector";
                    return false;
                }

                const std::size_t dash = token.find('-');
                if (dash == std::string::npos) {
                    int ch = -1;
                    try {
                        ch = std::stoi(token);
                    } catch (...) {
                        error = "invalid channel token '" + token + "'";
                        return false;
                    }

                    if (ch < 0 || ch >= numChannels) {
                        error = "channel " + std::to_string(ch) + " out of range [0," + std::to_string(numChannels - 1)
                                + "]";
                        return false;
                    }
                    selected.insert(ch);
                } else {
                    const std::string beginText = token.substr(0, dash);
                    const std::string endText = token.substr(dash + 1);
                    int begin = -1;
                    int end = -1;

                    try {
                        begin = std::stoi(beginText);
                        end = std::stoi(endText);
                    } catch (...) {
                        error = "invalid channel range token '" + token + "'";
                        return false;
                    }

                    if (begin > end) {
                        error = "descending range not allowed in token '" + token + "'";
                        return false;
                    }
                    if (begin < 0 || end >= numChannels) {
                        error = "range " + token + " out of bounds [0," + std::to_string(numChannels - 1) + "]";
                        return false;
                    }

                    for (int ch = begin; ch <= end; ++ch) {
                        selected.insert(ch);
                    }
                }

                cursor = nextComma + 1;
            }
        } else if (spec.rfind("mask:", 0) == 0) {
            const std::string maskText = spec.substr(std::string("mask:").size());
            unsigned long long maskValue = 0;
            try {
                maskValue = std::stoull(maskText, nullptr, 0);
            } catch (...) {
                error = "invalid mask value '" + maskText + "'";
                return false;
            }

            for (int ch = 0; ch < numChannels && ch < 64; ++ch) {
                if ((maskValue >> static_cast<unsigned>(ch)) & 1ULL) {
                    selected.insert(ch);
                }
            }
        } else {
            error = "unsupported selector spec '" + spec + "'";
            return false;
        }

        if (selected.empty()) {
            error = "selector resolved to zero channels";
            return false;
        }

        channels.assign(selected.begin(), selected.end());
        return true;
    }

    bool expandSettingPath(const std::string &section,
                           const std::string &key,
                           std::string &path) {
        if (section == "par") {
            path = "/par/" + key;
            return true;
        }

        if (isChannelSection(section)) {
            path = "/ch/" + section + "/par/" + key;
            return true;
        }

        return false;
    }

    std::string sanitizeFileComponent(const std::string &value) {
        std::string sanitized;
        sanitized.reserve(value.size());

        for (unsigned char character: value) {
            if (std::isalnum(character) || character == '-' || character == '_') {
                sanitized.push_back(static_cast<char>(character));
            } else {
                sanitized.push_back('_');
            }
        }

        if (sanitized.empty()) {
            return "unknown";
        }

        return sanitized;
    }

    bool stripRequiredQuotes(const std::string &value, std::string &out) {
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            out = value.substr(1, value.size() - 2);
            return true;
        }
        return false;
    }

    std::string buildRunToken(const CaenScope &scope) {
        std::string addressToken = scope.getAddress();
        const std::size_t protoPos = addressToken.find("://");
        if (protoPos != std::string::npos) {
            addressToken.replace(protoPos, 3, "-");
        }

        for (char &character: addressToken) {
            const unsigned char u = static_cast<unsigned char>(character);
            if (!(std::isalnum(u) || character == '-' || character == '_' || character == '.')) {
                character = '-';
            }
        }

        if (addressToken.empty()) {
            addressToken = "unknown";
        }

        const auto now = std::chrono::system_clock::now();
        const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm localTm{};
        localtime_r(&nowTime, &localTm);

        char timeBuf[32];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d%H%M%S", &localTm);

        return addressToken + "_" + std::string(timeBuf);
    }

    std::filesystem::path buildChannelOutputPath(const std::string &outputDirectory,
                                                 const std::string &runToken,
                                                 int channel) {
        return std::filesystem::path(outputDirectory) / ("raw_" + runToken + "_CH" + std::to_string(channel) + ".bin");
    }


    bool parseSectionAddressAndScope(const std::string &section,
                                     const std::string &defaultAddress,
                                     std::string &address,
                                     std::string &scope) {
        std::string normalized = util::trim(section);

        if (!normalized.empty() && normalized.front() == '"') {
            const std::size_t quoteEnd = normalized.find('"', 1);
            if (quoteEnd == std::string::npos) {
                return false;
            }

            address = normalized.substr(1, quoteEnd - 1);
            std::string rest = util::trim(normalized.substr(quoteEnd + 1));
            if (rest.empty() || rest.front() != '.') {
                return false;
            }
            rest = rest.substr(1); // remove dot

            if (rest == "par") {
                scope = "par";
                return !address.empty();
            }


            if (rest == "ch") {
                scope = "ch_default";
                return !address.empty();
            }

            const std::string chPrefix = "ch.";
            if (rest.rfind(chPrefix, 0) == 0) {
                const std::string selector = rest.substr(chPrefix.size());
                if (isChannelSection(selector)) {
                    scope = selector;
                    return !address.empty();
                }

                const std::string maskPrefix = "mask.";
                if (selector.rfind(maskPrefix, 0) == 0) {
                    const std::string maskValue = selector.substr(maskPrefix.size());
                    if (maskValue.empty()) {
                        return false;
                    }
                    scope = "mask:" + maskValue;
                    return !address.empty();
                }

                if (isChannelSelectorSyntax(selector)) {
                    scope = "selector:" + selector;
                    return !address.empty();
                }

                return false;
            }

            return false;
        }

        return false;
    }

    bool loadSettingsFile(const std::string &path,
                          const std::string &defaultAddress,
                          std::map<std::string, std::vector<std::pair<std::string, std::string> > > &settingsByAddress,
                          std::map<std::string, std::vector<std::pair<std::string, std::string> > > &
                          channelDefaultsByAddress,
                          bool verbose) {
        (void) defaultAddress;

        auto nodeToString = [](const toml::node &node, std::string &out) -> bool {
            if (const auto v = node.value<std::string>()) {
                out = *v;
                return true;
            }
            if (const auto v = node.value<int64_t>()) {
                out = std::to_string(*v);
                return true;
            }
            if (const auto v = node.value<uint64_t>()) {
                out = std::to_string(*v);
                return true;
            }
            if (const auto v = node.value<double>()) {
                std::ostringstream ss;
                ss << *v;
                out = ss.str();
                return true;
            }
            if (const auto v = node.value<bool>()) {
                out = *v ? "True" : "False";
                return true;
            }
            return false;
        };

        try {
            const toml::table config = toml::parse_file(path);

            for (const auto &[addressKey, addressNode]: config) {
                const auto *addressTable = addressNode.as_table();
                if (!addressTable) {
                    spdlog::error("Invalid settings: top-level key '{}' must contain a table",
                                  std::string(addressKey.str()));
                    return false;
                }

                const std::string address = std::string(addressKey.str());

                std::function<bool(const toml::table &, const std::string &)> visitTable;
                visitTable = [&](const toml::table &tbl, const std::string &scopePath) -> bool {
                    for (const auto &[key, node]: tbl) {
                        if (const auto *child = node.as_table()) {
                            const std::string nextScope = scopePath.empty()
                                                              ? std::string(key.str())
                                                              : scopePath + "." + std::string(key.str());
                            if (!visitTable(*child, nextScope)) {
                                return false;
                            }
                            continue;
                        }

                        if (scopePath.empty()) {
                            spdlog::error(
                                "Invalid settings in address '{}': values must be under [\"<address>\".par] or channel sections",
                                address);
                            return false;
                        }

                        std::string value;
                        if (!nodeToString(node, value)) {
                            spdlog::error("Invalid value type for {}.{} in address '{}'", scopePath,
                                          std::string(key.str()), address);
                            return false;
                        }

                        const std::string keyStr = std::string(key.str());

                        if (scopePath == "par") {
                            const std::string settingPath = "/par/" + keyStr;
                            settingsByAddress[address].emplace_back(settingPath, value);
                            if (verbose) {
                                spdlog::debug("parsed setting: {} {}={}", address, settingPath, value);
                            }
                            continue;
                        }

                        if (scopePath == "ch") {
                            channelDefaultsByAddress[address].emplace_back(keyStr, value);
                            if (verbose) {
                                spdlog::debug("parsed channel default: {} {}={}", address, keyStr, value);
                            }
                            continue;
                        }

                        if (scopePath.rfind("ch.", 0) != 0) {
                            spdlog::error("Invalid scope '{}' for address '{}'", scopePath, address);
                            return false;
                        }

                        const std::string selector = scopePath.substr(3);
                        if (isChannelSection(selector)) {
                            const std::string settingPath = "/ch/" + selector + "/par/" + keyStr;
                            settingsByAddress[address].emplace_back(settingPath, value);
                            if (verbose) {
                                spdlog::debug("parsed setting: {} {}={}", address, settingPath, value);
                            }
                            continue;
                        }

                        std::string multiSpec;
                        if (selector.rfind("mask.", 0) == 0) {
                            const std::string maskValue = selector.substr(std::string("mask.").size());
                            if (maskValue.empty()) {
                                spdlog::error("Invalid channel mask scope '{}' for address '{}'", scopePath, address);
                                return false;
                            }
                            multiSpec = "mask:" + maskValue;
                        } else if (isChannelSelectorSyntax(selector)) {
                            multiSpec = "selector:" + selector;
                        } else {
                            spdlog::error("Invalid channel scope '{}' for address '{}'", scopePath, address);
                            return false;
                        }

                        const std::string settingPath = "/__multi__/" + multiSpec + "/par/" + keyStr;
                        settingsByAddress[address].emplace_back(settingPath, value);
                        if (verbose) {
                            spdlog::debug("parsed multi-channel setting: {} {}={}", address, settingPath, value);
                        }
                    }

                    return true;
                };

                if (!visitTable(*addressTable, "")) {
                    return false;
                }
            }

            return true;
        } catch (const toml::parse_error &e) {
            spdlog::error("Unable to parse settings file '{}': {}", path, e.description());
            return false;
        }
    }

    bool applySettings(CaenScope &scope,
                       const std::vector<std::pair<std::string, std::string> > &settings,
                       bool verbose) {
        for (const auto &[path, value]: settings) {
            const int result = scope.setParameter(path, value);

            if (result != CAEN_FELib_Success) {
                spdlog::error("Failed to apply setting {}={} (error {})", path, value, result);
                return false;
            }

            if (verbose) {
                spdlog::debug("applied setting {}={}", path, value);
            }
        }

        if (verbose && !settings.empty()) {
            spdlog::debug("Applied {} settings", settings.size());
        }

        return true;
    }

    void printDeviceInfo(const CaenScope &scope) {
        util::print("Model:           ", scope.getParameter("/par/modelname"), "\n");
        util::print("Serial:          ", scope.getParameter("/par/serialnum"), "\n");
        util::print("FW type:         ", scope.getParameter("/par/fwtype"), "\n");
        util::print("FPGA FW version: ", scope.getParameter("/par/fpga_fwver"), "\n");
        util::print("CUP version:     ", scope.getParameter("/par/cupver"), "\n");
        util::print("Channels:        ", scope.getParameter("/par/numch"), "\n");
        util::print("ADC bits:        ", scope.getParameter("/par/adc_nbit"), "\n");
        util::print("ADC sample rate: ", scope.getParameter("/par/adc_samplrate"), " Msps\n");
    }

    void printRequestedPaths(const CaenScope &scope, const std::vector<std::string> &paths) {
        for (const std::string &path: paths) {
            util::print(path, '=', scope.getParameter(path), '\n');
        }
    }

    class RawTerminal {
    public:
        RawTerminal() {
            tcgetattr(STDIN_FILENO, &original_);
            struct termios raw = original_;
            raw.c_lflag &= ~static_cast<unsigned>(ICANON | ECHO);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }

        ~RawTerminal() { tcsetattr(STDIN_FILENO, TCSANOW, &original_); }

    private:
        struct termios original_;
    };

    // 'q' for quit and 't' for trigger
    char checkKeyPress() {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        struct timeval tv{0, 0};
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                return (c == 'Q' || c == 'q')
                           ? 'Q'
                           : (c == 'T' || c == 't')
                                 ? 'T'
                                 : 0;
            }
        }
        return 0;
    }

    bool ensureOutputDirectory(const std::string &outputDirectory) {
        std::error_code error;
        std::filesystem::create_directories(outputDirectory, error);

        if (error) {
            spdlog::error("Unable to create output directory {}: {}", outputDirectory, error.message());
            return false;
        }

        return true;
    }

    void closeScopes(std::vector<std::unique_ptr<CaenScope> > &scopes, bool verbose) {
        for (auto &scope: scopes) {
            const std::string addr = scope->getAddress();
            scope->close();
            if (verbose) {
                spdlog::debug("Disconnected from CAEN Scope at {}", addr);
            }
        }
    }

    void addAddressIfNew(std::vector<std::string> &addresses, const std::string &address) {
        for (const auto &existing: addresses) {
            if (existing == address) {
                return;
            }
        }

        addresses.push_back(address);
    }

    std::vector<std::string> collectAddresses(
        const Options &options,
        const std::map<std::string, std::vector<std::pair<std::string, std::string> > > &settingsByAddress,
        const std::map<std::string, std::vector<std::pair<std::string, std::string> > > &channelDefaultsByAddress) {
        std::vector<std::string> addresses;

        if (!options.address.empty()) {
            addresses.push_back(options.address);
        }

        for (const auto &it: settingsByAddress) {
            addAddressIfNew(addresses, it.first);
        }
        for (const auto &it: channelDefaultsByAddress) {
            addAddressIfNew(addresses, it.first);
        }

        return addresses;
    }

    bool connectScopes(const std::vector<std::string> &addresses,
                       std::vector<std::unique_ptr<CaenScope> > &scopes,
                       bool verbose) {
        scopes.clear();
        scopes.reserve(addresses.size());

        for (const auto &address: addresses) {
            auto scope = std::make_unique<CaenScope>(address);
            const int openResult = scope->open();
            if (openResult != CAEN_FELib_Success) {
                util::printErr("Connection failed for ", address,
                               " with error ", openResult, '\n');
                closeScopes(scopes, verbose);
                return false;
            }

            if (verbose) {
                spdlog::debug("Connected to CAEN Scope at {}", address);
            }

            scopes.push_back(std::move(scope));
        }

        return true;
    }

    bool expandAndFlattenSettingsForScope(
        const CaenScope &scope,
        const std::string &address,
        std::map<std::string, std::vector<std::pair<std::string, std::string> > > &settingsByAddress,
        const std::map<std::string, std::vector<std::pair<std::string, std::string> > > &channelDefaultsByAddress) {
        int numch = 0;
        try {
            numch = std::stoi(scope.getParameter("/par/numch"));
        } catch (...) {
            util::printErr("Invalid /par/numch value for ", address, '\n');
            return false;
        }

        const auto defaultsIt = channelDefaultsByAddress.find(address);
        if (defaultsIt != channelDefaultsByAddress.end()) {
            std::vector<std::pair<std::string, std::string> > expanded;
            expanded.reserve(static_cast<std::size_t>(numch) * defaultsIt->second.size());
            for (int ch = 0; ch < numch; ++ch) {
                for (const auto &[key, value]: defaultsIt->second) {
                    expanded.emplace_back("/ch/" + std::to_string(ch) + "/par/" + key, value);
                }
            }

            auto &existing = settingsByAddress[address];
            auto insertPos = existing.end();
            for (auto it = existing.begin(); it != existing.end(); ++it) {
                if (it->first.rfind("/ch/", 0) == 0) {
                    insertPos = it;
                    break;
                }
            }
            existing.insert(insertPos, expanded.begin(), expanded.end());
        }

        auto &existing = settingsByAddress[address];
        std::vector<std::pair<std::string, std::string> > flattened;
        flattened.reserve(existing.size());

        for (const auto &[path, value]: existing) {
            const std::string marker = "/__multi__/";
            if (path.rfind(marker, 0) != 0) {
                flattened.emplace_back(path, value);
                continue;
            }

            const std::size_t parPos = path.find("/par/");
            if (parPos == std::string::npos) {
                util::printErr("Invalid multi-channel setting path: ", path, '\n');
                return false;
            }

            const std::string spec = path.substr(marker.size(), parPos - marker.size());
            const std::string key = path.substr(parPos + 5);
            std::vector<int> channels;
            std::string parseError;
            if (!parseChannelSelectorSpec(spec, numch, channels, parseError)) {
                util::printErr("Invalid multi-channel selector '", spec,
                               "' for ", address, ": ", parseError, '\n');
                return false;
            }

            for (const int ch: channels) {
                flattened.emplace_back("/ch/" + std::to_string(ch) + "/par/" + key, value);
            }
        }

        existing.swap(flattened);
        return true;
    }

    bool applySettingsForAllScopes(
        const std::vector<std::unique_ptr<CaenScope> > &scopes,
        const std::vector<std::string> &addresses,
        const std::map<std::string, std::vector<std::pair<std::string, std::string> > > &settingsByAddress,
        bool verbose) {
        for (std::size_t i = 0; i < scopes.size(); ++i) {
            const auto settingsIt = settingsByAddress.find(addresses[i]);
            const std::vector<std::pair<std::string, std::string> > emptySettings;
            const auto &settings = (settingsIt != settingsByAddress.end()) ? settingsIt->second : emptySettings;
            if (!applySettings(*scopes[i], settings, verbose)) {
                return false;
            }
        }

        return true;
    }

    bool anyAcquisitionRunning(const std::vector<std::unique_ptr<Acquisition> > &acquisitions) {
        for (const auto &acq: acquisitions) {
            if (acq->isRunning()) {
                return true;
            }
        }
        return false;
    }

    void stopAndJoinAcquisitions(std::vector<std::unique_ptr<Acquisition> > &acquisitions) {
        for (auto &acq: acquisitions) {
            acq->stop();
        }
        for (auto &acq: acquisitions) {
            acq->join();
        }
    }

    void printAcquisitionSummary(const std::vector<std::unique_ptr<Acquisition> > &acquisitions) {
        uint64_t totalTriggers = 0;
        uint64_t totalTriggerCnt = 0;
        uint64_t totalLost = 0;
        uint64_t totalSaved = 0;
        uint64_t totalBytes = 0;
        double totalElapsed = 0.0;

        for (const auto &acq: acquisitions) {
            totalTriggers += acq->triggerCount();
            totalTriggerCnt += acq->triggerCnt();
            totalLost += acq->lostTriggerCnt();
            totalSaved += acq->eventsSaved();
            totalBytes += acq->bytesWritten();
            totalElapsed = std::max(totalElapsed, acq->elapsedSeconds());
        }

        const char *unit = "B";
        double displayBytes = static_cast<double>(totalBytes);
        if (totalBytes > 1e9) {
            displayBytes = totalBytes / 1e9;
            unit = "GB";
        } else if (totalBytes > 1e6) {
            displayBytes = totalBytes / 1e6;
            unit = "MB";
        }

        const double throughput = (totalElapsed > 0) ? (totalBytes / 1e6 / totalElapsed) : 0.0;

        printf("\033[1;32m=== Acquisition Complete ===\033[0m\n");
        printf("  \033[36mTotal triggers:     %lu\033[0m\n", (unsigned long) totalTriggers);
        printf("  \033[36mTotal TriggerCnt:   %lu\033[0m\n", (unsigned long) totalTriggerCnt);
        printf("  \033[36mTotal LostTrigger:  %lu\033[0m\n", (unsigned long) totalLost);
        printf("  \033[36mTotal events saved: %lu\033[0m\n", (unsigned long) totalSaved);
        printf("  \033[36mTotal data:         %.2f %s\033[0m\n", displayBytes, unit);
        printf("  \033[32mAverage throughput: %.2f MB/s\033[0m\n", throughput);
        printf("  \033[36mElapsed time:       %.2f s\033[0m\n", totalElapsed);
    }
}

int main(int argc, char **argv) {
    Options options;
    CLI::App app{"CAEN digitizer readout tool"};
    app.set_help_flag("-h,--help", "Show this help message and exit");
    app.add_option("-s,--settings,--config", options.settingsFile,
                   "Settings file with address-qualified sections (required for acquisition)");
    app.add_option("-o,--output", options.outputDirectory,
                   "Output directory for generated files");
    app.add_option("-a,--address", options.address,
                   "Device address (for --test or --get)");
    app.add_option("-g,--get", options.getPaths,
                   "Read and print a FELib path; may be repeated")->expected(1);
    app.add_flag("-t,--test", options.testConnection,
                 "Connect, print device info, and exit (requires --address)");
    app.add_flag("--reboot", options.reboot,
                 "Send /cmd/reboot to all devices in settings file");
    app.add_flag("-v,--verbose", options.verbose,
                 "Print detailed acquisition and settings diagnostics");
    CLI11_PARSE(app, argc, argv);

    spdlog::set_pattern("%v");
    spdlog::set_level(options.verbose ? spdlog::level::debug : spdlog::level::info);

    if (options.settingsFile.empty() && !options.testConnection && !options.reboot && options.getPaths.empty()) {
        util::printErr("[error] --settings is required (except for --test, --get, or --reboot with --address)\n");
        util::printErr(app.help());
        return 1;
    }

    if ((options.testConnection || !options.getPaths.empty()) && options.address.empty()) {
        util::printErr("[error] --test and --get require --address\n");
        return 1;
    }

    std::map<std::string, std::vector<std::pair<std::string, std::string> > > settingsByAddress;
    std::map<std::string, std::vector<std::pair<std::string, std::string> > > channelDefaultsByAddress;
    if (!options.settingsFile.empty() && !loadSettingsFile(options.settingsFile, options.address, settingsByAddress,
                                                           channelDefaultsByAddress, options.verbose)) {
        return 1;
    }

    std::vector<std::string> addresses = collectAddresses(options, settingsByAddress, channelDefaultsByAddress);

    if (addresses.empty()) {
        util::printErr("No device addresses found in settings file.\n");
        return 1;
    }

    std::vector<std::unique_ptr<CaenScope> > scopes;
    if (!connectScopes(addresses, scopes, options.verbose)) {
        return 1;
    }

    if (options.reboot) {
        bool ok = true;
        for (auto &scope: scopes) {
            const int rebootResult = scope->sendCommand("/cmd/reboot");
            if (rebootResult != CAEN_FELib_Success) {
                util::printErr("Reboot command failed with error ", rebootResult, '\n');
                ok = false;
            }
        }

        closeScopes(scopes, options.verbose);

        if (!ok) {
            return 1;
        }
        util::print("Reboot command sent to all connected devices. Wait a few seconds before reconnecting.\n");
        return 0;
    }

    if (!ensureOutputDirectory(options.outputDirectory)) {
        return 1;
    }

    for (std::size_t i = 0; i < scopes.size(); ++i) {
        if (!expandAndFlattenSettingsForScope(*scopes[i], addresses[i], settingsByAddress, channelDefaultsByAddress)) {
            closeScopes(scopes, options.verbose);
            return 1;
        }
    }

    if (!applySettingsForAllScopes(scopes, addresses, settingsByAddress, options.verbose)) {
        closeScopes(scopes, options.verbose);
        return 1;
    }

    if (options.testConnection) {
        for (std::size_t i = 0; i < scopes.size(); ++i) {
            util::print("\n=== Device ", i, " (", addresses[i], ") ===\n");
            printDeviceInfo(*scopes[i]);
        }
        closeScopes(scopes, options.verbose);
        return 0;
    }

    if (!options.getPaths.empty()) {
        for (std::size_t i = 0; i < scopes.size(); ++i) {
            util::print("\n=== Device ", i, " (", addresses[i], ") ===\n");
            printRequestedPaths(*scopes[i], options.getPaths);
        }
        closeScopes(scopes, options.verbose);
        return 0;
    }

    if (options.verbose) {
        spdlog::debug("Output directory: {}", options.outputDirectory);
    }

    std::vector<std::unique_ptr<Acquisition> > acquisitions;
    acquisitions.reserve(scopes.size());

    for (std::size_t i = 0; i < scopes.size(); ++i) {
        const std::string runToken = buildRunToken(*scopes[i]);

        if (options.verbose) {
            spdlog::debug("Run token (device {}): {}", i, runToken);
            spdlog::debug("Device output dir ({}): {}", addresses[i], options.outputDirectory);
        }

        auto acq = std::make_unique<Acquisition>(
            *scopes[i], options.outputDirectory, runToken, options.verbose);
        if (!acq->setup()) {
            for (auto &scope: scopes) {
                scope->close();
            }
            return 1;
        }
        acquisitions.push_back(std::move(acq));
    }

    for (auto &acq: acquisitions) {
        acq->start();
    }

    int totalChannels = 0;
    for (const auto &acq: acquisitions) {
        totalChannels += acq->channelCount();
    }

    // Launch master display thread to show per-device live table and totals
    std::atomic<bool> displayRunning{true};
    const bool useTui = !options.verbose;
    const std::string tuiOutputDir = options.outputDirectory;
    std::thread displayThread([&acquisitions, &displayRunning, useTui, totalChannels, tuiOutputDir]() {
        constexpr double kDisplayIntervalSeconds = 0.2;
        std::vector<uint64_t> prevBytesPerDevice(acquisitions.size(), 0);
        auto prevTime = std::chrono::steady_clock::now();
        bool firstDraw = true;

        struct DeviceSnap {
            uint64_t bytes;
            uint64_t saved;
            uint64_t trigCnt;
            uint64_t lostCnt;
            double throughputMBs;
        };

        while (displayRunning) {
            // ── Pass 1: sample bytes/saved for all devices as close together as
            //    possible (fast local reads) so per-device throughput uses the same dt.
            auto now = std::chrono::steady_clock::now();
            const double dt = std::max(1e-6, std::chrono::duration<double>(now - prevTime).count());
            prevTime = now;

            std::vector<DeviceSnap> snaps(acquisitions.size());
            for (std::size_t i = 0; i < acquisitions.size(); ++i) {
                snaps[i].bytes = acquisitions[i]->bytesWritten();
                snaps[i].saved = acquisitions[i]->eventsSaved();
                const uint64_t prev = prevBytesPerDevice[i];
                const uint64_t delta = (snaps[i].bytes >= prev) ? (snaps[i].bytes - prev) : 0;
                prevBytesPerDevice[i] = snaps[i].bytes;
                snaps[i].throughputMBs = (delta / 1e6) / dt;
            }

            // ── Pass 2: fetch trigger counts (slower network calls) once per device.
            for (std::size_t i = 0; i < acquisitions.size(); ++i) {
                snaps[i].trigCnt = acquisitions[i]->triggerCnt();
                snaps[i].lostCnt = acquisitions[i]->lostTriggerCnt();
            }

            // ── Accumulate totals.
            uint64_t totalTriggerCnt = 0;
            uint64_t totalLost = 0;
            uint64_t totalSaved = 0;
            uint64_t totalBytes = 0;
            double totalThroughputMBs = 0.0;
            for (const auto &s: snaps) {
                totalTriggerCnt += s.trigCnt;
                totalLost += s.lostCnt;
                totalSaved += s.saved;
                totalBytes += s.bytes;
                totalThroughputMBs += s.throughputMBs;
            }

            if (!useTui) {
                // In verbose mode just skip the visual table to avoid mangling logs.
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    static_cast<int>(kDisplayIntervalSeconds * 1000)));
                continue;
            }

            // ── Build FTXUI table.
            using namespace ftxui;

            // Helper: format bytes as human-readable string.
            auto fmtBytes = [](uint64_t b) -> std::string {
                char buf[32];
                if (b >= uint64_t(1e9))
                    snprintf(buf, sizeof(buf), "%.2f GB", b / 1e9);
                else if (b >= uint64_t(1e6))
                    snprintf(buf, sizeof(buf), "%.2f MB", b / 1e6);
                else if (b >= uint64_t(1e3))
                    snprintf(buf, sizeof(buf), "%.2f KB", b / 1e3);
                else
                    snprintf(buf, sizeof(buf), "%lu B", static_cast<unsigned long>(b));
                return buf;
            };

            // Column headers.
            std::vector<Element> headerCells = {
                text("#") | size(WIDTH, EQUAL, 3),
                text("Address") | size(WIDTH, EQUAL, 28),
                text("TrigCnt") | align_right | size(WIDTH, EQUAL, 12),
                text("Lost") | align_right | size(WIDTH, EQUAL, 12),
                text("Saved") | align_right | size(WIDTH, EQUAL, 12),
            };
            Element header = hbox(std::move(headerCells)) | bold | color(Color::Cyan);

            // Device rows.
            Elements rows;
            rows.push_back(text("Live Acquisition Stats") | bold | color(Color::Cyan));
            rows.push_back(text("Devices: " + std::to_string(acquisitions.size()) +
                                "  Channels: " + std::to_string(totalChannels) +
                                "  Output: " + tuiOutputDir) | dim);
            rows.push_back(separator());
            rows.push_back(header);
            rows.push_back(separator());

            for (std::size_t i = 0; i < acquisitions.size(); ++i) {
                const std::string addr = acquisitions[i]->address();
                const std::string shownAddr = (addr.size() > 28) ? addr.substr(0, 28) : addr;
                const auto &s = snaps[i];
                std::vector<Element> cells = {
                    text(std::to_string(i)) | size(WIDTH, EQUAL, 3),
                    text(shownAddr) | size(WIDTH, EQUAL, 28),
                    text(std::to_string(s.trigCnt)) | align_right | size(WIDTH, EQUAL, 12),
                    text(std::to_string(s.lostCnt)) | align_right | size(WIDTH, EQUAL, 12),
                    text(std::to_string(s.saved)) | align_right | size(WIDTH, EQUAL, 12),
                };
                rows.push_back(hbox(std::move(cells)));
            }

            rows.push_back(separator());

            // Total row.
            char mbsBuf[24];
            snprintf(mbsBuf, sizeof(mbsBuf), "%.2f MB/s", totalThroughputMBs);
            std::vector<Element> totalCells = {
                text("") | size(WIDTH, EQUAL, 3),
                text("TOTAL") | bold | size(WIDTH, EQUAL, 28),
                text(std::to_string(totalTriggerCnt)) | align_right | size(WIDTH, EQUAL, 12),
                text(std::to_string(totalLost)) | align_right | size(WIDTH, EQUAL, 12),
                text(std::to_string(totalSaved)) | align_right | size(WIDTH, EQUAL, 12),
            };
            std::vector<Element> totalExtra = {
                text(fmtBytes(totalBytes)) | align_right | size(WIDTH, EQUAL, 14),
                text(mbsBuf) | align_right | size(WIDTH, EQUAL, 12),
            };
            // Append extras into totalCells.
            for (auto &e: totalExtra) totalCells.push_back(std::move(e));
            rows.push_back(hbox(std::move(totalCells)) | color(Color::Green));

            rows.push_back(text("Press 'Q' to stop, 'T' to trigger.") | dim);

            Element doc = vbox(std::move(rows));

            // Render into an off-screen buffer sized to the document.
            auto screen = Screen::Create(Dimension::Full(), Dimension::Fit(doc));
            Render(screen, doc);

            // On subsequent draws rewind cursor to top of the last frame.
            if (firstDraw)
                firstDraw = false;
            else
                util::print(screen.ResetPosition());

            screen.Print();
            util::flush();

            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<int>(kDisplayIntervalSeconds * 1000)));
        }

        if (useTui) {
            util::print("\n");
            util::flush();
        }
    });

    {
        RawTerminal rawTerm;
        if (!useTui)
            util::print("Acquisition running. Press 'Q' to stop, 'T' to trigger.\n");

        while (true) {
            if (!anyAcquisitionRunning(acquisitions)) {
                break;
            }

            const char key = checkKeyPress();
            if (key == 'Q') {
                util::print("\nStop requested...\n");
                break;
            } else if (key == 'T') {
                for (auto &acq: acquisitions) {
                    acq->sendTrigger();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    stopAndJoinAcquisitions(acquisitions);

    // Stop and join the master display thread
    displayRunning = false;
    if (displayThread.joinable()) {
        displayThread.join();
    }

    printAcquisitionSummary(acquisitions);

    closeScopes(scopes, options.verbose);
    return 0;
}
