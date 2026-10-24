#pragma once
#include "platform.hpp"
#include <future>
#include <mutex>
#include <thread>

namespace qb {
class AudioEngine {
    Settings settings_;
    GainControl& gain_;
    Handle stop_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::thread worker_;
    std::mutex errorMutex_;
    std::wstring error_;
    void run(std::wstring input, std::wstring output, std::shared_ptr<std::promise<void>> ready, bool diagnostic);
public:
    std::atomic<float> inputPeak{0}, outputPeak{0};
    std::atomic<bool> limiting{false}, running{false};
    std::atomic<unsigned long> capturedFrames{0}, renderedFrames{0}, droppedFrames{0};
    AudioEngine(const Settings& settings, GainControl& gain) : settings_(settings), gain_(gain) {}
    ~AudioEngine() { stop(); }
    void start(const std::wstring& input, const std::wstring& output, bool diagnostic = false);
    void stop();
    std::wstring error();
};
class Route {
    Settings& settings_;
    StateStore& store_;
    Endpoints endpoints_;
    Guardian guardian_;
public:
    GainControl gain;
    std::unique_ptr<AudioEngine> engine;
    std::wstring virtualId, outputId;
    Route(Settings& settings, StateStore& store) : settings_(settings), store_(store), gain(settings) {}
    void start(const std::wstring& input, const std::wstring& output);
    RecoveryResult stop();
    void emergencyStop();
};
}
