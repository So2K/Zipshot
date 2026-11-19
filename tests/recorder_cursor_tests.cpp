// Synthetic recorder state regression: real recorder implementation, fake time,
// cursor, processes, preferences, hotkeys and output notifications. No capture.
#include "../src/common.h"
#include "../src/capture_ext.h"
#include "../src/sharex_import.h"
#include "../src/cursor_video.h"
#include <dxgi.h>
#include <wrl/client.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace test {
std::wstring root, clipboard, toast, savedSource, renderTarget;
std::vector<std::wstring> notified;
std::atomic<DWORD> now{10000};
std::atomic<int> cursorReads{0}, spawns{0}, saves{0}, renders{0}, forbidden{0};
std::atomic<int> preparedSaves{0}, preparedRenders{0}, loads{0};
std::atomic<bool> blockRender{false}, renderEntered{false};
bool followDefault = false, cursorVisible = true, cursorAvailable = true;
bool hotkeyAllowed = true, failSave = false, failRender = false;
POINT cursor{-750, 200};
RECT work{-1280, -720, 1280, 680};
int registered = 0, unregistered = 0, preferenceWrites = 0, outputNumber = 0;
std::mutex mutex;
std::deque<std::function<void()>> callbacks;
std::map<HANDLE, bool> processes;
cursor_video::Track savedTrack;
int checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
bool WriteOwned(const std::wstring& path, const std::string& bytes) {
    if (path.rfind(root + L"\\", 0) != 0) { ++forbidden; return false; }
    std::ofstream file(std::filesystem::path(path), std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}
DWORD WINAPI Time() { return now; }
BOOL WINAPI Cursor(PCURSORINFO info) {
    ++cursorReads;
    info->flags = cursorVisible ? CURSOR_SHOWING : 0;
    info->ptScreenPos = cursor;
    return cursorAvailable;
}
DWORD WINAPI TempPath(DWORD length, LPWSTR buffer) {
    const auto path = root + L"\\";
    if (path.size() >= length) return static_cast<DWORD>(path.size() + 1);
    wcscpy_s(buffer, length, path.c_str());
    return static_cast<DWORD>(path.size());
}
BOOL WINAPI Process(LPCWSTR, LPWSTR command, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
                    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION pi) {
    ++spawns;
    const std::wstring text(command);
    const size_t end = text.find_last_of(L'"');
    const size_t start = end == std::wstring::npos ? end : text.find_last_of(L'"', end - 1);
    if (start == std::wstring::npos || !WriteOwned(text.substr(start + 1, end - start - 1), std::string(4096, 'R')))
        return FALSE;
    pi->hProcess = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pi->hThread = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pi->dwProcessId = 123;
    pi->dwThreadId = 456;
    std::lock_guard lock(mutex);
    processes[pi->hProcess] = false;
    return TRUE;
}
DWORD WINAPI WaitProcess(HANDLE handle, DWORD timeout) {
    std::lock_guard lock(mutex);
    const auto it = processes.find(handle);
    if (it == processes.end()) { ++forbidden; return WAIT_FAILED; }
    if (timeout) it->second = true;
    return it->second ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}
BOOL WINAPI ExitCode(HANDLE handle, LPDWORD code) {
    std::lock_guard lock(mutex);
    if (!processes.count(handle)) { ++forbidden; return FALSE; }
    *code = 0; return TRUE;
}
BOOL WINAPI Terminate(HANDLE handle, UINT) {
    std::lock_guard lock(mutex);
    const auto it = processes.find(handle);
    if (it == processes.end()) { ++forbidden; return FALSE; }
    it->second = true; return TRUE;
}
BOOL WINAPI Close(HANDLE handle) {
    { std::lock_guard lock(mutex); processes.erase(handle); }
    return ::CloseHandle(handle);
}
BOOL WINAPI HiddenShow(HWND hwnd, int) { return ::ShowWindow(hwnd, SW_HIDE); }
BOOL WINAPI Layered(HWND, HDC, POINT*, SIZE*, HDC, POINT*, COLORREF, BLENDFUNCTION*, DWORD) { return TRUE; }
BOOL WINAPI Affinity(HWND, DWORD) { return TRUE; }
BOOL WINAPI Register(HWND, int, UINT, UINT) { if (hotkeyAllowed) ++registered; return hotkeyAllowed; }
BOOL WINAPI Unregister(HWND, int) { ++unregistered; return TRUE; }
UINT_PTR WINAPI Timer(HWND, UINT_PTR id, UINT, TIMERPROC) { return id; }
BOOL WINAPI Kill(HWND, UINT_PTR) { return TRUE; }
BOOL WINAPI Profile(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR) { ++preferenceWrites; return TRUE; }
int WINAPI Message(HWND, LPCWSTR, LPCWSTR, UINT) { ++forbidden; return IDCANCEL; }
HMONITOR WINAPI MonitorPoint(POINT, DWORD) { return reinterpret_cast<HMONITOR>(1); }
HMONITOR WINAPI MonitorRect(LPCRECT, DWORD) { return reinterpret_cast<HMONITOR>(1); }
BOOL WINAPI Monitor(HMONITOR, LPMONITORINFO info) {
    info->rcMonitor = {-1280, -720, 1280, 720};
    info->rcWork = work;
    return TRUE;
}
HRESULT WINAPI Factory(REFIID, void**) { return E_FAIL; }
void Drain() {
    std::deque<std::function<void()>> copy;
    { std::lock_guard lock(mutex); copy.swap(callbacks); }
    for (auto& callback : copy) callback();
}
template<class Predicate> bool Until(Predicate done) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        Drain();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}
bool Png(int width, int height, std::vector<uint32_t> pixels, const std::wstring& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    for (auto& pixel : pixels) pixel |= 0xFF000000;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(width, height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(frame->SetPixelFormat(&format)) &&
           SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size() * 4),
                                        reinterpret_cast<BYTE*>(pixels.data()))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}
}  // namespace test

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HICON g_icon = nullptr;
void Log(const wchar_t*, ...) {}
void TrimMemory() {}
bool IsEnglish() { return false; }
void RunOnMain(std::function<void()> callback) {
    std::lock_guard lock(test::mutex);
    test::callbacks.push_back(std::move(callback));
}
int IniInt(const wchar_t*, const wchar_t* key, int fallback) {
    if (!wcscmp(key, L"FollowCursor")) return test::followDefault;
    if (!wcscmp(key, L"AutoStart")) return 0;
    return fallback;
}
std::wstring IniString(const wchar_t*, const wchar_t*, const wchar_t* fallback) { return fallback; }
std::wstring ZipshotIni() { return test::root + L"\\synthetic.ini"; }
void EnsureUnicodeIni() {}
std::wstring FindFFmpeg() { return test::root + L"\\not-a-real-ffmpeg.exe"; }
std::wstring NewOutputPath(const wchar_t* extension) {
    return test::root + L"\\capture_" + std::to_wstring(++test::outputNumber) + L"." + extension;
}
void CopyFileToClipboard(const std::wstring& path) { test::clipboard = path; }
void PlayUiSound(Sound) {}
void ShowToast(ImagePtr, const std::wstring&, const std::wstring& title) { test::toast = title; }
void MainWindowNotifyCapture(const std::wstring& path) { test::notified.push_back(path); }
HBITMAP CreateDib(int width, int height, void** bits) {
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    return CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, bits, nullptr, 0);
}
namespace cursor_video {
struct PreparedSource::State {
    std::wstring source;
    Track track;
};
PreparedSource::PreparedSource() = default;
PreparedSource::~PreparedSource() = default;
PreparedSource::PreparedSource(PreparedSource&&) noexcept = default;
PreparedSource& PreparedSource::operator=(PreparedSource&&) noexcept = default;

std::wstring TrackPath(const std::wstring& original) { return original + L".cursor.json"; }
Result SaveTrack(const std::wstring& original, int width, int height, double fps, double duration,
                 const std::vector<cursor_follow::Sample>& samples) {
    ++test::saves;
    if (test::failSave) return {false, L"Synthetic sidecar failure"};
    test::savedSource = original;
    test::savedTrack = {width, height, fps, duration, samples};
    return {test::WriteOwned(TrackPath(original), "synthetic cursor metadata"), L""};
}
Result SaveTrackPrepared(const std::wstring& original, int width, int height, double fps, double duration,
                         const std::vector<cursor_follow::Sample>& samples, PreparedSource& out) {
    ++test::preparedSaves;
    out.state_.reset();
    Result saved = SaveTrack(original, width, height, fps, duration, samples);
    if (saved.ok) {
        auto state = std::make_unique<PreparedSource::State>();
        state->source = original;
        state->track = {width, height, fps, duration, samples};
        out.state_ = std::move(state);
    }
    return saved;
}
bool LoadTrack(const std::wstring& original, Track& out, std::wstring*) {
    ++test::loads;
    if (original != test::savedSource) return false;
    out = test::savedTrack;
    return true;
}
Result Render(const std::wstring&, const std::wstring&, const Track&, const std::wstring& out, bool forceAll) {
    ++test::renders;
    test::renderTarget = out;
    test::renderEntered = true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (test::blockRender && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (forceAll) ++test::forbidden;
    if (test::failRender) return {false, L"Synthetic render failure"};
    return {test::WriteOwned(out, "synthetic focused video"), L""};
}
Result RenderPrepared(const std::wstring& ffmpeg, const PreparedSource& source,
                      const std::wstring& out, bool forceAll) {
    ++test::preparedRenders;
    if (!source.state_ || source.state_->source.empty()) return {false, L"Synthetic prepared source unavailable"};
    return Render(ffmpeg, source.state_->source, source.state_->track, out, forceAll);
}
}  // namespace cursor_video

#define GetTickCount test::Time
#define GetCursorInfo test::Cursor
#define GetTempPathW test::TempPath
#define CreateProcessW test::Process
#define WaitForSingleObject test::WaitProcess
#define GetExitCodeProcess test::ExitCode
#define TerminateProcess test::Terminate
#define CloseHandle test::Close
#define ShowWindow test::HiddenShow
#define UpdateLayeredWindow test::Layered
#define SetWindowDisplayAffinity test::Affinity
#define RegisterHotKey test::Register
#define UnregisterHotKey test::Unregister
#define SetTimer test::Timer
#define KillTimer test::Kill
#define WritePrivateProfileStringW test::Profile
#define MessageBoxW test::Message
#define MonitorFromPoint test::MonitorPoint
#define MonitorFromRect test::MonitorRect
#define GetMonitorInfoW test::Monitor
#define CreateDXGIFactory1 test::Factory
#ifdef ZIPSHOT_STAGED_CURSOR_RECORDER
#include "../build/cursor-qa/recorder_cursor.cpp"
#else
#include "../src/recorder.cpp"
#endif
#undef GetTickCount
#undef GetCursorInfo
#undef GetTempPathW
#undef CreateProcessW
#undef WaitForSingleObject
#undef GetExitCodeProcess
#undef TerminateProcess
#undef CloseHandle
#undef ShowWindow
#undef UpdateLayeredWindow
#undef SetWindowDisplayAffinity
#undef RegisterHotKey
#undef UnregisterHotKey
#undef SetTimer
#undef KillTimer
#undef WritePrivateProfileStringW
#undef MessageBoxW
#undef MonitorFromPoint
#undef MonitorFromRect
#undef GetMonitorInfoW
#undef CreateDXGIFactory1

namespace test {
bool FollowButton() {
    const auto buttons = PanelButtons();
    return std::any_of(buttons.begin(), buttons.end(), [](const auto& b) { return b.first == PB_Follow; });
}
void Start(bool gif = false) { RecorderStart({-1000, -100, 0, 500}, gif); }
void Finish() {
    RecorderStop();
    Check(Until([] { return !RecorderBusy(); }), "synthetic finalization releases processing guard");
}
bool PanelPreview(const std::wstring& name) {
    RECT area{};
    GetClientRect(g.panel, &area);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDib(area.right, area.bottom, &bits);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    SendMessageW(g.panel, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    GdiFlush();
    std::vector<uint32_t> pixels(static_cast<uint32_t*>(bits),
                                static_cast<uint32_t*>(bits) + area.right * area.bottom);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return Png(area.right, area.bottom, std::move(pixels), root + L"\\" + name);
}
}  // namespace test

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    test::root = std::filesystem::absolute(argv[1]).wstring();
    if (std::filesystem::exists(test::root)) return 2;
    std::filesystem::create_directories(test::root);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_inst = GetModuleHandleW(nullptr);
    const HWND foreground = GetForegroundWindow();
    RecorderToggleFollow();
    test::Check(test::cursorReads == 0 && test::registered == 0 && !RecorderFollowEnabled(),
                "idle recorder neither monitors cursor nor registers shortcut");
    test::Start();
    test::Check(g.active && g.state == RS_Waiting && !g.followEnabled && test::FollowButton(),
                "MP4 panel starts with explicit follow OFF control");
    test::Check(!IsWindowVisible(g.panel) && !IsWindowVisible(g.frame) && test::registered == 1,
                "test UI stays hidden and shortcut is scoped to active MP4 panel");
    RecordCursorSample(true);
    PanelProc(g.panel, WM_HOTKEY, kFollowHotkey, 0);
    test::Check(g.followEnabled && test::cursorReads == 0 && test::preferenceWrites == 1,
                "waiting shortcut toggles preference without reading cursor");
    test::Check(test::PanelPreview(L"panel-follow-on.png"), "genuine hidden follow-panel GDI preview is saved");
    PanelClick(PB_Start);
    test::Check(g.state == RS_Recording && g.cursorSamples.size() == 1 && g.cursorSamples[0].enabled &&
                g.cursorSamples[0].t == 0, "recording begins with mode marker at zero active time");
    test::now += 33;
    PanelProc(g.panel, WM_TIMER, kTimerCursor, 0);
    test::Check(g.cursorSamples.size() == 2 && g.cursorSamples.back().visible &&
                g.cursorSamples.back().x == .25 && g.cursorSamples.back().y == .5,
                "cursor coordinates normalize against selected rectangle including negative origin");
    test::cursor = {2000, 0};
    test::now += 17;
    RecordCursorSample();
    test::Check(!g.cursorSamples.back().visible && g.cursorSamples.back().x == .5 && g.cursorSamples.back().y == .5,
                "cursor outside capture is unavailable and outside movement is not persisted");
    test::cursor = {-750, 200};
    test::cursorVisible = false;
    test::now += 17;
    RecordCursorSample();
    test::Check(!g.cursorSamples.back().visible, "hidden cursor is explicitly marked unavailable");
    test::cursorVisible = true;
    PanelClick(PB_Follow);
    test::Check(!g.cursorSamples.back().enabled && !g.followEnabled,
                "recording button creates immediate OFF marker");
    test::now = 10100;
    RecorderPauseResume();
    const size_t completedSamples = g.cursorSamples.size();
    const int readsBeforePause = test::cursorReads;
    test::now += 5000;
    PanelProc(g.panel, WM_TIMER, kTimerCursor, 0);
    RecorderToggleFollow();
    test::Check(g.state == RS_Paused && g.recordedMs == 100 && test::cursorReads == readsBeforePause &&
                g.cursorSamples.size() == completedSamples && g.followEnabled,
                "paused mode can toggle without sampling or counting wall-time gap");
    RecorderPauseResume();
    test::Check(g.cursorSamples.back().t == .1 && g.cursorSamples.back().enabled,
                "resumed segment uses continuous active-video time and pending mode");
    test::now += 40;
    RecordCursorSample();
    // This synthetic host exposes GDI only; add a second mock encoder choice
    // so the real failed-start fallback branch can be exercised deterministically.
    g.variants.push_back(g.variants.front());
    { std::lock_guard lock(test::mutex); test::processes[g.cur.pi.hProcess] = true; }
    PanelProc(g.panel, WM_TIMER, kTimerCheck, 0);
    test::Check(g.variant == 1 && g.cursorSamples.size() == completedSamples + 1 &&
                g.cursorSamples.back().t == .1 && g.cursorSamples[completedSamples - 1].t == .1,
                "failed startup rollback removes only failed segment trace and restarts its clock");
    const std::wstring raw = g.out;
    const std::wstring collision = FocusedOutputPath(raw);
    test::WriteOwned(collision, "existing focused video must survive");
    test::now += 100;
    test::blockRender = true;
    RecorderStop();
    test::Check(test::Until([] { return test::renderEntered.load(); }) && RecorderBusy(),
                "recorder stays busy throughout focused-copy processing");
    const int spawnCount = test::spawns;
    test::Start();
    test::Check(test::spawns == spawnCount && !RecorderActive(), "processing guard rejects overlapping recording");
    test::blockRender = false;
    test::Check(test::Until([] { return !RecorderBusy(); }), "focused-copy completion releases processing guard");
    test::Check(test::savedTrack.width == 1000 && test::savedTrack.height == 600 &&
                test::savedTrack.fps == 30 && test::savedTrack.duration == .2 &&
                std::all_of(test::savedTrack.samples.begin(), test::savedTrack.samples.end(),
                            [](const auto& s) { return s.t >= 0 && s.t <= .2; }),
                "sidecar binds original dimensions and pause-free active timeline");
    test::Check(std::filesystem::exists(raw) && std::filesystem::exists(cursor_video::TrackPath(raw)) &&
                test::clipboard == test::renderTarget && test::renderTarget.find(L"_focus_2.mp4") != std::wstring::npos &&
                test::notified == std::vector<std::wstring>{raw, test::renderTarget} &&
                test::preparedSaves == 1 && test::preparedRenders == 1 && test::loads == 0,
                "successful prepared effect avoids sidecar reload and publishes separate unique focused copy");
    test::Check(std::filesystem::file_size(collision) == 35 && std::filesystem::file_size(raw) == 4096 &&
                test::registered == test::unregistered, "existing destination and raw survive; panel shortcut unregisters");

    test::notified.clear();
    test::Start();
    PanelClick(PB_Start);
    test::now += 100;
    const std::wstring defaultRaw = g.out;
    test::Finish();
    test::Check(test::saves == 2 && test::renders == 1 && test::clipboard == defaultRaw &&
                std::all_of(test::savedTrack.samples.begin(), test::savedTrack.samples.end(),
                            [](const auto& s) { return !s.enabled; }),
                "default OFF still saves cursor metadata for later application without automatic render");

    test::followDefault = true;
    test::failRender = true;
    test::Start();
    PanelClick(PB_Start);
    test::now += 100;
    const std::wstring failedRaw = g.out;
    test::Finish();
    test::Check(std::filesystem::exists(failedRaw) && test::clipboard == failedRaw &&
                test::toast.find(L"Исходник сохранён") != std::wstring::npos,
                "render failure retains original and reports inline saved-original warning");
    test::failRender = false;
    test::failSave = true;
    test::Start();
    PanelClick(PB_Start);
    test::now += 100;
    const int rendersBeforeSaveFailure = test::renders;
    const std::wstring sidecarFailedRaw = g.out;
    test::Finish();
    test::Check(std::filesystem::exists(sidecarFailedRaw) && test::renders == rendersBeforeSaveFailure &&
                test::toast.find(L"данные курсора") != std::wstring::npos,
                "sidecar failure keeps original and skips unbound effect processing");
    test::failSave = false;
    const int readsBeforeGif = test::cursorReads, savesBeforeGif = test::saves, rendersBeforeGif = test::renders;
    test::Start(true);
    RecorderToggleFollow();
    test::Check(!test::FollowButton() && !g.followHotkey && !g.followEnabled, "GIF panel exposes no follow control or shortcut");
    PanelClick(PB_Start);
    test::now += 100;
    RecordCursorSample(true);
    test::Finish();
    test::Check(test::cursorReads == readsBeforeGif && test::saves == savesBeforeGif && test::renders == rendersBeforeGif,
                "GIF recording never reads cursor metadata or runs MP4 follow processing");

    test::hotkeyAllowed = false;
    test::followDefault = false;
    test::work = {0, 0, 300, 200};
    test::Start();
    RECT panel{};
    GetWindowRect(g.panel, &panel);
    PanelClick(PB_Follow);
    test::Check(!g.followHotkey && test::FollowButton() && g.followEnabled,
                "shortcut conflict leaves explicit follow button functional");
    test::Check(panel.left >= 0 && panel.top >= 0 && panel.right <= 300 && panel.bottom <= 200,
                "recording panel stays inside monitor work area even on narrow display");
    const auto buttons = PanelButtons();
    test::Check(std::all_of(buttons.begin(), buttons.end(), [](const auto& b) {
        return b.second.left >= 0 && b.second.right <= 300 && b.second.right > b.second.left;
    }), "narrow panel keeps every button inside client bounds");
    RecorderAbort();
    test::Check(test::Until([] { return !RecorderBusy(); }), "abort before recording cleans up owned panel state");
    g = {};
    g.active = true;
    g.state = RS_Recording;
    g.r = {-1000, -100, 0, 500};
    g.segStart = test::now - 100;
    g.followEnabled = true;
    g.cursorSamples.resize(kMaxCursorSamples, cursor_follow::Sample{0, .5, .5, true});
    const int readsBeforeLimit = test::cursorReads;
    RecordCursorSample(true);
    const double lastAtLimit = g.cursorSamples.back().t;
    test::now += 100;
    RecordCursorSample(true);
    test::Check(g.cursorSamples.size() == kMaxCursorSamples && g.cursorLimitReached && !g.followEnabled &&
                !g.cursorSamples.back().enabled && !g.cursorSamples.back().visible &&
                g.cursorSamples.back().t == lastAtLimit && test::cursorReads == readsBeforeLimit,
                "tracking limit stays bounded and ends with neutral marker without further cursor reads");
    g = {};
    test::Check(test::forbidden == 0 && test::processes.empty() && test::registered == test::unregistered,
                "all fake process handles and scoped shortcuts are cleaned up without forbidden APIs");
    test::Check(GetForegroundWindow() == foreground, "synthetic recorder test leaves host foreground unchanged");
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("%d assertions, %d failures\n", test::checks, test::failures);
    return test::failures ? 1 : 0;
}
