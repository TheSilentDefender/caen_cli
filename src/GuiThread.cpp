#include "GuiThread.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL2/SDL.h>
#include <imgui.h>
#include <implot.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_sdlrenderer2.h>

namespace {
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 900;
constexpr uint32_t kWindowFlags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
constexpr uint32_t kRendererFlags = SDL_RENDERER_ACCELERATED;
constexpr float kChannelPanelWidth = 220.0f;
constexpr float kMinPlotHeight = 240.0f;
constexpr float kStatsBlockHeight = 70.0f;
constexpr std::size_t kPlotDownsampleFactor = 4;
} // namespace

GuiThread::GuiThread(const std::vector<std::unique_ptr<Acquisition>> &acquisitions)
    : acquisitions_(acquisitions) {
}

GuiThread::~GuiThread() {
    stop();
    join();
}

bool GuiThread::start() {
    stopRequested_ = false;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(startMutex_);
        startReady_ = false;
        startOk_ = false;
    }

    thread_ = std::thread([this] { guiLoop(); });

    std::unique_lock<std::mutex> lock(startMutex_);
    startCv_.wait(lock, [this] { return startReady_; });
    if (!startOk_) {
        running_ = false;
    }

    return startOk_;
}

void GuiThread::stop() {
    stopRequested_ = true;
}

void GuiThread::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void GuiThread::guiLoop() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::lock_guard<std::mutex> lock(startMutex_);
        startOk_ = false;
        startReady_ = true;
        startCv_.notify_one();
        return;
    }

    SDL_Window *window = SDL_CreateWindow("caen_cli --gui", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                           kWindowWidth, kWindowHeight, kWindowFlags);
    if (!window) {
        SDL_Quit();
        std::lock_guard<std::mutex> lock(startMutex_);
        startOk_ = false;
        startReady_ = true;
        startCv_.notify_one();
        return;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, kRendererFlags);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::lock_guard<std::mutex> lock(startMutex_);
        startOk_ = false;
        startReady_ = true;
        startCv_.notify_one();
        return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();

    // Use standard LMB drag for box selection in this app.
    ImPlotInputMap &inputMap = ImPlot::GetInputMap();
    inputMap.Select = ImGuiMouseButton_Left;
    inputMap.SelectCancel = ImGuiMouseButton_Right;
    inputMap.Pan = ImGuiMouseButton_Middle;

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    {
        std::lock_guard<std::mutex> lock(startMutex_);
        startOk_ = true;
        startReady_ = true;
        startCv_.notify_one();
    }

    std::vector<std::vector<bool>> channelVisible(acquisitions_.size());
    std::vector<bool> sawFirstEvent(acquisitions_.size(), false);
    std::vector<Acquisition::LatestEventSnapshotPtr> lastSnapshots(acquisitions_.size());
    std::vector<std::vector<std::vector<float>>> plotBuffers(acquisitions_.size());
    std::vector<std::vector<std::vector<float>>> plotXBuffers(acquisitions_.size());
    std::vector<bool> hasZoomWindow(acquisitions_.size(), false);
    std::vector<double> zoomXMin(acquisitions_.size(), 0.0);
    std::vector<double> zoomXMax(acquisitions_.size(), 0.0);
    std::vector<double> zoomYMin(acquisitions_.size(), -1.1);
    std::vector<double> zoomYMax(acquisitions_.size(), 1.1);

    while (!stopRequested_) {
        bool renderNeeded = false;
        std::vector<Acquisition::LatestEventSnapshotPtr> activeSnapshots(acquisitions_.size());
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            renderNeeded = true;
            if (event.type == SDL_QUIT) {
                stopRequested_ = true;
            }
        }

        for (std::size_t dev = 0; dev < acquisitions_.size(); ++dev) {
            Acquisition::LatestEventSnapshotPtr snapshotPtr = acquisitions_[dev]->getLatestEventSnapshot();
            activeSnapshots[dev] = snapshotPtr;
            if (!snapshotPtr) {
                continue;
            }

            if (snapshotPtr != lastSnapshots[dev]) {
                renderNeeded = true;
                lastSnapshots[dev] = snapshotPtr;
            }

            const auto &snapshot = *snapshotPtr;

            const int nChannels = snapshot.nChannels;
            const int samplesPerChannel = snapshot.recordLength;
            if (nChannels <= 0 || samplesPerChannel <= 0) {
                continue;
            }

            if (static_cast<int>(channelVisible[dev].size()) != nChannels) {
                channelVisible[dev].assign(static_cast<std::size_t>(nChannels), false);
                sawFirstEvent[dev] = false;
                renderNeeded = true;
                hasZoomWindow[dev] = false;
            }

            std::vector<std::vector<float>> &devicePlotBuffers = plotBuffers[dev];
            std::vector<std::vector<float>> &devicePlotXBuffers = plotXBuffers[dev];
            if (static_cast<int>(devicePlotBuffers.size()) != nChannels) {
                devicePlotBuffers.assign(static_cast<std::size_t>(nChannels), {});
                devicePlotXBuffers.assign(static_cast<std::size_t>(nChannels), {});
                renderNeeded = true;
            }

            std::vector<bool> enabled(nChannels, false);
            int enabledCount = 0;
            for (int ch = 0; ch < nChannels; ++ch) {
                const bool isEnabled = ch < static_cast<int>(snapshot.waveformSizes.size()) && snapshot.waveformSizes[ch] > 0;
                enabled[ch] = isEnabled;
                if (isEnabled) {
                    ++enabledCount;
                }
            }

            if (!sawFirstEvent[dev]) {
                for (int ch = 0; ch < nChannels; ++ch) {
                    if (enabled[ch]) {
                        channelVisible[dev][ch] = true;
                    }
                }
                sawFirstEvent[dev] = true;
                renderNeeded = true;
                hasZoomWindow[dev] = false;
            }

            const int adcBits = std::max(1, snapshot.adcBits);
            const double adcScale = 2.0 / static_cast<double>((1 << adcBits) - 1);
            const double adcOffset = -1.0;

            for (int ch = 0; ch < nChannels; ++ch) {
                std::vector<float> &buffer = devicePlotBuffers[static_cast<std::size_t>(ch)];
                std::vector<float> &xbuffer = devicePlotXBuffers[static_cast<std::size_t>(ch)];

                const std::size_t downsampleFactor = std::max<std::size_t>(1, kPlotDownsampleFactor);
                const std::size_t reducedPoints = (static_cast<std::size_t>(samplesPerChannel) + downsampleFactor - 1) / downsampleFactor;

                if (buffer.size() != reducedPoints || xbuffer.size() != reducedPoints) {
                    buffer.assign(reducedPoints, 0.0f);
                    xbuffer.assign(reducedPoints, 0.0f);
                    renderNeeded = true;
                }

                if (!enabled[ch]) {
                    continue;
                }

                const std::size_t base = static_cast<std::size_t>(ch) * static_cast<std::size_t>(samplesPerChannel);
                if (base + static_cast<std::size_t>(samplesPerChannel) > snapshot.waveforms.size()) {
                    continue;
                }

                const uint32_t validSamples = static_cast<uint32_t>(std::min<std::size_t>(
                    static_cast<std::size_t>(samplesPerChannel),
                    ch < static_cast<int>(snapshot.waveformSizes.size())
                        ? static_cast<std::size_t>(snapshot.waveformSizes[ch])
                        : static_cast<std::size_t>(0)));

                std::size_t pointIndex = 0;
                for (std::size_t sampleIndex = 0; sampleIndex < validSamples; sampleIndex += downsampleFactor, ++pointIndex) {
                    buffer[pointIndex] = static_cast<float>(snapshot.waveforms[base + sampleIndex] * adcScale + adcOffset);
                    xbuffer[pointIndex] = static_cast<float>(sampleIndex);
                }
                for (std::size_t point = pointIndex; point < reducedPoints; ++point) {
                    buffer[point] = 0.0f;
                    xbuffer[point] = static_cast<float>(point * downsampleFactor);
                }
            }
        }

        if (!renderNeeded) {
            SDL_Delay(1);
            continue;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(1260.0f, 860.0f), ImGuiCond_Once);
        ImGui::Begin("Most Recent Event", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_MenuBar);

        ImGui::TextUnformatted("Only enabled channels are listed. Use checkboxes to filter what is shown.");
        ImGui::Separator();

        bool showedAny = false;
        for (std::size_t dev = 0; dev < acquisitions_.size(); ++dev) {
            const auto &snapshotPtr = activeSnapshots[dev];
            if (!snapshotPtr) {
                ImGui::Text("Device %zu (%s): waiting for first event...", dev,
                            acquisitions_[dev]->address().c_str());
                continue;
            }

            showedAny = true;
            const auto &snapshot = *snapshotPtr;
            const int nChannels = snapshot.nChannels;
            const int samplesPerChannel = snapshot.recordLength;
            if (nChannels <= 0 || samplesPerChannel <= 0) {
                ImGui::Text("Device %zu (%s): invalid event metadata", dev,
                            acquisitions_[dev]->address().c_str());
                continue;
            }

            std::vector<bool> enabled(nChannels, false);
            int enabledCount = 0;
            for (int ch = 0; ch < nChannels; ++ch) {
                const bool isEnabled = ch < static_cast<int>(snapshot.waveformSizes.size()) && snapshot.waveformSizes[ch] > 0;
                enabled[ch] = isEnabled;
                if (isEnabled) {
                    ++enabledCount;
                }
            }

            std::string header = "Device " + std::to_string(dev) + " - " + acquisitions_[dev]->address();
            if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Button((std::string("Show all enabled##") + std::to_string(dev)).c_str())) {
                    for (int ch = 0; ch < nChannels; ++ch) {
                        channelVisible[dev][ch] = enabled[ch];
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string("Hide all##") + std::to_string(dev)).c_str())) {
                    for (int ch = 0; ch < nChannels; ++ch) {
                        channelVisible[dev][ch] = false;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button((std::string("Reset zoom##") + std::to_string(dev)).c_str())) {
                    hasZoomWindow[dev] = false;
                }

                const float availableY = ImGui::GetContentRegionAvail().y;
                const float panelHeight = std::max(kMinPlotHeight, availableY - kStatsBlockHeight);
                const float overviewHeight = std::max(140.0f, panelHeight * 0.58f);
                const float zoomHeight = std::max(120.0f, panelHeight - overviewHeight - 8.0f);

                ImGui::BeginChild((std::string("ChannelFilterPanel##") + std::to_string(dev)).c_str(),
                                  ImVec2(kChannelPanelWidth, panelHeight), true,
                                  ImGuiWindowFlags_AlwaysVerticalScrollbar);
                ImGui::Text("Enabled channels (%d)", enabledCount);
                ImGui::Separator();
                for (int ch = 0; ch < nChannels; ++ch) {
                    if (!enabled[ch]) {
                        continue;
                    }

                    std::string checkboxLabel = "CH" + std::to_string(ch) + "##dev" + std::to_string(dev) + "ch" +
                                                std::to_string(ch);
                    bool value = channelVisible[dev][ch];
                    if (ImGui::Checkbox(checkboxLabel.c_str(), &value)) {
                        channelVisible[dev][ch] = value;
                    }
                }
                ImGui::EndChild();

                ImGui::SameLine();
                ImGui::BeginChild((std::string("PlotPanel##") + std::to_string(dev)).c_str(),
                                  ImVec2(0.0f, panelHeight), false);

                const std::string plotTitle = "Waveforms##plot" + std::to_string(dev);
                if (ImPlot::BeginPlot(plotTitle.c_str(), ImVec2(-1.0f, overviewHeight))) {
                    ImPlot::SetupAxes("Sample", "Voltage (V)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                    ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(samplesPerChannel), ImPlotCond_Always);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, -1.1, 1.1, ImPlotCond_Once);

                    const std::vector<std::vector<float>> &devicePlotBuffers = plotBuffers[dev];
                    const std::vector<std::vector<float>> &devicePlotXBuffers = plotXBuffers[dev];
                    for (int ch = 0; ch < nChannels; ++ch) {
                        if (!enabled[ch] || !channelVisible[dev][ch]) {
                            continue;
                        }

                        const std::vector<float> &buffer = devicePlotBuffers[static_cast<std::size_t>(ch)];
                        const std::vector<float> &xbuffer = devicePlotXBuffers[static_cast<std::size_t>(ch)];
                        if (buffer.empty() || buffer.size() != xbuffer.size()) {
                            continue;
                        }

                        const std::string lineLabel = "CH" + std::to_string(ch);
                        ImPlot::PlotLine(lineLabel.c_str(), xbuffer.data(), buffer.data(), static_cast<int>(buffer.size()));
                    }

                    if (ImPlot::IsPlotSelected()) {
                        ImPlotRect selection = ImPlot::GetPlotSelection(ImAxis_X1, ImAxis_Y1);
                        double xMin = std::min(selection.X.Min, selection.X.Max);
                        double xMax = std::max(selection.X.Min, selection.X.Max);
                        double yMin = std::min(selection.Y.Min, selection.Y.Max);
                        double yMax = std::max(selection.Y.Min, selection.Y.Max);
                        xMin = std::clamp(xMin, 0.0, static_cast<double>(samplesPerChannel - 1));
                        xMax = std::clamp(xMax, 0.0, static_cast<double>(samplesPerChannel - 1));
                        if (xMax > xMin && yMax > yMin) {
                            zoomXMin[dev] = xMin;
                            zoomXMax[dev] = xMax;
                            zoomYMin[dev] = yMin;
                            zoomYMax[dev] = yMax;
                            hasZoomWindow[dev] = true;
                        }
                        ImPlot::CancelPlotSelection();
                    }

                    ImPlot::EndPlot();
                }

                ImGui::Dummy(ImVec2(0.0f, 6.0f));

                const std::vector<std::vector<float>> &devicePlotBuffers = plotBuffers[dev];
                const std::vector<std::vector<float>> &devicePlotXBuffers = plotXBuffers[dev];
                if (hasZoomWindow[dev]) {
                    ImGui::Text("Zoomed view: X[%.0f, %.0f]  Y[%.3f, %.3f]",
                                zoomXMin[dev], zoomXMax[dev], zoomYMin[dev], zoomYMax[dev]);
                    const std::string zoomTitle = "Zoomed Waveforms##zoomplot" + std::to_string(dev);
                    if (ImPlot::BeginPlot(zoomTitle.c_str(), ImVec2(-1.0f, zoomHeight))) {
                        ImPlot::SetupAxes("Sample", "Voltage (V)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                        ImPlot::SetupAxisLimits(ImAxis_X1, zoomXMin[dev], zoomXMax[dev], ImPlotCond_Always);
                        ImPlot::SetupAxisLimits(ImAxis_Y1, zoomYMin[dev], zoomYMax[dev], ImPlotCond_Always);

                        for (int ch = 0; ch < nChannels; ++ch) {
                            if (!enabled[ch] || !channelVisible[dev][ch]) {
                                continue;
                            }

                            const std::vector<float> &buffer = devicePlotBuffers[static_cast<std::size_t>(ch)];
                            const std::vector<float> &xbuffer = devicePlotXBuffers[static_cast<std::size_t>(ch)];
                            if (buffer.empty() || buffer.size() != xbuffer.size()) {
                                continue;
                            }

                            const auto beginIt = std::lower_bound(xbuffer.begin(), xbuffer.end(), static_cast<float>(zoomXMin[dev]));
                            const auto endIt = std::upper_bound(xbuffer.begin(), xbuffer.end(), static_cast<float>(zoomXMax[dev]));
                            const int startIndex = static_cast<int>(std::distance(xbuffer.begin(), beginIt));
                            const int endIndex = static_cast<int>(std::distance(xbuffer.begin(), endIt));
                            const int count = endIndex - startIndex;
                            if (count <= 1) {
                                continue;
                            }

                            const std::string lineLabel = "CH" + std::to_string(ch) + "##zoom" + std::to_string(dev);
                            ImPlot::PlotLine(lineLabel.c_str(), xbuffer.data() + startIndex, buffer.data() + startIndex, count);
                        }

                        ImPlot::EndPlot();
                    }
                } else {
                    ImGui::TextUnformatted("Drag a box on the main plot to open a zoomed view here.");
                }

                ImGui::EndChild();

                int visibleCount = 0;
                for (int ch = 0; ch < nChannels; ++ch) {
                    if (enabled[ch] && channelVisible[dev][ch]) {
                        ++visibleCount;
                    }
                }

                ImGui::Separator();
                ImGui::Text("Stats: Trigger ID=%u  Timestamp=%llu  Enabled=%d  Showing=%d  Samples/channel=%d",
                            snapshot.trigId,
                            static_cast<unsigned long long>(snapshot.timestamp),
                            enabledCount,
                            visibleCount,
                            samplesPerChannel);
                ImGui::Separator();
            }
        }

        if (!showedAny) {
            ImGui::TextUnformatted("No events received yet.");
        }

        ImGui::End();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 20, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    running_ = false;
}
