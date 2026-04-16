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
    std::vector<uint32_t> lastSeenTrigId(acquisitions_.size(), 0);
    std::vector<bool> sawAnySnapshot(acquisitions_.size(), false);

    while (!stopRequested_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                stopRequested_ = true;
            }
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
        bool sawNewEvent = false;

        for (std::size_t dev = 0; dev < acquisitions_.size(); ++dev) {
            Acquisition::LatestEventSnapshot snapshot;
            if (!acquisitions_[dev]->getLatestEventSnapshot(snapshot)) {
                ImGui::Text("Device %zu (%s): waiting for first event...", dev,
                            acquisitions_[dev]->address().c_str());
                continue;
            }

            showedAny = true;

            if (!sawAnySnapshot[dev] || snapshot.trigId != lastSeenTrigId[dev]) {
                sawNewEvent = true;
                sawAnySnapshot[dev] = true;
                lastSeenTrigId[dev] = snapshot.trigId;
            }

            const int nChannels = snapshot.nChannels;
            const int samplesPerChannel = snapshot.recordLength;
            if (nChannels <= 0 || samplesPerChannel <= 0) {
                ImGui::Text("Device %zu (%s): invalid event metadata", dev,
                            acquisitions_[dev]->address().c_str());
                continue;
            }

            if (static_cast<int>(channelVisible[dev].size()) != nChannels) {
                channelVisible[dev].assign(nChannels, false);
                sawFirstEvent[dev] = false;
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

                const float availableY = ImGui::GetContentRegionAvail().y;
                const float panelHeight = std::max(kMinPlotHeight, availableY - kStatsBlockHeight);

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

                const int adcBits = std::max(1, snapshot.adcBits);
                const double adcScale = 2.0 / static_cast<double>((1 << adcBits) - 1);
                const double adcOffset = -1.0;

                const std::string plotTitle = "Waveforms##plot" + std::to_string(dev);
                if (ImPlot::BeginPlot(plotTitle.c_str(), ImVec2(-1.0f, -1.0f))) {
                    ImPlot::SetupAxes("Sample", "Voltage (V)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                    ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, static_cast<double>(samplesPerChannel), ImPlotCond_Always);

                    std::vector<float> y(static_cast<std::size_t>(samplesPerChannel), 0.0f);
                    for (int ch = 0; ch < nChannels; ++ch) {
                        if (!enabled[ch] || !channelVisible[dev][ch]) {
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

                        std::fill(y.begin(), y.end(), 0.0f);
                        for (uint32_t s = 0; s < validSamples; ++s) {
                            y[s] = static_cast<float>(snapshot.waveforms[base + s] * adcScale + adcOffset);
                        }

                        const std::string lineLabel = "CH" + std::to_string(ch);
                        ImPlot::PlotLine(lineLabel.c_str(), y.data(), samplesPerChannel);
                    }

                    ImPlot::EndPlot();
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

        if (!sawNewEvent) {
            SDL_Delay(1);
        }
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
