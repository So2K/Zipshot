#pragma once
#include <cstdint>
#include <string>

namespace updater {

inline constexpr const wchar_t* kAppVersion = L"1.3.0";
inline constexpr const wchar_t* kRepo = L"So2K/Zipshot";

enum class AssetKind { None, StandaloneExe, InstallerExe, PortableZip };

struct ReleaseInfo {
    std::wstring version;
    std::wstring assetName;
    std::wstring downloadUrl;
    std::wstring sha256Url;
    uint64_t assetSize = 0;
    AssetKind kind = AssetKind::None;
};

// Pure helpers (unit-tested without network or UI).
bool IsNewerVersion(const std::wstring& remote, const std::wstring& local);
bool ParseLatestReleaseJson(const std::string& jsonUtf8, ReleaseInfo& out);
bool ParseSha256ForAsset(const std::string& sumsUtf8, const std::wstring& assetName, std::string& outHexLower);
std::wstring FormatBytes(uint64_t bytes);

// Lifecycle & state.
void CleanupOldExe();
void Check(bool manual = false);
void DownloadAndInstall();

bool Checking();
bool Downloading();
bool HasUpdate();
double Progress();
std::wstring AvailableVersion();
std::wstring StatusText();

}  // namespace updater
