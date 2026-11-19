// Real mainwindow.cpp, synthetic files only. All activation, keyboard-state and
// clipboard boundaries below are intercepted before including the implementation.
#include "../src/common.h"
#include "../src/combine.h"
#include "../src/clipboard_shelf.h"
#include "../src/sharex_import.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
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

using Microsoft::WRL::ComPtr;

namespace test {
std::wstring root, saveRoot;
std::mutex queueMutex;
std::deque<std::function<void()>> callbacks;
std::map<std::wstring, bool> settings;
std::atomic<int> readCalls{0}, storeCalls{0}, forbiddenCalls{0};
bool mediaAvailable = true, controlDown = false, english = false;
HWND virtualFocus = nullptr;
HDC renderDC = nullptr;
int foregroundBlocked = 0, topLevelShowBlocked = 0, defaultTextPastes = 0;
int checks = 0, failures = 0;

void Check(bool ok, const char* text) {
    ++checks;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
    if (!ok) ++failures;
}

BOOL WINAPI HiddenShowWindow(HWND hwnd, int how) {
    if (GetParent(hwnd)) return ::ShowWindow(hwnd, how);
    ++topLevelShowBlocked;
    return ::ShowWindow(hwnd, SW_HIDE);
}
BOOL WINAPI BlockForeground(HWND) { ++foregroundBlocked; return FALSE; }
HWND WINAPI VirtualSetFocus(HWND hwnd) {
    const HWND old = virtualFocus;
    virtualFocus = hwnd;
    return old;
}
HWND WINAPI VirtualGetFocus() { return virtualFocus; }
SHORT WINAPI VirtualKeyState(int key) {
    return key == VK_CONTROL && controlDown ? static_cast<SHORT>(0x8000) : 0;
}
BOOL WINAPI SyntheticClipboardFormat(UINT format) {
    return mediaAvailable && format == CF_DIB;
}
UINT WINAPI SyntheticRegisterFormat(LPCWSTR) { return 0xC123; }
BOOL WINAPI BlockOpenClipboard(HWND) { ++forbiddenCalls; return FALSE; }
BOOL WINAPI BlockCloseClipboard() { ++forbiddenCalls; return FALSE; }
BOOL WINAPI BlockEmptyClipboard() { ++forbiddenCalls; return FALSE; }
HANDLE WINAPI BlockClipboardData(UINT) { ++forbiddenCalls; return nullptr; }
HANDLE WINAPI BlockSetClipboardData(UINT, HANDLE) { ++forbiddenCalls; return nullptr; }
BOOL WINAPI SyntheticCursor(LPPOINT point) { *point = {0, 0}; return TRUE; }
HINSTANCE WINAPI BlockShell(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT) {
    ++forbiddenCalls; return reinterpret_cast<HINSTANCE>(SE_ERR_ACCESSDENIED);
}
int WINAPI BlockMessageBox(HWND, LPCWSTR, LPCWSTR, UINT) { ++forbiddenCalls; return IDCANCEL; }
BOOL WINAPI BlockPopup(HMENU, UINT, int, int, HWND, LPTPMPARAMS) { ++forbiddenCalls; return 0; }

LRESULT WINAPI SafeCallWindowProc(WNDPROC proc, HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_PASTE) {
        // Proves that the search subclass forwards plain text paste, without
        // letting the native EDIT control read this computer's clipboard.
        ++defaultTextPastes;
        return ::CallWindowProcW(proc, hwnd, EM_REPLACESEL, TRUE,
                                 reinterpret_cast<LPARAM>(L"Архив_Москва"));
    }
    return ::CallWindowProcW(proc, hwnd, message, w, l);
}

HDC WINAPI OwnedBeginPaint(HWND hwnd, LPPAINTSTRUCT ps) {
    if (!renderDC) return ::BeginPaint(hwnd, ps);
    *ps = {};
    ps->hdc = renderDC;
    GetClientRect(hwnd, &ps->rcPaint);
    return renderDC;
}
BOOL WINAPI OwnedEndPaint(HWND hwnd, const PAINTSTRUCT* ps) {
    if (!renderDC) return ::EndPaint(hwnd, ps);
    return ValidateRect(hwnd, nullptr);
}

bool WritePng(const Image& image, const std::wstring& path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(image.w, image.h))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(frame->SetPixelFormat(&format)) &&
           IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
           SUCCEEDED(frame->WritePixels(image.h, image.w * 4,
                                        static_cast<UINT>(image.px.size() * 4),
                                        reinterpret_cast<BYTE*>(const_cast<uint32_t*>(image.px.data())))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

void DrainCallbacks() {
    std::deque<std::function<void()>> local;
    { std::lock_guard lock(queueMutex); local.swap(callbacks); }
    for (auto& callback : local) callback();
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

template<class Predicate> bool Wait(Predicate done, int milliseconds = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        DrainCallbacks();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

void TouchAt(const std::wstring& path, uint64_t ordinal) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    const uint64_t time = 134080704000000000ULL + ordinal * 10000000ULL;
    FILETIME ft{static_cast<DWORD>(time), static_cast<DWORD>(time >> 32)};
    if (file == INVALID_HANDLE_VALUE || !SetFileTime(file, &ft, &ft, &ft)) ++forbiddenCalls;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}
}  // namespace test

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HICON g_icon = nullptr;
void RunOnMain(std::function<void()> callback) {
    std::lock_guard lock(test::queueMutex);
    test::callbacks.push_back(std::move(callback));
}
void TrimMemory() {}
void Log(const wchar_t*, ...) {}
bool IsEnglish() { return test::english; }
void SetEnglish(bool value) { test::english = value; }
std::wstring ExeDir() { return test::root; }
std::wstring SaveDirRoot() { return test::saveRoot; }
bool GetSettingBool(const wchar_t* key, bool fallback) {
    auto it = test::settings.find(key);
    return it == test::settings.end() ? fallback : it->second;
}
void SetSettingBool(const wchar_t* key, bool value) { test::settings[key] = value; }
std::vector<HotkeyInfo> HotkeyStatus() {
    return {{L"PrtScn", L"Снимок экрана", true}, {L"Ctrl+PrtScn", L"Захват области", true}};
}
void RunCommand(int) { ++test::forbiddenCalls; }
bool AutostartEnabled() { return false; }
void SetAutostart(bool) { ++test::forbiddenCalls; }
std::wstring ZipshotIni() { return test::root + L"\\test-settings.ini"; }
void EnsureUnicodeIni() {}
bool LoadImageFile(const std::wstring&, Image&) { return false; }
void OpenEditor(ImagePtr, const std::wstring&) { ++test::forbiddenCalls; }
void CopyImageToClipboard(const Image&) { ++test::forbiddenCalls; }
void CopyFileToClipboard(const std::wstring&) { ++test::forbiddenCalls; }
void PlayUiSound(Sound) {}
CombineOptions LoadCombineOptions() { return {}; }
bool ShowCombineDialog(HWND, std::vector<std::wstring>&, CombineOptions&) { ++test::forbiddenCalls; return false; }
void CombineFilesAsync(std::vector<std::wstring>, CombineOptions,
                       std::function<void(const std::wstring&)>) { ++test::forbiddenCalls; }
HBITMAP CreateDib(int width, int height, void** bits) {
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    return CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, bits, nullptr, 0);
}

namespace shelf {
std::wstring Directory() { return test::saveRoot + L"\\Shelf"; }
bool IsShelfPath(const std::wstring& path) {
    const std::wstring prefix = Directory() + L"\\";
    return path.size() >= prefix.size() &&
           CompareStringOrdinal(path.c_str(), static_cast<int>(prefix.size()),
                                prefix.c_str(), static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}
bool ReadClipboard(HWND, Payload& out, std::wstring& error) {
    ++test::readCalls;
    if (!test::mediaAvailable) { error = L"Тестовый буфер не содержит изображения"; return false; }
    out.image = {160, 100, false, std::vector<uint32_t>(16000)};
    for (int y = 0; y < out.image.h; ++y)
        for (int x = 0; x < out.image.w; ++x)
            out.image.px[y * out.image.w + x] = 0xFF000000 | (0x3A << 16) |
                ((x * 255 / out.image.w) << 8) | (255 - y * 180 / out.image.h);
    return true;
}
SaveResult Store(Payload data) {
    const int number = ++test::storeCalls;
    const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring path = Directory() + L"\\Вставка_" + std::to_wstring(number) + L".png";
    const bool ok = test::WritePng(data.image, path);
    if (ok) test::TouchAt(path, 20000 + number);
    if (SUCCEEDED(hr)) CoUninitialize();
    return ok ? SaveResult{{path}, L""} : SaveResult{{}, L"Synthetic PNG could not be written"};
}
}  // namespace shelf

#define ShowWindow test::HiddenShowWindow
#define SetForegroundWindow test::BlockForeground
#define SetFocus test::VirtualSetFocus
#define GetFocus test::VirtualGetFocus
#define GetKeyState test::VirtualKeyState
#define IsClipboardFormatAvailable test::SyntheticClipboardFormat
#define RegisterClipboardFormatW test::SyntheticRegisterFormat
#define OpenClipboard test::BlockOpenClipboard
#define CloseClipboard test::BlockCloseClipboard
#define EmptyClipboard test::BlockEmptyClipboard
#define GetClipboardData test::BlockClipboardData
#define SetClipboardData test::BlockSetClipboardData
#define GetCursorPos test::SyntheticCursor
#define ShellExecuteW test::BlockShell
#define MessageBoxW test::BlockMessageBox
#define TrackPopupMenuEx test::BlockPopup
#define CallWindowProcW test::SafeCallWindowProc
#define BeginPaint test::OwnedBeginPaint
#define EndPaint test::OwnedEndPaint
#include "../src/mainwindow.cpp"
#undef ShowWindow
#undef SetForegroundWindow
#undef SetFocus
#undef GetFocus
#undef GetKeyState
#undef IsClipboardFormatAvailable
#undef RegisterClipboardFormatW
#undef OpenClipboard
#undef CloseClipboard
#undef EmptyClipboard
#undef GetClipboardData
#undef SetClipboardData
#undef GetCursorPos
#undef ShellExecuteW
#undef MessageBoxW
#undef TrackPopupMenuEx
#undef CallWindowProcW
#undef BeginPaint
#undef EndPaint

namespace test {
int IndexOf(const std::wstring& path) {
    for (int i = 0; i < static_cast<int>(g_mw->files.size()); ++i)
        if (g_mw->files[i].path == path) return i;
    return -1;
}
std::wstring SelectedPath() { return Selected() ? Selected()->path : L""; }
void Query(const wchar_t* text) { SetWindowTextW(g_mw->search, text); }

void SizeClient(int width, int height, UINT dpi) {
    ApplyDpi(dpi);
    RECT outer{0, 0, width, height};
    AdjustWindowRectExForDpi(&outer, static_cast<DWORD>(GetWindowLongPtrW(g_mw->hwnd, GWL_STYLE)),
                             FALSE, 0, GetDpiForWindow(g_mw->hwnd));
    SetWindowPos(g_mw->hwnd, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                  SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
    LayoutToolbar();
}

void CheckLayout(const char* message) {
    const ToolbarLayout layout = GetToolbarLayout();
    const RECT content = ContentRect();
    RECT client{};
    GetClientRect(g_mw->hwnd, &client);
    std::vector<RECT> controls{layout.search, layout.paste, layout.sort};
    for (const RECT& filter : layout.filters) controls.push_back(filter);
    if (layout.status.right > layout.status.left) controls.push_back(layout.status);
    bool valid = true;
    for (size_t i = 0; i < controls.size(); ++i) {
        const RECT& r = controls[i];
        valid &= r.left >= g_mw->panelW && r.top >= 0 && r.right <= client.right &&
                 r.bottom <= content.top && r.right > r.left && r.bottom > r.top;
        for (size_t j = 0; j < i; ++j) {
            RECT overlap{};
            valid &= !IntersectRect(&overlap, &r, &controls[j]);
        }
    }
    valid &= GetGrid().y0 >= content.top && content.top == layout.height;
    Check(valid, message);
    RECT edit{};
    GetWindowRect(g_mw->search, &edit);
    MapWindowPoints(nullptr, g_mw->hwnd, reinterpret_cast<POINT*>(&edit), 2);
    Check(edit.left >= layout.search.left && edit.top >= layout.search.top &&
          edit.right <= layout.search.right && edit.bottom <= layout.search.bottom,
          "native search EDIT stays inside its toolbar rectangle");
}

bool Preview(const std::wstring& path) {
    RECT client{};
    GetClientRect(g_mw->hwnd, &client);
    Image image;
    image.w = client.right;
    image.h = client.bottom;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDib(image.w, image.h, &bits);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    renderDC = dc;
    SendMessageW(g_mw->hwnd, WM_PAINT, 0, 0);  // The actual production Paint path.
    renderDC = nullptr;
    if (g_mw->search && GridMode()) {
        RECT edit{};
        GetWindowRect(g_mw->search, &edit);
        MapWindowPoints(nullptr, g_mw->hwnd, reinterpret_cast<POINT*>(&edit), 2);
        const int saved = SaveDC(dc);
        SetViewportOrgEx(dc, edit.left, edit.top, nullptr);
        SendMessageW(g_mw->search, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
    }
    GdiFlush();
    image.px.assign(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + image.w * image.h);
    for (auto& pixel : image.px) pixel |= 0xFF000000;
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    std::set<uint32_t> colors(image.px.begin(), image.px.end());
    return colors.size() > 32 && WritePng(image, path);
}
}  // namespace test

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    test::root = std::filesystem::absolute(argv[1]).wstring();
    // Reuse is forbidden: tests can only create their own fresh synthetic tree.
    if (std::filesystem::exists(test::root)) return 2;
    test::saveRoot = test::root + L"\\catalog";
    std::filesystem::create_directories(test::saveRoot + L"\\2026-11");
    std::filesystem::create_directories(shelf::Directory());
    const HRESULT com = OleInitialize(nullptr);
    g_inst = GetModuleHandleW(nullptr);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HWND foregroundBefore = GetForegroundWindow();
    WNDCLASSW dummy{};
    dummy.lpfnWndProc = DefWindowProcW;
    dummy.hInstance = g_inst;
    dummy.lpszClassName = L"ZipshotGalleryRegressionDummyOnly";
    RegisterClassW(&dummy);
    g_main = CreateWindowExW(WS_EX_NOACTIVATE, dummy.lpszClassName, L"Synthetic gallery test owner", 0,
                             0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);

    Image fixture{160, 100, false, std::vector<uint32_t>(16000)};
    for (int y = 0; y < fixture.h; ++y)
        for (int x = 0; x < fixture.w; ++x)
            fixture.px[y * fixture.w + x] = 0xFF000000 | ((x * 255 / fixture.w) << 16) |
                                           ((y * 255 / fixture.h) << 8) | 0x80;
    const std::wstring templatePath = test::root + L"\\synthetic-fixture.png";
    test::Check(test::WritePng(fixture, templatePath), "synthetic PNG fixture is valid");
    std::wstring archive;
    for (unsigned i = 0; i < 620; ++i) {
        const std::wstring name = i ? L"Снимок_" + std::to_wstring(i) + L".png" : L"Архив_Москва.PNG";
        const std::wstring path = test::saveRoot + L"\\2026-11\\" + name;
        if (!CopyFileW(templatePath.c_str(), path.c_str(), TRUE)) return 2;
        test::TouchAt(path, i + 1);
        if (!i) archive = path;
    }
    const std::wstring gif = test::saveRoot + L"\\Движение.gif";
    const std::wstring video = test::saveRoot + L"\\Ролик.MP4";
    // Empty extension markers test catalog classification, not media decoding.
    std::ofstream(std::filesystem::path(gif), std::ios::binary).put('\0');
    std::ofstream(std::filesystem::path(video), std::ios::binary).put('\0');
    test::TouchAt(gif, 800);
    test::TouchAt(video, 900);
    const std::wstring shelfImage = shelf::Directory() + L"\\Вставка_Образец.png";
    CopyFileW(templatePath.c_str(), shelfImage.c_str(), TRUE);
    test::TouchAt(shelfImage, 1000);

    ShowMainWindow();
    test::Check(g_mw && IsWindow(g_mw->hwnd) && g_mw->search, "real main window and native search EDIT are created");
    if (!g_mw || !g_mw->search) return 1;
    wchar_t searchCue[128]{};
    test::Check(SendMessageW(g_mw->search, EM_GETCUEBANNER,
                            reinterpret_cast<WPARAM>(searchCue), std::size(searchCue)) &&
                std::wstring(searchCue) == L"Поиск по имени...",
                "native search EDIT retains localized empty-field cue banner");
    test::Check(!IsWindowVisible(g_mw->hwnd), "test main window remains hidden");
    test::Check(g_mw->catalog.size() == 623 && g_mw->files.size() == 623,
                "real directory scan and gallery retain more than 500 captures");
    test::Check(GridMode() && !g_mw->showTable, "gallery is the default view");
    const std::wstring populatedRoot = test::saveRoot;
    test::saveRoot = test::root + L"\\empty-catalog";
    std::filesystem::create_directory(test::saveRoot);
    Rescan();
    test::Check(g_mw->catalog.empty() && g_mw->files.empty() && GridMode() && !g_mw->showTable,
                "empty capture directory keeps gallery empty state rather than hotkey table");
    test::saveRoot = populatedRoot;
    Rescan();
    test::Query(L"мОсКвА.pNg");
    test::Check(g_mw->files.size() == 1 && g_mw->files[0].path == archive,
                "native EDIT search finds oldest Cyrillic filename case-insensitively");
    test::Query(L"no-such-synthetic-capture");
    test::Check(g_mw->files.empty() && GridMode() && !g_mw->showTable,
                "no-result search keeps gallery empty state rather than hotkey table");
    test::Query(L"");
    ToolbarCommand(kFilterBase + static_cast<UINT>(gallery::Filter::Images));
    test::Check(g_mw->files.size() == 621, "Photos filter excludes GIF and video in actual window");
    ToolbarCommand(kFilterBase + static_cast<UINT>(gallery::Filter::Gifs));
    test::Check(g_mw->files.size() == 1 && g_mw->files[0].path == gif, "GIF filter is independent of Photos");
    ToolbarCommand(kFilterBase + static_cast<UINT>(gallery::Filter::Videos));
    test::Check(g_mw->files.size() == 1 && g_mw->files[0].path == video, "video filter routes to video catalog entry");
    ToolbarCommand(kFilterBase + static_cast<UINT>(gallery::Filter::Shelf));
    test::Check(g_mw->files.size() == 1 && g_mw->files[0].path == shelfImage, "Shelf filter uses shelf metadata");
    ToolbarCommand(kFilterBase);
    ToolbarCommand(kOldest);
    test::Check(g_mw->files.front().path == archive && GetSettingBool(L"GalleryOldestFirst", false),
                "oldest sort changes actual view and persists setting");
    const std::wstring later = test::saveRoot + L"\\Поздний_снимок.png";
    CopyFileW(templatePath.c_str(), later.c_str(), TRUE);
    test::TouchAt(later, 5000);
    MainWindowNotifyCapture(later);
    test::Check(g_mw->files.back().path == later && g_mw->catalog.size() == 624,
                "capture notification respects oldest-first order");
    ToolbarCommand(kFilterBase + static_cast<UINT>(gallery::Filter::Videos));
    MainWindowNotifyCapture(later);
    test::Check(g_mw->files.size() == 1 && g_mw->files[0].path == video && g_mw->catalog.size() == 624,
                "image notification respects active video filter and does not duplicate metadata");
    ToolbarCommand(kFilterBase);
    test::Query(L"Москва");
    MainWindowNotifyCapture(later);
    test::Check(g_mw->files.size() == 1 && g_mw->files[0].path == archive,
                "capture notification preserves active filename query");
    test::Query(L"");
    ToolbarCommand(kNewest);
    test::Check(g_mw->files.front().path == later && !GetSettingBool(L"GalleryOldestFirst", true),
                "newest sort restores descending order and persists setting");
    g_mw->sel = test::IndexOf(archive);
    g_mw->anchor = test::IndexOf(gif);
    g_mw->picked = {archive, gif};
    ToolbarCommand(kOldest);
    test::Check(test::SelectedPath() == archive && g_mw->anchor >= 0 &&
                g_mw->files[g_mw->anchor].path == gif && g_mw->picked == std::vector<std::wstring>{archive, gif},
                "sort preserves focused path, anchor path and pick order");
    ToolbarCommand(kFilterBase + static_cast<UINT>(gallery::Filter::Images));
    test::Check(test::SelectedPath() == archive && g_mw->anchor == -1 &&
                g_mw->picked == std::vector<std::wstring>{archive},
                "filter retains visible focus and prunes hidden anchor and picks");
    test::Query(L"absent");
    test::Check(g_mw->sel == -1 && g_mw->anchor == -1 && g_mw->picked.empty(),
                "search hiding all files clears stale selection paths");
    test::controlDown = true;
    SendMessageW(g_mw->hwnd, WM_KEYDOWN, 'F', 0);
    test::Check(test::virtualFocus == g_mw->search, "Ctrl+F routes focus to search through intercepted SetFocus");
    const int readsBefore = test::readCalls;
    SendMessageW(g_mw->hwnd, WM_KEYDOWN, 'V', 0);
    SendMessageW(g_mw->hwnd, WM_KEYDOWN, 'V', 0);
    test::Check(test::readCalls == readsBefore + 1, "busy paste ignores duplicate Ctrl+V before completion");
    test::Check(test::Wait([] { return !g_mw->pasting; }), "background paste result returns through RunOnMain queue");
    test::Check(g_mw->filter == gallery::Filter::Shelf && g_mw->query.empty() &&
                test::SelectedPath() == shelf::Directory() + L"\\Вставка_1.png",
                "Ctrl+V selects saved synthetic image in Shelf and clears search");
    SendMessageW(g_mw->search, WM_KEYDOWN, 'V', 0);
    SendMessageW(g_mw->search, WM_CHAR, 22, 0);
    test::Check(test::Wait([] { return !g_mw->pasting; }) && test::readCalls == readsBefore + 2 &&
                test::storeCalls == 2, "media paste in search routes once without duplicate WM_CHAR paste");
    test::mediaAvailable = false;
    test::Query(L"");
    const int readsBeforeText = test::readCalls;
    SendMessageW(g_mw->search, WM_PASTE, 0, 0);
    test::Check(test::defaultTextPastes == 1 && test::readCalls == readsBeforeText &&
                g_mw->query == L"Архив_Москва", "plain text paste stays with native search editing, not Shelf");
    PasteToShelf();
    test::Check(!g_mw->pasting && !g_mw->notice.empty() && test::storeCalls == 2,
                "unsupported clipboard produces inline notice without starting a worker");
    test::controlDown = false;
    test::Query(L"");
    ToolbarCommand(kFilterBase + static_cast<UINT>(gallery::Filter::Shelf));
    ToolbarCommand(kNewest);
    test::SizeClient(1200, 760, 96);
    test::CheckLayout("wide toolbar controls stay inside toolbar and outside grid");
    test::Preview(test::root + L"\\warmup.png");
    test::Wait([] { return std::all_of(g_mw->files.begin(), g_mw->files.end(), [](const Entry& e) { return e.done; }); });
    test::Check(test::Preview(test::root + L"\\gallery-1200x760.png"), "1200x760 genuine GDI gallery preview saved");
    test::SizeClient(740, 700, 96);
    test::CheckLayout("narrow toolbar controls wrap without overlapping grid");
    test::Check(test::Preview(test::root + L"\\gallery-740x700.png"), "740x700 genuine GDI gallery preview saved");
    test::SizeClient(1110, 1050, 144);
    test::CheckLayout("144 DPI toolbar and content geometry stay separate");
    test::Check(!IsWindowVisible(g_mw->hwnd) && GetForegroundWindow() != g_mw->hwnd &&
                test::foregroundBlocked > 0 && test::topLevelShowBlocked > 0,
                "test window never activates and all top-level show requests are blocked");
    std::weak_ptr<Worker> stopping = g_worker;
    DestroyWindow(g_mw->hwnd);
    test::Check(test::Wait([&] { return stopping.expired(); }), "owned thumbnail worker exits after window teardown");
    DestroyWindow(g_main);
    test::Check(test::forbiddenCalls == 0, "no clipboard API, shell, modal dialog or app command boundary was invoked");
    test::Check(GetForegroundWindow() == foregroundBefore, "host foreground window is unchanged");
    if (SUCCEEDED(com)) OleUninitialize();
    std::printf("%d assertions, %d failures\n", test::checks, test::failures);
    return test::failures ? 1 : 0;
}
