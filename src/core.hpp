#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace qb {
inline double monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Settings {
    int maxGainPercent = 2000;
    int silenceTimeoutMs = 5000;
    double silenceThresholdDb = -90;
    int bufferMs = 60;
    bool autoStart = true;
    std::wstring virtualId, outputId;
    void validate() const {
        if (maxGainPercent < 100 || maxGainPercent > 1000000) throw std::runtime_error("MaxGainPercent must be 100..1000000");
        if (silenceTimeoutMs < 1000 || silenceTimeoutMs > 600000) throw std::runtime_error("SilenceTimeoutMs must be 1000..600000");
        if (!std::isfinite(silenceThresholdDb) || silenceThresholdDb < -120 || silenceThresholdDb > -30) throw std::runtime_error("SilenceThresholdDb must be -120..-30");
        if (bufferMs < 20 || bufferMs > 500) throw std::runtime_error("BufferMs must be 20..500");
    }
};
class GainControl {
    Settings settings_;
    std::function<double()> clock_;
    std::atomic<double> activity_;
    std::atomic<double> target_{1};
    std::atomic<bool> autoRestore_{true};
    std::atomic<unsigned> resets_{0};
    std::mutex mutation_;
public:
    explicit GainControl(const Settings& settings, std::function<double()> clock = monotonic)
        : settings_(settings), clock_(std::move(clock)), activity_(clock_()) { settings_.validate(); }
    double target() const { return target_.load(); }
    bool automatic() const { return autoRestore_.load(); }
    unsigned resets() const { return resets_.load(); }
    double silentSeconds() const { return std::max(0.0, clock_() - activity_.load()); }
    void setPercent(double value) {
        if (!std::isfinite(value) || value < 100 || value > settings_.maxGainPercent) throw std::out_of_range("gain");
        std::lock_guard lock(mutation_);
        activity_.store(clock_()); target_.store(value / 100.0);
    }
    void observe(double peak) {
        if (std::isfinite(peak) && peak >= std::pow(10.0, settings_.silenceThresholdDb / 20.0)) { std::lock_guard lock(mutation_); activity_.store(clock_()); }
    }
    void reset() { target_.store(1); }
    void setAutomatic(bool value) { autoRestore_.store(value); if (value) tick(); }
    bool tick() {
        std::lock_guard lock(mutation_);
        if (!automatic() || silentSeconds() < settings_.silenceTimeoutMs / 1000.0) return false;
        double current = target();
        if (current > 1 && target_.compare_exchange_strong(current, 1)) { ++resets_; return true; }
        return false;
    }
};
class Dsp {
    double gain_ = 1, attenuation_ = 1;
    double smoothing_, release_;
public:
    explicit Dsp(int rate) : smoothing_(1 - std::exp(-1.0 / (rate * .03))), release_(1 - std::exp(-1.0 / (rate * .1))) {}
    bool limiting = false;
    float outputPeak = 0;
    void beginBlock() { limiting = false; outputPeak = 0; }
    void frame(float& left, float& right, double target) {
        if (!std::isfinite(left)) left = 0;
        if (!std::isfinite(right)) right = 0;
        gain_ += (target - gain_) * smoothing_;
        double peak = std::max(std::abs(static_cast<double>(left)), std::abs(static_cast<double>(right))) * gain_;
        double needed = peak > .98 ? .98 / peak : 1;
        if (needed < attenuation_) attenuation_ = needed;
        else attenuation_ += (needed - attenuation_) * release_;
        limiting = limiting || attenuation_ < .999;
        left = static_cast<float>(std::clamp(left * gain_ * attenuation_, -.98, .98));
        right = static_cast<float>(std::clamp(right * gain_ * attenuation_, -.98, .98));
        outputPeak = std::max({outputPeak, std::abs(left), std::abs(right)});
    }
};
struct Stereo { float left = 0, right = 0; };
class Ring {
    std::vector<Stereo> data_;
    size_t read_ = 0, write_ = 0, size_ = 0;
public:
    explicit Ring(size_t capacity) : data_(capacity) { if (!capacity) throw std::invalid_argument("capacity"); }
    size_t size() const { return size_; }
    void clear() { read_ = write_ = size_ = 0; }
    void push(Stereo frame) {
        if (size_ == data_.size()) { read_ = (read_ + 1) % data_.size(); --size_; }
        data_[write_] = frame; write_ = (write_ + 1) % data_.size(); ++size_;
    }
    Stereo pop() {
        if (!size_) return {};
        Stereo result = data_[read_]; read_ = (read_ + 1) % data_.size(); --size_; return result;
    }
};
struct Device { std::wstring id, name; bool virtualDevice = false; };
struct Snapshot { std::wstring virtualId, outputId, consoleId, multimediaId; unsigned long pid = 0; unsigned long long processTime = 0; };
struct EndpointService {
    virtual ~EndpointService() = default;
    virtual std::vector<Device> list() = 0;
    virtual std::wstring current(int role) = 0;
    virtual void select(const std::wstring& id, int role) = 0;
};
struct RecoveryResult { bool success = true; std::wstring message; };
inline RecoveryResult restore(EndpointService& endpoints, const Snapshot& record) {
    RecoveryResult result;
    auto fail = [&](const std::wstring& message) { result.success = false; result.message += message + L"\n"; };
    auto all = endpoints.list();
    for (int role = 0; role < 2; ++role) {
        try {
            if (endpoints.current(role) != record.virtualId) continue;
            auto desired = role == 0 ? record.consoleId : record.multimediaId;
            auto candidate = std::find_if(all.begin(), all.end(), [&](auto& d) { return d.id == desired && d.id != record.virtualId; });
            if (candidate == all.end()) candidate = std::find_if(all.begin(), all.end(), [&](auto& d) { return d.id == record.outputId && !d.virtualDevice; });
            if (candidate == all.end()) candidate = std::find_if(all.begin(), all.end(), [&](auto& d) { return !d.virtualDevice && d.id != record.virtualId; });
            if (candidate == all.end()) { fail(L"没有可用的实际输出设备，请连接耳机或扬声器。"); continue; }
            if (endpoints.current(role) == record.virtualId) endpoints.select(candidate->id, role);
        } catch (...) { fail(L"Windows 未能恢复输出设备。"); }
    }
    for (int role = 0; role < 2; ++role) try { if (endpoints.current(role) == record.virtualId) fail(L"默认输出仍指向虚拟设备。"); } catch (...) { fail(L"无法验证默认设备。"); }
    return result;
}
} // namespace qb
