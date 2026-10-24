#include "platform.hpp"
#include "driver_resources.hpp"
#include <bcrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <shellapi.h>
#include <iomanip>

namespace qb {
static std::wstring hashBytes(const BYTE* data, DWORD size) {
    BCRYPT_ALG_HANDLE algorithm = nullptr; check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "SHA256 provider");
    struct CloseAlgorithm { BCRYPT_ALG_HANDLE value; ~CloseAlgorithm() { BCryptCloseAlgorithmProvider(value, 0); } } cleanup{algorithm};
    DWORD objectSize = 0, returned = 0;
    check(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &returned, 0), "SHA256 property");
    std::vector<BYTE> object(objectSize); BCRYPT_HASH_HANDLE hash = nullptr;
    check(BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0), "SHA256 create");
    struct CloseHash { BCRYPT_HASH_HANDLE value; ~CloseHash() { BCryptDestroyHash(value); } } hashCleanup{hash};
    check(BCryptHashData(hash, const_cast<PUCHAR>(data), size, 0), "SHA256 data"); BYTE digest[32]{};
    check(BCryptFinishHash(hash, digest, sizeof(digest), 0), "SHA256 finish");
    std::wostringstream text; text << std::uppercase << std::hex << std::setfill(L'0'); for (BYTE byte : digest) text << std::setw(2) << static_cast<unsigned>(byte); return text.str();
}
fs::path verifyDriver(StateStore& store) {
    fs::path directory = store.root / L"driver" / L"45"; fs::create_directories(directory);
    HMODULE module = GetModuleHandleW(nullptr);
    for (const auto& resource : driverResources) {
        HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(resource.id), RT_RCDATA);
        if (!found) throw std::runtime_error("Embedded driver resource missing");
        HGLOBAL loaded = LoadResource(module, found); auto data = static_cast<const BYTE*>(LockResource(loaded)); DWORD size = SizeofResource(module, found);
        if (!data || hashBytes(data, size) != resource.sha256) throw std::runtime_error("Embedded driver integrity check failed");
        Handle file(CreateFileW((directory / resource.name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
        DWORD written = 0; if (!file || !WriteFile(file.get(), data, size, &written, nullptr) || written != size || !FlushFileBuffers(file.get())) throw std::runtime_error("Cannot extract embedded driver");
    }
    SYSTEM_INFO info{}; GetNativeSystemInfo(&info);
    fs::path installer = directory / (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? L"VBCABLE_Setup.exe" : L"VBCABLE_Setup_x64.exe");
    WINTRUST_FILE_INFO fileInfo{sizeof(fileInfo)}; fileInfo.pcwszFilePath = installer.c_str();
    WINTRUST_DATA trust{sizeof(trust)}; trust.dwUIChoice = WTD_UI_NONE; trust.fdwRevocationChecks = WTD_REVOKE_NONE; trust.dwUnionChoice = WTD_CHOICE_FILE; trust.pFile = &fileInfo;
    trust.dwStateAction = WTD_STATEACTION_VERIFY; trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &action, &trust); trust.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(nullptr, &action, &trust);
    if (status != ERROR_SUCCESS) throw std::runtime_error("VB-CABLE installer signature verification failed");
    return installer;
}
void installDriver(HWND parent, StateStore& store) {
    fs::path installer = verifyDriver(store), directory = installer.parent_path();
    SHELLEXECUTEINFOW launch{sizeof(launch)}; launch.hwnd = parent; launch.lpVerb = L"runas"; launch.lpFile = installer.c_str(); launch.lpDirectory = directory.c_str(); launch.nShow = SW_SHOWNORMAL; launch.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&launch)) {
        if (GetLastError() == ERROR_CANCELLED) return;
        throw std::runtime_error("Cannot open the VB-CABLE installer");
    }
    if (launch.hProcess) CloseHandle(launch.hProcess);
}
}
