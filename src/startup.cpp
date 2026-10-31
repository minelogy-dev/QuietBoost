#include "platform.hpp"
#include <shlobj.h>
#include <shobjidl.h>

namespace qb {
namespace {
constexpr wchar_t startupDescription[] = L"QuietBoost managed login startup";
bool samePath(const fs::path& a, const fs::path& b) {
    return _wcsicmp(fs::absolute(a).lexically_normal().c_str(), fs::absolute(b).lexically_normal().c_str()) == 0;
}
std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\""; size_t slashes = 0;
    for (wchar_t character : value) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += character; slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
ComPtr<IShellLinkW> loadLink(const fs::path& path) {
    ComPtr<IShellLinkW> link; check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)), "Create startup shortcut");
    ComPtr<IPersistFile> file; check(link.As(&file), "Startup IPersistFile"); check(file->Load(path.c_str(), STGM_READ), "Read startup shortcut");
    return link;
}
}
LoginStartup::LoginStartup(const StateStore& store, fs::path directory) : root_(store.root), directory_(std::move(directory)) {
    if (directory_.empty()) {
        PWSTR raw = nullptr; check(SHGetKnownFolderPath(FOLDERID_Startup, KF_FLAG_DONT_VERIFY, nullptr, &raw), "Current user Startup folder");
        directory_ = raw; CoTaskMemFree(raw);
    }
}
std::wstring LoginStartup::arguments() const { return L"--startup --state-dir " + quoteArgument(root_.wstring()); }
bool LoginStartup::ownsLink() const {
    if (!fs::exists(linkPath())) return false;
    try {
        auto link = loadLink(linkPath()); wchar_t target[32768]{}, description[256]{};
        check(link->GetPath(target, 32768, nullptr, SLGP_RAWPATH), "Read startup target");
        check(link->GetDescription(description, 256), "Read startup description");
        return *target && samePath(target, executable()) && std::wstring(description) == startupDescription;
    } catch (...) { return false; }
}
bool LoginStartup::enabled() const {
    if (!ownsLink() || !fs::is_regular_file(executable())) return false;
    try {
        wchar_t args[32768]{}; check(loadLink(linkPath())->GetArguments(args, 32768), "Read startup arguments");
        return std::wstring(args) == arguments();
    } catch (...) { return false; }
}
void LoginStartup::setEnabled(bool enable, const fs::path& source) {
    if (fs::exists(linkPath()) && !ownsLink()) throw std::runtime_error("An unrelated QuietBoostNative.lnk already exists in the Startup folder; it was not changed.");
    if (!enable) {
        if (fs::exists(linkPath()) && !DeleteFileW(linkPath().c_str())) throw std::runtime_error("Cannot remove QuietBoost startup shortcut");
        return;
    }
    fs::create_directories(directory_); fs::create_directories(executable().parent_path());
    auto copied = executable(); copied += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    auto shortcut = linkPath(); shortcut += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    struct Clean { fs::path a, b; ~Clean() { std::error_code ignored; fs::remove(a, ignored); fs::remove(b, ignored); } } clean{copied, shortcut};
    // Keep the login target stable even if the downloaded/USB copy is later moved.
    if (!samePath(source, executable())) {
        if (!CopyFileW(source.c_str(), copied.c_str(), FALSE) || !MoveFileExW(copied.c_str(), executable().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot update the startup copy. Exit the running QuietBoost first, then enable startup from the new version.");
    } else if (!fs::is_regular_file(executable())) throw std::runtime_error("Startup executable is missing");
    ComPtr<IShellLinkW> link; check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)), "Create startup shortcut");
    check(link->SetPath(executable().c_str()), "Set startup target"); check(link->SetArguments(arguments().c_str()), "Set startup arguments");
    check(link->SetWorkingDirectory(executable().parent_path().c_str()), "Set startup directory");
    check(link->SetDescription(startupDescription), "Set startup description"); check(link->SetShowCmd(SW_SHOWNOACTIVATE), "Set startup display");
    ComPtr<IPersistFile> file; check(link.As(&file), "Startup IPersistFile"); check(file->Save(shortcut.c_str(), TRUE), "Save startup shortcut");
    if (!MoveFileExW(shortcut.c_str(), linkPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot commit startup shortcut");
    if (!enabled()) throw std::runtime_error("Startup registration could not be verified");
}
}
