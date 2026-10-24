#include "platform.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <iomanip>
#include <fstream>
#include <cwctype>

namespace qb {
std::wstring wide(const std::string& value) {
    int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (!count) return L"Unknown error";
    std::wstring result(count, 0); MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count); result.pop_back(); return result;
}
std::wstring executablePath() { std::wstring path(32768, 0); DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())); path.resize(size); return path; }
unsigned long long processTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) throw std::runtime_error("GetProcessTimes failed");
    return (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
void atomicWrite(const fs::path& file, const std::wstring& content) {
    fs::create_directories(file.parent_path());
    auto temporary = file; temporary += L".tmp";
    {
        Handle handle(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        if (!handle) throw std::runtime_error("Cannot create state file");
        std::wstring text = L"\xFEFF" + content;
        DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t)), written = 0;
        if (!WriteFile(handle.get(), text.data(), bytes, &written, nullptr) || written != bytes || !FlushFileBuffers(handle.get())) throw std::runtime_error("Cannot save state file");
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot commit state file");
}
std::wstring readIni(const fs::path& file, const wchar_t* section, const wchar_t* key, const std::wstring& fallback) {
    std::wstring value(32768, 0);
    DWORD size = GetPrivateProfileStringW(section, key, fallback.c_str(), value.data(), static_cast<DWORD>(value.size()), file.c_str());
    value.resize(size); return value;
}
StateStore::StateStore(fs::path directory) {
    if (directory.empty()) {
        PWSTR raw = nullptr; check(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw), "LocalAppData");
        directory = fs::path(raw) / L"QuietBoostNative"; CoTaskMemFree(raw);
    }
    root = fs::absolute(directory); fs::create_directories(root); config = root / L"config.ini"; journal = root / L"recovery.ini";
}
Settings StateStore::loadSettings() {
    Settings value;
    if (!fs::exists(config)) { saveSettings(value); return value; }
    auto integer = [&](const wchar_t* key, int initial) { return std::stoi(readIni(config, L"QuietBoost", key, std::to_wstring(initial))); };
    value.maxGainPercent = integer(L"MaxGainPercent", 2000);
    value.silenceTimeoutMs = integer(L"SilenceTimeoutMs", 5000);
    value.silenceThresholdDb = std::stod(readIni(config, L"QuietBoost", L"SilenceThresholdDb", L"-90"));
    value.bufferMs = integer(L"BufferMs", 60); value.autoStart = integer(L"AutoStart", 1) != 0;
    value.virtualId = readIni(config, L"QuietBoost", L"VirtualDeviceId"); value.outputId = readIni(config, L"QuietBoost", L"OutputDeviceId");
    value.validate(); return value;
}
void StateStore::saveSettings(const Settings& value) {
    value.validate(); std::wostringstream text; text.imbue(std::locale::classic());
    text << L"; QuietBoost native configuration. Current gain and temporary protection state are never saved.\r\n[QuietBoost]\r\nMaxGainPercent=" << value.maxGainPercent
        << L"\r\nSilenceTimeoutMs=" << value.silenceTimeoutMs << L"\r\nSilenceThresholdDb=" << value.silenceThresholdDb << L"\r\nBufferMs=" << value.bufferMs
        << L"\r\nAutoStart=" << (value.autoStart ? 1 : 0) << L"\r\nVirtualDeviceId=" << value.virtualId << L"\r\nOutputDeviceId=" << value.outputId << L"\r\n";
    atomicWrite(config, text.str());
}
Snapshot StateStore::loadSnapshot() {
    Snapshot value;
    value.virtualId = readIni(journal, L"Recovery", L"VirtualId"); value.outputId = readIni(journal, L"Recovery", L"OutputId");
    value.consoleId = readIni(journal, L"Recovery", L"ConsoleId"); value.multimediaId = readIni(journal, L"Recovery", L"MultimediaId");
    value.pid = std::stoul(readIni(journal, L"Recovery", L"Pid", L"0")); value.processTime = std::stoull(readIni(journal, L"Recovery", L"ProcessTime", L"0"));
    if (value.virtualId.empty()) throw std::runtime_error("Invalid recovery record"); return value;
}
void StateStore::saveSnapshot(const Snapshot& value) {
    std::wostringstream text;
    text << L"[Recovery]\r\nVirtualId=" << value.virtualId << L"\r\nOutputId=" << value.outputId << L"\r\nConsoleId=" << value.consoleId << L"\r\nMultimediaId=" << value.multimediaId
        << L"\r\nPid=" << value.pid << L"\r\nProcessTime=" << value.processTime << L"\r\n";
    atomicWrite(journal, text.str());
}
void StateStore::log(const std::wstring& message) const {
    try {
        int length = WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
        std::string encoded(length, '\0'); WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), encoded.data(), length, nullptr, nullptr);
        std::ofstream file(root / L"QuietBoost.log", std::ios::app | std::ios::binary); file << encoded << "\n";
    } catch (...) {}
}
bool Endpoints::isVirtual(const std::wstring& name) {
    std::wstring lower = name; std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return lower.find(L"vb-audio") != lower.npos || lower.find(L"virtual") != lower.npos || lower.find(L"voicemeeter") != lower.npos || lower.find(L"虚拟") != lower.npos;
}
std::vector<Device> Endpoints::list() {
    ComPtr<IMMDeviceEnumerator> enumerator; check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "MMDeviceEnumerator");
    ComPtr<IMMDeviceCollection> collection; check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "EnumAudioEndpoints");
    UINT count = 0; check(collection->GetCount(&count), "GetCount"); std::vector<Device> result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device; check(collection->Item(i, &device), "Item");
        LPWSTR rawId = nullptr; check(device->GetId(&rawId), "GetId"); std::wstring id(rawId); CoTaskMemFree(rawId);
        ComPtr<IPropertyStore> properties; check(device->OpenPropertyStore(STGM_READ, &properties), "OpenPropertyStore");
        PROPVARIANT name; PropVariantInit(&name); check(properties->GetValue(PKEY_Device_FriendlyName, &name), "FriendlyName");
        std::wstring text = name.vt == VT_LPWSTR ? name.pwszVal : L"Audio device"; PropVariantClear(&name);
        result.push_back({id, text, isVirtual(text)});
    }
    return result;
}
std::wstring Endpoints::current(int role) {
    ComPtr<IMMDeviceEnumerator> enumerator; check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "MMDeviceEnumerator");
    ComPtr<IMMDevice> device; HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, static_cast<ERole>(role), &device);
    if (hr == E_NOTFOUND || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return L""; check(hr, "GetDefaultAudioEndpoint");
    LPWSTR raw = nullptr; check(device->GetId(&raw), "GetId"); std::wstring id(raw); CoTaskMemFree(raw); return id;
}
// Only SetDefaultEndpoint is called. Other declarations preserve the COM vtable positions.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, BOOL, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, BOOL, INT64*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};
struct __declspec(uuid("870af99c-171d-4f9e-af0d-e63df40c2bc9")) PolicyConfigClient;
void Endpoints::select(const std::wstring& id, int role) {
    ComPtr<IPolicyConfig> policy; check(CoCreateInstance(__uuidof(PolicyConfigClient), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy)), "PolicyConfig");
    check(policy->SetDefaultEndpoint(id.c_str(), static_cast<ERole>(role)), "SetDefaultEndpoint");
    if (current(role) != id) throw std::runtime_error("Default device switch not confirmed");
}
RecoveryResult recover(StateStore& store, unsigned long expectedPid, unsigned long long expectedTime) {
    Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\QuietBoostNative.Recovery.v1"));
    DWORD wait = WaitForSingleObject(mutex.get(), 5000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return {false, L"另一个设备恢复操作尚未结束。"};
    RecoveryResult result;
    try {
        if (fs::exists(store.journal)) {
            auto record = store.loadSnapshot();
            if (!expectedPid || (record.pid == expectedPid && record.processTime == expectedTime)) {
                Endpoints endpoints; result = restore(endpoints, record);
                if (result.success) fs::remove(store.journal);
            }
        }
    } catch (const std::exception& e) { result = {false, wide(e.what())}; }
    ReleaseMutex(mutex.get()); if (!result.success) store.log(result.message); return result;
}
void Guardian::ensure(const StateStore& store) {
    if (started_) return;
    std::wstring eventName = L"Local\\QuietBoostNative.Guard." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str())); if (!ready) throw std::runtime_error("Cannot create guard event");
    std::wstring command = L"\"" + executablePath() + L"\" --watchdog " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(processTime(GetCurrentProcess())) + L" \"" + store.root.wstring() + L"\" " + eventName;
    STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE; PROCESS_INFORMATION process{};
    if (!CreateProcessW(executablePath().c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) throw std::runtime_error("Cannot start recovery guardian");
    Handle processHandle(process.hProcess), threadHandle(process.hThread);
    if (WaitForSingleObject(ready.get(), 4000) != WAIT_OBJECT_0) throw std::runtime_error("Recovery guardian did not become ready");
    started_ = true;
}
int watchdog(const std::vector<std::wstring>& args) {
    DWORD pid = std::stoul(args.at(1)); auto expectedTime = std::stoull(args.at(2));
    StateStore store(args.at(3)); Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, args.at(4).c_str()));
    Handle parent(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    bool matching = parent && processTime(parent.get()) == expectedTime;
    if (ready) SetEvent(ready.get());
    if (matching) WaitForSingleObject(parent.get(), INFINITE);
    for (int i = 0; i < 5; ++i) { if (recover(store, pid, expectedTime).success) return 0; Sleep(1000); }
    return 1;
}
}
