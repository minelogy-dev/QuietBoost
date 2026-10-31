#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <stdexcept>
#include <sstream>
#include "core.hpp"

namespace qb {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
class Handle {
    HANDLE handle_ = nullptr;
public:
    explicit Handle(HANDLE handle = nullptr) : handle_(handle) {}
    ~Handle() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
};
struct ComApartment {
    HRESULT hr;
    explicit ComApartment(DWORD kind = COINIT_MULTITHREADED) : hr(CoInitializeEx(nullptr, kind)) { if (FAILED(hr)) throw std::runtime_error("COM initialization failed"); }
    ~ComApartment() { CoUninitialize(); }
};
inline void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) { std::ostringstream message; message << operation << " (0x" << std::hex << static_cast<unsigned long>(hr) << ")"; throw std::runtime_error(message.str()); }
}
std::wstring wide(const std::string& value);
std::wstring executablePath();
unsigned long long processTime(HANDLE process);
void atomicWrite(const fs::path& file, const std::wstring& content);
std::wstring readIni(const fs::path& file, const wchar_t* section, const wchar_t* key, const std::wstring& fallback = L"");
class StateStore {
public:
    fs::path root, config, journal;
    explicit StateStore(fs::path directory = {});
    Settings loadSettings();
    void saveSettings(const Settings& value);
    Snapshot loadSnapshot();
    void saveSnapshot(const Snapshot& value);
    void log(const std::wstring& message) const;
};
class LoginStartup {
    fs::path root_, directory_;
    std::wstring arguments() const;
    bool ownsLink() const;
public:
    explicit LoginStartup(const StateStore& store, fs::path startupDirectory = {});
    fs::path linkPath() const { return directory_ / L"QuietBoostNative.lnk"; }
    fs::path executable() const { return root_ / L"app" / L"QuietBoost.exe"; }
    bool enabled() const;
    void setEnabled(bool enabled, const fs::path& source = executablePath());
};
class Endpoints : public EndpointService {
public:
    std::vector<Device> list() override;
    std::wstring current(int role) override;
    void select(const std::wstring& id, int role) override;
    static bool isVirtual(const std::wstring& name);
};
RecoveryResult recover(StateStore& store, unsigned long expectedPid = 0, unsigned long long expectedTime = 0);
int watchdog(const std::vector<std::wstring>& args);
class Guardian {
    bool started_ = false;
public:
    void ensure(const StateStore& store);
};
void installDriver(HWND parent, StateStore& store);
fs::path verifyDriver(StateStore& store);
}
