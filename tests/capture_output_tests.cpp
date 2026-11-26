// Synthetic-only regression tests: no desktop capture, input, clipboard or installed settings.
#include "../src/common.h"
#include <cstdio>
#include <thread>
#include <set>
#include <filesystem>

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HICON g_icon = nullptr;
static std::wstring testDir;
std::wstring ExeDir() { return testDir; }
std::wstring ZipshotIni() { return testDir + L"\\zipshot.ini"; }
void Log(const wchar_t*, ...) {}
static void UnexpectedUiCall() { void (*volatile fail)() = &std::abort; fail(); }
void RunOnMain(std::function<void()>) { UnexpectedUiCall(); }
void TrimMemory() { UnexpectedUiCall(); }
bool IsEnglish() { return true; }
void MainWindowNotifyCapture(const std::wstring&) { UnexpectedUiCall(); }
void ShowToast(ImagePtr, const std::wstring&, const std::wstring&) { UnexpectedUiCall(); }
int IniInt(const wchar_t*, const wchar_t*, int value) { return value; }
void EnsureUnicodeIni() {
    const wchar_t bom = 0xFEFF;
    HANDLE f = CreateFileW((testDir + L"\\zipshot.ini").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written;
    WriteFile(f, &bom, sizeof(bom), &written, nullptr);
    CloseHandle(f);
}
std::wstring IniString(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t value[2048];
    GetPrivateProfileStringW(section, key, def, value, (DWORD)std::size(value), (testDir + L"\\zipshot.ini").c_str());
    return value;
}

#include "../src/capture.cpp"
#include "../src/output.cpp"
#include "../src/recorder.cpp"

// This suite calls only recording-rectangle normalization. Any video completion
// work would violate its no-process/no-capture contract and must fail closed.
namespace cursor_video {
struct PreparedSource::State {};
PreparedSource::PreparedSource() = default;
PreparedSource::~PreparedSource() = default;
Result SaveTrackPrepared(const std::wstring&, int, int, double, double,
                         const std::vector<cursor_follow::Sample>&, PreparedSource&) {
    UnexpectedUiCall(); return {};
}
Result RenderPrepared(const std::wstring&, const PreparedSource&, const std::wstring&, bool) {
    UnexpectedUiCall(); return {};
}
}
namespace wasapi_audio {
struct Recorder::Impl {};
Recorder::Recorder() = default;
Recorder::~Recorder() = default;
Mode ParseMode(const std::wstring&) { return Mode::None; }
const wchar_t* ModeToSetting(Mode) { return L""; }
bool IsWasapiSetting(const std::wstring&) { return false; }
bool Recorder::Start(const std::wstring&, Mode, bool, uint32_t) { return false; }
void Recorder::SetMuted(bool) {}
bool Recorder::Muted() const { return false; }
bool Recorder::Active() const { return false; }
void Recorder::Stop() {}
}

static int failures = 0;
static void Check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    testDir = std::filesystem::absolute(argv[1]).wstring();
    std::filesystem::create_directories(testDir);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Tests must use a new per-run directory, never an installed zipshot.ini.
    if (GetFileAttributesW((testDir + L"\\zipshot.ini").c_str()) != INVALID_FILE_ATTRIBUTES) return 2;
    LoadSettings();
    Check(SaveDirRoot().find(L"\\Zipshot") != std::wstring::npos &&
          SaveDirRoot() != L"D:\\Programs\\ShareX\\ShareX\\Screenshots", "first run defaults to this user's Pictures/Zipshot");
    Check(SoundEnabled(), "sounds are enabled by default on first run");
    SetSoundEnabled(false);
    Check(!SoundEnabled() && !GetSettingBool(L"Sound", true), "disabling sound updates live state and persists to zipshot.ini");
    LoadSettings();
    Check(!SoundEnabled(), "disabled sound setting survives reload");
    SetSoundEnabled(true);
    Check(SoundEnabled() && GetSettingBool(L"Sound", false), "re-enabling sound updates live state and persists");

    Image transparent{2, 2, true, {0x00FFFFFFu, 0x80FF0000u, 0xFFFFFFFFu, 0xFF123456u}};
    auto crop = transparent.crop({0, 0, 2, 1});
    Check(crop.alpha && crop.px == std::vector<uint32_t>({0x00FFFFFFu, 0x80FF0000u}), "crop preserves alpha metadata and pixels");
    Check(transparent.crop({5, 5, 9, 9}).empty(), "crop outside image returns empty");
    void* bits = (void*)1;
    Check(!CreateDib(0, 3, &bits) && bits == nullptr, "invalid DIB size fails with a null pixel pointer");
    RECT monitor{-1920, 0, 0, 1080};
    RECT tiny{-3, 1077, -1, 1079};
    Check(NormalizeRecordingRect(tiny, monitor) && tiny.left == -16 && tiny.top == 1064 && tiny.right == 0 && tiny.bottom == 1080,
          "tiny recording selection expands inside negative-coordinate monitor edges");
    RECT outside{1, 1, 80, 80};
    Check(!NormalizeRecordingRect(outside, monitor), "recording selection outside the monitor is rejected");
    RECT odd{-99, 3, -2, 100};
    Check(NormalizeRecordingRect(odd, monitor) && odd.right - odd.left == 96 && odd.bottom - odd.top == 96,
          "recording dimensions become even without crossing monitor bounds");

    std::wstring alphaPath = testDir + L"\\alpha.png";
    Check(EncodeImage(crop, alphaPath, false), "transparent crop encodes to PNG");
    Image decoded;
    Check(LoadImageFile(alphaPath, decoded) && decoded.w == 2 && decoded.h == 1 && decoded.alpha && decoded.px == crop.px,
          "transparent PNG roundtrip preserves exact straight-alpha pixels and metadata");
    Image malformed{3, 2, false, {0xFFFFFFFFu}};
    Check(!EncodeImage(malformed, testDir + L"\\invalid.png", false), "malformed image buffer rejected");
    Image before = decoded;
    Check(!LoadImageFile(testDir + L"\\missing.png", decoded) && decoded.px == before.px && decoded.w == before.w,
          "failed load leaves caller's image unchanged");

    Image noise;
    noise.w = noise.h = 128;
    noise.px.resize(128 * 128);
    uint32_t rng = 12345;
    for (auto& p : noise.px) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; p = 0xFF000000u | (rng & 0xFFFFFFu); }
    g_jpegOverKB = 1;
    std::wstring first = testDir + L"\\same.png", second = first;
    Check(SaveImageAuto(noise, first) && first.ends_with(L".jpg"), "PNG threshold selects JPEG");
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    GetFileAttributesExW(first.c_str(), GetFileExInfoStandard, &attributes);
    Check(SaveImageAuto(noise, second) && second != first && second.ends_with(L".jpg"), "JPEG fallback avoids existing capture");
    WIN32_FILE_ATTRIBUTE_DATA after{};
    GetFileAttributesExW(first.c_str(), GetFileExInfoStandard, &after);
    Check(CompareFileTime(&attributes.ftLastWriteTime, &after.ftLastWriteTime) == 0, "existing JPEG remains untouched");

    g_jpegOverKB = 0;
    constexpr int count = 8;
    std::wstring paths[count];
    bool ok[count]{};
    std::thread workers[count];
    for (int i = 0; i < count; ++i) workers[i] = std::thread([&, i] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        paths[i] = testDir + L"\\concurrent.png";
        ok[i] = SaveImageAuto(noise, paths[i]);
        CoUninitialize();
    });
    for (auto& worker : workers) worker.join();
    std::set<std::wstring> distinct;
    bool allOk = true;
    for (int i = 0; i < count; ++i) { allOk &= ok[i]; distinct.insert(paths[i]); }
    Check(allOk && distinct.size() == count, "eight concurrent captures claim unique PNG files atomically");
    for (auto& path : distinct) {
        Image loaded;
        allOk &= LoadImageFile(path, loaded) && loaded.px == noise.px;
    }
    Check(allOk, "all concurrent PNGs decode with exact original pixels");
    CoUninitialize();
    return failures ? 1 : 0;
}
