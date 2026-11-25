// Synthetic-only unit tests for src/updater.cpp: no network, no process launch, no UI.
#include "../src/common.h"
#include "../src/updater.h"
#include <cstdio>

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HICON g_icon = nullptr;
void RunOnMain(std::function<void()> fn) { if (fn) fn(); }
void TrimMemory() {}
void Log(const wchar_t*, ...) {}
bool IsEnglish() { return true; }
std::wstring ExeDir() { return L"."; }
void MainWindowRefresh() {}
void PlayUiSound(Sound) {}
void RunCommand(int) {}

#include "../src/updater.cpp"

static int failures = 0;
static void Check(bool ok, const char* msg) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", msg);
    if (!ok) ++failures;
}

int wmain() {
    Check(updater::IsNewerVersion(L"1.3.1", L"1.3.0"), "patch bump is newer");
    Check(updater::IsNewerVersion(L"v1.4.0", L"1.3.9"), "minor bump with v prefix is newer");
    Check(updater::IsNewerVersion(L"2.0", L"1.9.9"), "major bump with 2 components is newer");
    Check(!updater::IsNewerVersion(L"1.3.0", L"1.3.0"), "equal versions are not newer");
    Check(!updater::IsNewerVersion(L"1.2.9", L"1.3.0"), "older version is not newer");
    Check(updater::IsNewerVersion(L"1.4.0-rc1", L"1.3.0"), "prerelease tag strips suffix");
    Check(!updater::IsNewerVersion(L"", L"1.3.0"), "empty remote version is rejected");
    Check(!updater::IsNewerVersion(L"abc", L"1.3.0"), "non-numeric version is rejected");

    const std::string jsonMulti = R"({
        "tag_name": "v1.4.2",
        "assets": [
            {"name": "Zipshot-1.4.2-Portable-win-x64.zip", "size": 1800000, "browser_download_url": "https://example.com/zipshot.zip"},
            {"name": "Zipshot-1.4.2-Setup-x64.exe", "size": 2400000, "browser_download_url": "https://example.com/setup.exe"},
            {"name": "zipshot.exe", "size": 1950000, "browser_download_url": "https://example.com/zipshot.exe"},
            {"name": "SHA256SUMS.txt", "size": 256, "browser_download_url": "https://example.com/SHA256SUMS.txt"}
        ]
    })";
    updater::ReleaseInfo info;
    Check(updater::ParseLatestReleaseJson(jsonMulti, info) &&
          info.version == L"1.4.2" &&
          info.kind == updater::AssetKind::StandaloneExe &&
          info.assetName == L"zipshot.exe" &&
          info.assetSize == 1950000 &&
          info.sha256Url == L"https://example.com/SHA256SUMS.txt",
          "standalone zipshot.exe is preferred over installer and zip, with SHA256SUMS captured");

    const std::string jsonSetup = R"({
        "tag_name": "v1.5.0",
        "assets": [
            {"name": "Zipshot-1.5.0-Portable-win-x64.zip", "size": 1800000, "browser_download_url": "https://example.com/zipshot.zip"},
            {"name": "Zipshot-1.5.0-Setup-x64.exe", "size": 2400000, "browser_download_url": "https://example.com/setup.exe"}
        ]
    })";
    Check(updater::ParseLatestReleaseJson(jsonSetup, info) &&
          info.kind == updater::AssetKind::InstallerExe &&
          info.assetName == L"Zipshot-1.5.0-Setup-x64.exe",
          "installer exe is preferred over portable zip when standalone exe is absent");

    const std::string jsonZip = R"({
        "tag_name": "v1.6.0",
        "assets": [
            {"name": "Zipshot-1.6.0-Portable-win-x64.zip", "size": 1800000, "browser_download_url": "https://example.com/zipshot.zip"}
        ]
    })";
    Check(updater::ParseLatestReleaseJson(jsonZip, info) &&
          info.kind == updater::AssetKind::PortableZip &&
          info.assetName == L"Zipshot-1.6.0-Portable-win-x64.zip",
          "portable zip is selected as fallback");

    const std::string sums =
        "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef  Zipshot-1.4.2-Portable-win-x64.zip\r\n"
        "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210 *zipshot.exe\n";
    std::string hash;
    Check(updater::ParseSha256ForAsset(sums, L"ZIPSHOT.EXE", hash) &&
          hash == "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",
          "SHA256SUMS parser matches filename case-insensitively with binary asterisk");
    Check(updater::ParseSha256ForAsset(sums, L"Zipshot-1.4.2-Portable-win-x64.zip", hash) &&
          hash == "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          "SHA256SUMS parser normalizes uppercase hex to lowercase");
    Check(!updater::ParseSha256ForAsset(sums, L"missing.exe", hash),
          "SHA256SUMS parser returns false for absent asset");

    Check(updater::FormatBytes(512) == L"512 B", "FormatBytes formats bytes");
    Check(updater::FormatBytes(2048) == L"2 KB", "FormatBytes formats kilobytes");
    Check(updater::FormatBytes(2 * 1024 * 1024) == L"2.0 MB", "FormatBytes formats megabytes");

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
