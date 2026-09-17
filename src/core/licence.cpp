#include "core/licence.h"

#include <Windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "core/guard.h"
#include "core/log.h"

// bridger.dll imports only GDI32, KERNEL32 and USER32 (see bridger.cpp for why), so wintrust
// and crypt32 are loaded here by hand and every function is resolved by name.

namespace bridger::licence {
namespace {

constexpr std::uint32_t kAppId = 1190460;
// The interface version ds.exe itself asks for, so the client is guaranteed to provide it.
constexpr const char* kAppsInterface = "STEAMAPPS_INTERFACE_VERSION008";

template <typename T>
T resolve(HMODULE module, const char* name) {
    return module == nullptr ? nullptr : reinterpret_cast<T>(GetProcAddress(module, name));
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

// ds.exe loads steam_api64.dll at runtime rather than importing it. It is there long before
// the engine registers its types, so this loop is only insurance.
HMODULE wait_for_steam_api() {
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (const HMODULE module = GetModuleHandleW(L"steam_api64.dll"); module != nullptr) {
            return module;
        }
        Sleep(250);
    }
    return nullptr;
}

std::wstring path_of(HMODULE module) {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, buffer, MAX_PATH);
    return std::wstring(buffer, length);
}

// Two questions about the file: does Windows trust its Authenticode signature, and is the
// signer Valve. A Steam emulator dropped in as steam_api64.dll fails the first; one signed by
// somebody else fails the second.
bool signed_by_valve(const std::wstring& path, std::string& reason) {
    using WinVerifyTrustFn = LONG(WINAPI*)(HWND, GUID*, LPVOID);
    using CryptQueryObjectFn = BOOL(WINAPI*)(DWORD, const void*, DWORD, DWORD, DWORD, DWORD*,
                                             DWORD*, DWORD*, HCERTSTORE*, HCRYPTMSG*,
                                             const void**);
    using CryptMsgGetParamFn = BOOL(WINAPI*)(HCRYPTMSG, DWORD, DWORD, void*, DWORD*);
    using CryptMsgCloseFn = BOOL(WINAPI*)(HCRYPTMSG);
    using CertFindCertificateInStoreFn = PCCERT_CONTEXT(WINAPI*)(HCERTSTORE, DWORD, DWORD, DWORD,
                                                                 const void*, PCCERT_CONTEXT);
    using CertGetNameStringWFn = DWORD(WINAPI*)(PCCERT_CONTEXT, DWORD, DWORD, void*, LPWSTR,
                                                DWORD);
    using CertFreeCertificateContextFn = BOOL(WINAPI*)(PCCERT_CONTEXT);
    using CertCloseStoreFn = BOOL(WINAPI*)(HCERTSTORE, DWORD);

    const HMODULE wintrust = LoadLibraryW(L"wintrust.dll");
    const HMODULE crypt32 = LoadLibraryW(L"crypt32.dll");
    const auto verify_trust = resolve<WinVerifyTrustFn>(wintrust, "WinVerifyTrust");
    const auto query_object = resolve<CryptQueryObjectFn>(crypt32, "CryptQueryObject");
    const auto get_param = resolve<CryptMsgGetParamFn>(crypt32, "CryptMsgGetParam");
    const auto close_message = resolve<CryptMsgCloseFn>(crypt32, "CryptMsgClose");
    const auto find_certificate =
        resolve<CertFindCertificateInStoreFn>(crypt32, "CertFindCertificateInStore");
    const auto get_name = resolve<CertGetNameStringWFn>(crypt32, "CertGetNameStringW");
    const auto free_certificate =
        resolve<CertFreeCertificateContextFn>(crypt32, "CertFreeCertificateContext");
    const auto close_store = resolve<CertCloseStoreFn>(crypt32, "CertCloseStore");
    if (!verify_trust || !query_object || !get_param || !close_message || !find_certificate ||
        !get_name || !free_certificate || !close_store) {
        reason = "wintrust.dll or crypt32.dll could not be loaded";
        return false;
    }

    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    // Never go to the network for this: an offline machine must get the same answer.
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = verify_trust(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    verify_trust(nullptr, &action, &data);
    if (status != ERROR_SUCCESS) {
        reason = std::format("steam_api64.dll carries no signature Windows trusts (status {:#x})",
                             static_cast<std::uint32_t>(status));
        return false;
    }

    HCERTSTORE store = nullptr;
    HCRYPTMSG message = nullptr;
    DWORD encoding = 0;
    DWORD content = 0;
    DWORD format = 0;
    if (!query_object(CERT_QUERY_OBJECT_FILE, path.c_str(),
                      CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY,
                      0, &encoding, &content, &format, &store, &message, nullptr)) {
        reason = "the signature on steam_api64.dll could not be read";
        return false;
    }

    std::wstring signer;
    DWORD size = 0;
    if (get_param(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) && size > 0) {
        std::vector<std::uint8_t> buffer(size);
        if (get_param(message, CMSG_SIGNER_INFO_PARAM, 0, buffer.data(), &size)) {
            const auto* info = reinterpret_cast<const CMSG_SIGNER_INFO*>(buffer.data());
            CERT_INFO wanted{};
            wanted.Issuer = info->Issuer;
            wanted.SerialNumber = info->SerialNumber;
            if (const PCCERT_CONTEXT certificate =
                    find_certificate(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                     CERT_FIND_SUBJECT_CERT, &wanted, nullptr)) {
                wchar_t name[256]{};
                get_name(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
                signer = name;
                free_certificate(certificate);
            }
        }
    }
    close_message(message);
    close_store(store, 0);

    if (signer != L"Valve") {
        reason = std::format("steam_api64.dll is signed by \"{}\"; Valve's own library is required",
                             narrow(signer));
        return false;
    }
    return true;
}

// The Steam flat API, by name. One calling convention on x64, so plain function pointers do.
struct SteamApi {
    bool (*is_steam_running)() = nullptr;
    std::int32_t (*get_user)() = nullptr;
    void* (*find_interface)(std::int32_t, const char*) = nullptr;
    bool (*is_subscribed)(void*) = nullptr;
    bool (*is_subscribed_app)(void*, std::uint32_t) = nullptr;
    bool (*is_subscribed_from_family_sharing)(void*) = nullptr;

    bool load(HMODULE module) {
        is_steam_running = resolve<decltype(is_steam_running)>(module, "SteamAPI_IsSteamRunning");
        get_user = resolve<decltype(get_user)>(module, "SteamAPI_GetHSteamUser");
        find_interface =
            resolve<decltype(find_interface)>(module, "SteamInternal_FindOrCreateUserInterface");
        is_subscribed = resolve<decltype(is_subscribed)>(module, "SteamAPI_ISteamApps_BIsSubscribed");
        is_subscribed_app =
            resolve<decltype(is_subscribed_app)>(module, "SteamAPI_ISteamApps_BIsSubscribedApp");
        is_subscribed_from_family_sharing = resolve<decltype(is_subscribed_from_family_sharing)>(
            module, "SteamAPI_ISteamApps_BIsSubscribedFromFamilySharing");
        return is_steam_running && get_user && find_interface && is_subscribed &&
               is_subscribed_app;
    }
};

void check(void* raw) {
    auto& verdict = *static_cast<Verdict*>(raw);

    const HMODULE module = wait_for_steam_api();
    if (module == nullptr) {
        verdict.reason = "steam_api64.dll is not loaded; this is not the Steam build";
        return;
    }
    const auto path = path_of(module);
    if (!signed_by_valve(path, verdict.reason)) {
        return;
    }

    SteamApi steam;
    if (!steam.load(module)) {
        verdict.reason = "steam_api64.dll lacks the Steamworks exports the game's own build has";
        return;
    }
    if (!steam.is_steam_running()) {
        verdict.reason = "the Steam client is not running";
        return;
    }
    const auto user = steam.get_user();
    if (user == 0) {
        verdict.reason = "the game has not initialised the Steam API";
        return;
    }
    void* apps = steam.find_interface(user, kAppsInterface);
    if (apps == nullptr) {
        verdict.reason = std::format("the Steam client did not provide {}", kAppsInterface);
        return;
    }
    // BIsSubscribed answers for the app this process runs as; BIsSubscribedApp for Death
    // Stranding by id. Both must agree, so a steam_appid.txt pointing at some other owned
    // title does not pass.
    if (!steam.is_subscribed(apps) || !steam.is_subscribed_app(apps, kAppId)) {
        verdict.reason =
            std::format("the signed-in Steam account holds no licence for app {}", kAppId);
        return;
    }

    verdict.ok = true;
    const bool shared = steam.is_subscribed_from_family_sharing != nullptr &&
                        steam.is_subscribed_from_family_sharing(apps);
    verdict.detail = shared ? "Steam licence confirmed, through Family Sharing"
                            : "Steam licence confirmed";
}

}

Verdict verify() {
    Verdict verdict;
    const auto fault = guarded_call(check, &verdict);
    if (fault != 0) {
        verdict.ok = false;
        verdict.reason = std::format("faulted with code {:#x} while asking the Steam API", fault);
    }
    return verdict;
}

}
