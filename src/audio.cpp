#include "audio.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <avrt.h>
#include <cstring>
#include <cstdint>

namespace qb {
namespace {
struct FormatDeleter { void operator()(WAVEFORMATEX* value) const { CoTaskMemFree(value); } };
struct Scheduler {
    DWORD index = 0; HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    ~Scheduler() { if (task) AvRevertMmThreadCharacteristics(task); }
};
struct StopClients {
    IAudioClient* capture; IAudioClient* render;
    ~StopClients() { if (capture) capture->Stop(); if (render) render->Stop(); }
};
struct Decoder {
    int channels, bytes; bool floating; std::vector<Stereo> weights;
    explicit Decoder(const WAVEFORMATEX* format) {
        channels = format->nChannels; bytes = format->wBitsPerSample / 8;
        if (channels < 1 || channels > 16) throw std::runtime_error("Virtual audio device must have 1..16 channels");
        WORD tag = format->wFormatTag; DWORD mask = 0;
        if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
            auto extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format); mask = extended->dwChannelMask;
            if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) tag = WAVE_FORMAT_PCM;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT;
        if ((floating && bytes != 4) || (!floating && (tag != WAVE_FORMAT_PCM || (bytes != 2 && bytes != 3 && bytes != 4))))
            throw std::runtime_error("Unsupported virtual-device sample format");
        if (format->nBlockAlign != channels * bytes) throw std::runtime_error("Unsupported packed audio format");
        weights.resize(channels);
        if (channels == 1) { weights[0] = {1, 1}; return; }
        if (!mask) { weights[0].left = 1; weights[1].right = 1; if (channels > 2) weights[2] = {.7071f, .7071f}; for (int c = 4; c < channels; ++c) (c % 2 ? weights[c].right : weights[c].left) = .5f; return; }
        int channel = 0;
        for (DWORD bit = 1; bit && channel < channels; bit <<= 1) if (mask & bit) {
            Stereo weight{};
            switch (bit) {
                case SPEAKER_FRONT_LEFT: weight.left = 1; break;
                case SPEAKER_FRONT_RIGHT: weight.right = 1; break;
                case SPEAKER_FRONT_CENTER: weight = {.7071f, .7071f}; break;
                case SPEAKER_LOW_FREQUENCY: weight = {.25f, .25f}; break;
                case SPEAKER_BACK_LEFT: case SPEAKER_SIDE_LEFT: case SPEAKER_FRONT_LEFT_OF_CENTER: weight.left = .7071f; break;
                case SPEAKER_BACK_RIGHT: case SPEAKER_SIDE_RIGHT: case SPEAKER_FRONT_RIGHT_OF_CENTER: weight.right = .7071f; break;
                default: weight = {.35f, .35f}; break;
            }
            weights[channel++] = weight;
        }
    }
    Stereo frame(const BYTE* data, double& peak) const {
        Stereo result{};
        for (int c = 0; c < channels; ++c) {
            const BYTE* p = data + c * bytes; double sample = 0;
            if (floating) { float value; std::memcpy(&value, p, 4); sample = value; }
            else if (bytes == 2) { int16_t value; std::memcpy(&value, p, 2); sample = value / 32768.0; }
            else if (bytes == 3) { uint32_t packed = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16); int32_t value = static_cast<int32_t>((packed & 0x800000u) ? packed | 0xFF000000u : packed); sample = value / 8388608.0; }
            else { int32_t value; std::memcpy(&value, p, 4); sample = value / 2147483648.0; }
            if (!std::isfinite(sample)) sample = 0;
            peak = std::max(peak, std::abs(sample));
            result.left += static_cast<float>(sample * weights[c].left); result.right += static_cast<float>(sample * weights[c].right);
        }
        return result;
    }
};
}
void AudioEngine::start(const std::wstring& input, const std::wstring& output, bool diagnostic) {
    if (worker_.joinable()) throw std::runtime_error("Audio engine already started");
    if (input == output && !diagnostic) throw std::runtime_error("Input and output cannot be the same endpoint");
    ResetEvent(stop_.get()); auto ready = std::make_shared<std::promise<void>>(); auto future = ready->get_future();
    worker_ = std::thread(&AudioEngine::run, this, input, output, ready, diagnostic);
    try {
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) throw std::runtime_error("Audio engine readiness timed out");
        future.get();
    } catch (...) { stop(); throw; }
}
void AudioEngine::stop() { if (stop_) SetEvent(stop_.get()); if (worker_.joinable()) worker_.join(); running.store(false); }
std::wstring AudioEngine::error() { std::lock_guard lock(errorMutex_); return error_; }
void AudioEngine::run(std::wstring input, std::wstring output, std::shared_ptr<std::promise<void>> ready, bool diagnostic) {
    bool announced = false;
    try {
        ComApartment apartment; Scheduler scheduler;
        ComPtr<IMMDeviceEnumerator> enumerator; check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "MMDeviceEnumerator");
        ComPtr<IMMDevice> inDevice, outDevice; check(enumerator->GetDevice(input.c_str(), &inDevice), "Virtual device"); check(enumerator->GetDevice(output.c_str(), &outDevice), "Physical device");
        ComPtr<IAudioClient> inClient, outClient;
        check(inDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &inClient), "Capture activation");
        check(outDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &outClient), "Render activation");
        WAVEFORMATEX* raw = nullptr; check(inClient->GetMixFormat(&raw), "Capture mix format"); std::unique_ptr<WAVEFORMATEX, FormatDeleter> inputFormat(raw);
        Decoder decoder(inputFormat.get()); int rate = static_cast<int>(inputFormat->nSamplesPerSec);
        WAVEFORMATEXTENSIBLE outputFormat{}; outputFormat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        outputFormat.Format.nChannels = 2; outputFormat.Format.nSamplesPerSec = rate; outputFormat.Format.wBitsPerSample = 32;
        outputFormat.Format.nBlockAlign = 8; outputFormat.Format.nAvgBytesPerSec = rate * 8; outputFormat.Format.cbSize = 22;
        outputFormat.Samples.wValidBitsPerSample = 32; outputFormat.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT; outputFormat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        Handle captureEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr)), renderEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!captureEvent || !renderEvent) throw std::runtime_error("Cannot create audio event");
        REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(settings_.bufferMs) * 10000;
        check(inClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0, inputFormat.get(), nullptr), "Capture initialize");
        check(outClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration, 0, &outputFormat.Format, nullptr), "Render initialize");
        check(inClient->SetEventHandle(captureEvent.get()), "Capture event"); check(outClient->SetEventHandle(renderEvent.get()), "Render event");
        ComPtr<IAudioCaptureClient> capture; ComPtr<IAudioRenderClient> render;
        check(inClient->GetService(IID_PPV_ARGS(&capture)), "Capture service"); check(outClient->GetService(IID_PPV_ARGS(&render)), "Render service");
        UINT32 capacity = 0; check(outClient->GetBufferSize(&capacity), "Render buffer size");
        BYTE* initial = nullptr; check(render->GetBuffer(capacity, &initial), "Render priming"); check(render->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT), "Render priming release");
        StopClients cleanup{inClient.Get(), outClient.Get()};
        check(inClient->Start(), "Capture start"); check(outClient->Start(), "Render start");
        Ring ring(static_cast<size_t>(rate) / 4); Dsp dsp(rate);
        HANDLE events[] = {stop_.get(), captureEvent.get(), renderEvent.get()};
        running.store(true); ready->set_value(); announced = true;
        while (true) {
            DWORD wait = WaitForMultipleObjects(3, events, FALSE, 20);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) throw std::runtime_error("Audio event wait failed");
            gain_.tick();
            UINT32 packet = 0; check(capture->GetNextPacketSize(&packet), "Capture packet size");
            double peak = 0; bool received = false;
            for (unsigned batch = 0; packet && batch < 16; ++batch) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
                check(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Capture buffer");
                received = true;
                if (ring.size() > static_cast<size_t>(rate * .18)) { droppedFrames.fetch_add(static_cast<unsigned long>(ring.size())); ring.clear(); }
                for (UINT32 f = 0; f < frames; ++f) {
                    Stereo sample{};
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) sample = decoder.frame(data + static_cast<size_t>(f) * inputFormat->nBlockAlign, peak);
                    // Diagnostic mode must never forward captured physical-device audio back to itself.
                    ring.push(diagnostic ? Stereo{} : sample);
                }
                capturedFrames.fetch_add(frames);
                check(capture->ReleaseBuffer(frames), "Capture release"); check(capture->GetNextPacketSize(&packet), "Capture next packet");
            }
            if (received) { inputPeak.store(static_cast<float>(peak)); gain_.observe(peak); }
            UINT32 padding = 0; check(outClient->GetCurrentPadding(&padding), "Render padding");
            UINT32 available = capacity - std::min(capacity, padding);
            if (available) {
                BYTE* rawOutput = nullptr; check(render->GetBuffer(available, &rawOutput), "Render buffer");
                float* floats = reinterpret_cast<float*>(rawOutput); double target = gain_.target(); dsp.beginBlock();
                for (UINT32 f = 0; f < available; ++f) {
                    Stereo frame = ring.pop(); dsp.frame(frame.left, frame.right, target); floats[f * 2] = frame.left; floats[f * 2 + 1] = frame.right;
                }
                check(render->ReleaseBuffer(available, 0), "Render release"); renderedFrames.fetch_add(available);
                outputPeak.store(dsp.outputPeak); limiting.store(dsp.limiting);
            }
        }
    } catch (const std::exception& e) {
        { std::lock_guard lock(errorMutex_); error_ = wide(e.what()); }
        gain_.reset();
        if (!announced) try { ready->set_exception(std::current_exception()); } catch (...) {}
    }
    running.store(false); outputPeak.store(0);
}
void Route::start(const std::wstring& input, const std::wstring& output) {
    if (engine) return;
    auto all = endpoints_.list();
    if (!std::any_of(all.begin(), all.end(), [&](auto& d) { return d.id == input && d.virtualDevice; })) throw std::runtime_error("Select an installed virtual audio device");
    if (!std::any_of(all.begin(), all.end(), [&](auto& d) { return d.id == output && !d.virtualDevice; })) throw std::runtime_error("Select a physical output device");
    auto recovered = recover(store_); if (!recovered.success) throw std::runtime_error("Previous output recovery is incomplete");
    auto oldConsole = endpoints_.current(0), oldMedia = endpoints_.current(1);
    gain.reset(); auto next = std::make_unique<AudioEngine>(settings_, gain); next->start(input, output);
    bool saved = false;
    try {
        guardian_.ensure(store_);
        store_.saveSnapshot({input, output, oldConsole == input ? output : oldConsole, oldMedia == input ? output : oldMedia, GetCurrentProcessId(), processTime(GetCurrentProcess())}); saved = true;
        if (!next->running.load()) throw std::runtime_error("Audio engine stopped before device switching");
        endpoints_.select(input, 0); endpoints_.select(input, 1);
        virtualId = input; outputId = output; engine = std::move(next);
    } catch (...) {
        if (saved && !recover(store_).success) { virtualId = input; outputId = output; engine = std::move(next); }
        throw;
    }
}
RecoveryResult Route::stop() {
    gain.reset(); if (engine) Sleep(180);
    auto result = recover(store_); if (result.success) engine.reset(); return result;
}
void Route::emergencyStop() { gain.reset(); recover(store_); engine.reset(); }
}
