#include "common.h"
#include <shlobj.h>

// UI language: zipshot.ini [main] Language=ru|en. Read once, switched at runtime from the menus.

namespace {
int g_lang = -1;  // -1 unknown, 0 ru, 1 en
}

bool IsEnglish() {
    if (g_lang < 0) {
        wchar_t buf[8];
        GetPrivateProfileStringW(L"main", L"Language", L"ru", buf, 8, (ExeDir() + L"\\zipshot.ini").c_str());
        g_lang = _wcsicmp(buf, L"en") == 0 ? 1 : 0;
    }
    return g_lang == 1;
}

namespace {
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"Zipshot";
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        // One autostart entry only: the old Startup-folder shortcut would start a second copy attempt.
        wchar_t* startup = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &startup))) {
            DeleteFileW((std::wstring(startup) + L"\\Zipshot.lnk").c_str());
            CoTaskMemFree(startup);
        }
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

void SetEnglish(bool en) {
    g_lang = en ? 1 : 0;
    WritePrivateProfileStringW(L"main", L"Language", en ? L"en" : L"ru", (ExeDir() + L"\\zipshot.ini").c_str());
}
