// Actual app.cpp completion flow, with only OS/UI/codec boundaries replaced.
// No window, input, clipboard, screen capture, sound or user file is accessed.
#include "../src/common.h"
#include "../src/shapes_ext.h"
#include "../src/sharex_import.h"
#include "../src/capture_ext.h"
#include "../src/encoded_png.h"
#include <ole2.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

class PreparedPng { public: std::shared_ptr<const Image> image; };
namespace test {
std::mutex mutex;
std::condition_variable cv;
std::deque<std::function<void()>*> callbacks;
std::map<std::wstring, bool> settings;
std::atomic_uint sequence{100}, preparing{0}, prepared{0}, plainSaves{0}, cachedSaves{0};
std::atomic_uint copies{0}, notifications{0}, errors{0}, toasts{0}, editors{0}, quitCalls{0};
std::atomic_uint clipboard{0}, holdImage{0};
std::atomic_bool release{false}, failPrepare{false}, failSave{false};
DWORD mainThread = 0;
std::atomic_uint prepareOnMain{0};
int assertions = 0, failures = 0;
unsigned Id(const Image& image) { return image.px.empty() ? 0 : image.px[0] & 255; }
void Check(bool good, const char* label) { ++assertions; if (!good) ++failures; std::printf("%s %s\n", good ? "PASS" : "FAIL", label); }
bool Wait(const std::function<bool()>& ready) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!ready() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return ready();
}
void Drain() {
    for (;;) {
        std::function<void()>* callback = nullptr;
        { std::lock_guard<std::mutex> lock(mutex); if (callbacks.empty()) break; callback = callbacks.front(); callbacks.pop_front(); }
        (*callback)(); delete callback;
    }
}
size_t Pending() { std::lock_guard<std::mutex> lock(mutex); return callbacks.size(); }
Image Pixels(unsigned id, bool alpha = true) { Image image; image.w = image.h = 2; image.alpha = alpha; image.px.assign(4, 0x80112200u | id); return image; }
}

bool TestSetting(const wchar_t* key, bool value) { const auto found = test::settings.find(key); return found == test::settings.end() ? value : found->second; }
DWORD WINAPI TestSequence() { return test::sequence.load(); }
BOOL WINAPI TestPost(HWND, UINT, WPARAM, LPARAM value) { std::lock_guard<std::mutex> lock(test::mutex); test::callbacks.push_back(reinterpret_cast<std::function<void()>*>(value)); return TRUE; }
void TestCopy(const Image& image) { ++test::copies; test::clipboard = test::Id(image); ++test::sequence; }
PreparedPngPtr TestPrepare(std::shared_ptr<const Image> image) {
    ++test::preparing;
    if (GetCurrentThreadId() == test::mainThread) ++test::prepareOnMain;
    if (test::Id(*image) == test::holdImage) { std::unique_lock<std::mutex> lock(test::mutex); test::cv.wait(lock, [] { return test::release.load(); }); }
    ++test::prepared;
    if (test::failPrepare) return {};
    auto packet = std::make_shared<PreparedPng>(); packet->image = std::move(image); return packet;
}
void TestCopyPrepared(const PreparedPngPtr& packet, std::function<bool()> allow) {
    if (!packet || (allow && !allow())) return;
    TestCopy(*packet->image);
}
bool TestSavePrepared(const PreparedPngPtr&, std::wstring&) { ++test::cachedSaves; return !test::failSave; }
bool TestSave(const Image&, std::wstring&) { ++test::plainSaves; return !test::failSave; }
void TestSound(Sound) {}
void TestEditor(ImagePtr, const std::wstring&) { ++test::editors; }
void TestToast(ImagePtr, const std::wstring&, const std::wstring&) { ++test::toasts; }
void TestNotify(const std::wstring&) { ++test::notifications; }
int WINAPI TestError(HWND, LPCWSTR, LPCWSTR, UINT) { ++test::errors; return IDOK; }
void WINAPI TestQuit(int) { ++test::quitCalls; }

// These unrelated app entry points must never run in a completion-flow test.
[[noreturn]] void Forbidden() { std::fprintf(stderr, "Unexpected application boundary\n"); std::abort(); }
bool IsEnglish() { return false; }
void SetEnglish(bool) { Forbidden(); }
bool AutostartEnabled() { Forbidden(); }
void SetAutostart(bool) { Forbidden(); }
void ShowMainWindow() { Forbidden(); }
void MainWindowRefresh() { Forbidden(); }
bool MainWindowWriteReady(const std::wstring&) { Forbidden(); }
bool MainWindowMediaBusy() { Forbidden(); }
bool ImportShareXSettings(std::wstring&) { Forbidden(); }
RECT VirtualScreen() { Forbidden(); }
Image CaptureRect(RECT, bool) { Forbidden(); }
HBITMAP CreateDib(int, int, void**) { Forbidden(); }
void LoadSettings() { Forbidden(); }
void SetSettingBool(const wchar_t*, bool) { Forbidden(); }
std::wstring SaveDirRoot() { Forbidden(); }
std::wstring NewOutputPath(const wchar_t*) { Forbidden(); }
bool LoadImageFile(const std::wstring&, Image&) { Forbidden(); }
void CopyFileToClipboard(const std::wstring&) { Forbidden(); }
std::wstring FindFFmpeg() { Forbidden(); }
bool RegionCapture(ImagePtr, RECT, Image&, RECT&) { Forbidden(); }
bool SelectRegion(const Image&, RECT, RECT&, bool) { Forbidden(); }
bool DumpOverlayTest(const Image&, RECT, POINT, RECT, const std::wstring&) { Forbidden(); }
bool ColorPickerTest(const std::wstring&) { Forbidden(); }
HICON LoadAppIcon(int) { Forbidden(); }
bool EditorTest(const std::wstring&, const std::wstring&) { Forbidden(); }
bool EditorDemoTest(const std::wstring&, const std::wstring&) { Forbidden(); }
bool EditorWindowTest(const std::wstring&, const std::wstring&) { Forbidden(); }
bool GifExportTest(const std::wstring&, const std::wstring&) { Forbidden(); }
bool RegionWindowTest(const std::wstring&) { Forbidden(); }
bool ImageOpsTest(const std::wstring&, const std::wstring&) { Forbidden(); }
bool ShowColorPicker(HWND, POINT, float[4]) { Forbidden(); }
bool RecorderActive() { Forbidden(); }
bool RecorderBusy() { Forbidden(); }
void RecorderStart(RECT, bool) { Forbidden(); }
void RecorderStop() { Forbidden(); }
bool ParseHotkeyText(const std::wstring&, UINT&, UINT&) { Forbidden(); }
std::wstring HotkeyIniText(UINT, UINT) { Forbidden(); }
std::wstring HotkeyDisplayText(UINT, UINT) { Forbidden(); }
std::wstring ZipshotIni() { Forbidden(); }
std::wstring IniString(const wchar_t*, const wchar_t*, const wchar_t*) { Forbidden(); }
void EnsureUnicodeIni() { Forbidden(); }
void RecorderHotkey() { Forbidden(); }
void RecorderPauseResume() { Forbidden(); }
void RecorderAbort() { Forbidden(); }
bool RecorderPaused() { Forbidden(); }
std::vector<std::wstring> ListAudioDevices() { Forbidden(); }
namespace shapes_ext { bool PickSticker(HWND, POINT, float, std::wstring&, int&) { Forbidden(); } }

#define GetSettingBool TestSetting
#define GetClipboardSequenceNumber TestSequence
#define PostMessageW TestPost
#define CopyImageToClipboard TestCopy
#define PreparePngWic TestPrepare
#define PreparePng TestPrepare
#define CopyPreparedImageToClipboard TestCopyPrepared
#define SavePreparedImageAuto TestSavePrepared
#define SaveImageAuto TestSave
#define PlayUiSound TestSound
#define OpenEditor TestEditor
#define ShowToast TestToast
#define MainWindowNotifyCapture TestNotify
#define MessageBoxW TestError
#define PostQuitMessage TestQuit
#define wWinMain TestUnusedWinMain
#include "../src/app.cpp"
#undef wWinMain

namespace test {
void Reset(bool save = true, bool editor = false) {
    Check(g_stillJobs.load() == 0 && Pending() == 0, "previous completion fully drained");
    settings = {{L"AfterCopy", true}, {L"AfterSave", save}, {L"AfterToast", true}, {L"AfterEditor", editor}};
    sequence = 100; preparing = prepared = plainSaves = cachedSaves = copies = notifications = errors = toasts = editors = quitCalls = 0;
    clipboard = 90; holdImage = 0; release = false; failPrepare = failSave = false; prepareOnMain = 0;
    g_testMode = false; g_lastPath.clear();
}
bool Complete(unsigned expectedCallbacks = 2) {
    const bool ready = Wait([&] { return Pending() >= expectedCallbacks; }); Drain(); return ready && g_stillJobs.load() == 0;
}
}

int main() {
    test::mainThread = GetCurrentThreadId(); g_main = reinterpret_cast<HWND>(1);
    test::Reset(); test::holdImage = 1;
    FinishStill(test::Pixels(1), L"synthetic-alpha.png", L"Synthetic");
    test::Check(test::Wait([] { return test::preparing.load() == 1; }) && test::copies == 0 && g_stillJobs == 1,
                "alpha capture returns with encoding on a worker and pending media work");
    test::Check(test::prepareOnMain == 0, "expensive alpha PNG encoding does not run on the UI thread");
    test::release = true; test::cv.notify_all();
    test::Check(test::Complete(), "clipboard and save callbacks complete through main-thread queue");
    test::Check(test::prepared == 1 && test::cachedSaves == 1 && test::plainSaves == 0, "one immutable PNG serves clipboard and disk without a second encode");
    test::Check(test::clipboard == 1 && test::copies == 1 && test::notifications == 1 && test::errors == 0,
                "current capture keeps expected clipboard and saved-file notification");

    test::Reset(); test::holdImage = 1;
    FinishStill(test::Pixels(1), L"older.png", L"Older"); test::Wait([] { return test::preparing == 1; });
    test::settings[L"AfterSave"] = false;
    FinishStill(test::Pixels(2, false), L"", L"Latest opaque");
    test::release = true; test::cv.notify_all(); test::Complete();
    test::Check(test::clipboard == 2 && test::copies == 1, "slower older alpha capture cannot overwrite a newer opaque capture");
    test::Check(test::cachedSaves == 1 && test::notifications == 1, "superseded clipboard capture is still saved normally");

    test::Reset(); test::holdImage = 3;
    FinishStill(test::Pixels(3), L"manual-copy-race.png", L"Synthetic"); test::Wait([] { return test::preparing == 1; });
    test::clipboard = 77; ++test::sequence;
    test::release = true; test::cv.notify_all(); test::Complete();
    test::Check(test::clipboard == 77 && test::copies == 0, "manual user copy while encoding is never replaced by delayed screenshot data");

    test::Reset(false, true);
    FinishStill(test::Pixels(4), L"", L"Copy only");
    test::Check(test::Complete() && test::cachedSaves == 0 && test::plainSaves == 0 && test::clipboard == 4 && test::editors == 1,
                "copy-only alpha path prepares once and opens editor without saving a file");

    test::Reset(false);
    FinishStill(test::Pixels(5, false), L"", L"Opaque copy");
    test::Check(test::copies == 1 && test::preparing == 0 && g_stillJobs == 0 && test::toasts == 1,
                "ordinary opaque copy-only path remains immediate");

    test::Reset(); test::failPrepare = true;
    FinishStill(test::Pixels(6), L"failed.png", L"Failed"); test::Complete(1);
    test::Check(test::errors == 1 && test::copies == 0 && test::plainSaves == 0 && test::cachedSaves == 0 && test::notifications == 0,
                "failed PNG preparation does not publish mismatched clipboard or saved-file results");

    test::Reset(); test::failSave = true;
    FinishStill(test::Pixels(7), L"write-failed.png", L"Failed write"); test::Complete();
    test::Check(test::clipboard == 7 && test::errors == 1 && test::notifications == 0 && g_stillJobs == 0,
                "file write failure still retains the successfully copied capture and releases pending work");
    std::printf("%d assertions, %d failures\n", test::assertions, test::failures);
    return test::failures ? 1 : 0;
}
