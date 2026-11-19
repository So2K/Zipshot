#include "common.h"
#include "shapes_ext.h"
#include "sharex_import.h"
#include "capture_ext.h"
#include <ole2.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <psapi.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <thread>

HINSTANCE g_inst;
HWND g_main;
HICON g_icon;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RUN = WM_APP + 2;
constexpr wchar_t kMainClass[] = L"ZipshotMain";


// Hotkey jobs, configurable in zipshot.ini [hotkeys] (e.g. Region=Ctrl+PrintScreen; several keys separated by ';',
// empty = disabled). Without the section the ShareX defaults below apply.
struct Job { int cmd; const wchar_t* key; bool hasDefault; UINT defMods; const wchar_t* ru; const wchar_t* en; };
const Job kJobs[] = {
    {C_Region, L"Region", true, MOD_CONTROL, L"Захват области", L"Capture region"},
    {C_Full, L"Full", true, 0, L"Захват всего экрана", L"Capture entire screen"},
    {C_Window, L"Window", true, MOD_ALT, L"Захват активного окна", L"Capture active window"},
    {C_ActiveMonitor, L"ActiveMonitor", false, 0, L"Захват активного монитора", L"Capture active monitor"},
    {C_LastRegion, L"LastRegion", false, 0, L"Захват последней области", L"Capture last region"},
    {C_Video, L"Video", true, MOD_SHIFT, L"Старт/стоп записи экрана", L"Start/Stop screen recording"},
    {C_Gif, L"Gif", true, MOD_CONTROL | MOD_SHIFT, L"Старт/стоп записи экрана (GIF)", L"Start/Stop screen recording (GIF)"},
};

struct Bound { int cmd; UINT mods, vk; bool ok; const Job* job; };
std::vector<Bound> g_bound;  // RegisterHotKey id = index + 1

UINT g_taskbarCreated;
constexpr UINT_PTR kTrayClickTimer = 7;
bool g_trayDouble = false;
NOTIFYICONDATAW g_nid{};
std::wstring g_failedHotkeys;
std::wstring g_lastPath;
bool g_testMode = false;

// Tray menu ids that are not shared commands.
constexpr int kTrayPause = 900, kTrayAbort = 901, kAudioNone = 2000;
HMENU g_audioMenu = nullptr;
std::vector<std::wstring> g_audioDevices;

// Camera glyph drawn at runtime, so the exe needs no resources.
HICON MakeIcon(int s) {
    void* bits;
    HBITMAP color = CreateDib(s, s, &bits);
    auto* p = (uint32_t*)bits;
    auto in = [&](float x, float y, float l, float t, float r, float b) { return x >= l && x < r && y >= t && y < b; };
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            float fx = (x + 0.5f) / s, fy = (y + 0.5f) / s;
            uint32_t c = 0;
            bool body = in(fx, fy, 0.06f, 0.28f, 0.94f, 0.86f) || in(fx, fy, 0.34f, 0.16f, 0.66f, 0.30f);
            float dx = fx - 0.5f, dy = fy - 0.56f, d = dx * dx + dy * dy;
            if (body) c = 0xFF3A8DFFu;
            if (body && d < 0.045f) c = 0xFFFFFFFFu;
            if (body && d < 0.022f) c = 0xFF1D3B6Bu;
            p[y * s + x] = c;
        }
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// ShareX "after capture tasks", toggled from the main window.
void FinishStill(Image img, std::wstring path, const wchar_t* title) {
    if (img.empty()) return;
    auto sp = std::make_shared<Image>(std::move(img));
    bool copy = GetSettingBool(L"AfterCopy", true), save = GetSettingBool(L"AfterSave", true);
    bool toast = GetSettingBool(L"AfterToast", true), editor = GetSettingBool(L"AfterEditor", false);
    if (copy) CopyImageToClipboard(*sp);
    PlayUiSound(S_Capture);
    if (!save) {
        if (editor) OpenEditor(sp, L"");
        else if (toast) ShowToast(sp, L"", title);
        return;
    }
    std::wstring t = title;
    std::thread([sp, path, t, toast, editor]() mutable {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = SaveImageAuto(*sp, path);
        CoUninitialize();
        RunOnMain([sp, path, t, ok, toast, editor] {
            if (!ok) {
                PlayUiSound(S_Error);
                MessageBoxW(nullptr, (Tr(L"Не удалось сохранить ", L"Could not save ") + path).c_str(), L"Zipshot", MB_ICONERROR);
                return;
            }
            g_lastPath = path;
            PlayUiSound(S_TaskDone);
            Log(L"saved %s", path.c_str());
            if (!g_testMode) MainWindowNotifyCapture(path);
            if (g_testMode) PostQuitMessage(0);
            else if (editor) OpenEditor(sp, path);
            else if (toast) ShowToast(sp, path, t);
        });
    }).detach();
}

bool ShowCursorSetting() { return GetSettingBool(L"ShowCursor", true); }

void CaptureFull() {
    std::wstring path = NewOutputPath(L"png");
    FinishStill(CaptureRect(VirtualScreen(), ShowCursorSetting()), path, Tr(L"Экран", L"Screen"));
}

// ShareX "Capture active monitor": the monitor under the mouse cursor.
void CaptureActiveMonitor() {
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) return;
    RECT r = mi.rcMonitor, vs = VirtualScreen();
    IntersectRect(&r, &r, &vs);
    std::wstring path = NewOutputPath(L"png");
    FinishStill(CaptureRect(r, ShowCursorSetting()), path, Tr(L"Монитор", L"Monitor"));
}

void CaptureWindow() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(fg, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(fg, &r);
    RECT vs = VirtualScreen();
    IntersectRect(&r, &r, &vs);
    std::wstring path = NewOutputPath(L"png");
    FinishStill(CaptureRect(r, ShowCursorSetting()), path, Tr(L"Окно", L"Window"));
}

// Captures can nest: a hotkey pressed while a region is being picked captures that screen too.
void CaptureRegion(bool record, bool gif) {
    static int depth = 0;
    if (depth >= 4) return;
    ++depth;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    RECT vs = VirtualScreen();
    std::wstring path = record ? L"" : NewOutputPath(L"png");
    RECT sel{};
    bool ok;
    {
        auto screen = std::make_shared<Image>(CaptureRect(vs, false));
        QueryPerformanceCounter(&t1);
        Log(L"region: screen grabbed in %.1f ms", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        if (record) {
            ok = SelectRegion(*screen, vs, sel, true);
        } else {
            Image out;
            ok = RegionCapture(screen, vs, out, sel);
            screen.reset();
            if (ok) {
                SaveLastRegion(sel);
                FinishStill(std::move(out), path, Tr(L"Область", L"Region"));
            }
        }
    }
    --depth;
    if (ok && record) RecorderStart(sel, gif);
    TrimMemory();
}

// ShareX "Capture last region": the rectangle of the last region capture, grabbed fresh; without one, a normal region capture.
void CaptureLastRegion() {
    RECT r, vs = VirtualScreen();
    if (!LoadLastRegion(r) || !IntersectRect(&r, &r, &vs)) {
        CaptureRegion(false, false);
        return;
    }
    std::wstring path = NewOutputPath(L"png");
    FinishStill(CaptureRect(r, ShowCursorSetting()), path, Tr(L"Область", L"Region"));
}

void ToggleRecord(bool gif) {
    if (RecorderActive()) { RecorderHotkey(); return; }
    if (RecorderBusy()) return;
    CaptureRegion(true, gif);
}

// Reads [hotkeys] (or the defaults) and (re)registers everything.
void RegisterHotkeys() {
    for (size_t i = 0; i < g_bound.size(); ++i) UnregisterHotKey(g_main, (int)i + 1);
    g_bound.clear();
    g_failedHotkeys.clear();
    std::wstring ini = ZipshotIni();
    wchar_t probe[8];
    bool configured = GetPrivateProfileSectionW(L"hotkeys", probe, (DWORD)std::size(probe), ini.c_str()) > 0;
    for (const Job& j : kJobs) {
        if (!configured) {
            if (j.hasDefault) g_bound.push_back({j.cmd, j.defMods, VK_SNAPSHOT, false, &j});
            continue;
        }
        std::wstring v = IniString(L"hotkeys", j.key, L"");
        size_t start = 0;
        for (;;) {
            size_t p = v.find(L';', start);
            std::wstring one = v.substr(start, p == std::wstring::npos ? std::wstring::npos : p - start);
            UINT mods, vk;
            if (ParseHotkeyText(one, mods, vk)) g_bound.push_back({j.cmd, mods, vk, false, &j});
            else if (one.find_first_not_of(L" \t") != std::wstring::npos) Log(L"hotkeys: cannot parse %s=%s", j.key, one.c_str());
            if (p == std::wstring::npos) break;
            start = p + 1;
        }
    }
    for (size_t i = 0; i < g_bound.size(); ++i) {
        Bound& b = g_bound[i];
        b.ok = RegisterHotKey(g_main, (int)i + 1, b.mods | MOD_NOREPEAT, b.vk) != 0;
        if (!b.ok) {
            if (!g_failedHotkeys.empty()) g_failedHotkeys += L", ";
            g_failedHotkeys += HotkeyDisplayText(b.mods, b.vk);
        }
    }
}

std::wstring HotkeyFor(int cmd) {
    for (const Bound& b : g_bound)
        if (b.cmd == cmd) return L"\t" + HotkeyDisplayText(b.mods, b.vk);
    return L"";
}

void AddTray() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy_s(g_nid.szTip, L"Zipshot");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void Balloon(const std::wstring& text, DWORD icon = NIIF_WARNING) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcscpy_s(n.szInfoTitle, L"Zipshot");
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = icon;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

std::wstring BusyHotkeysText() {
    return Tr(L"Хоткеи заняты другой программой (ShareX?): ", L"Hotkeys are taken by another program (ShareX?): ") + g_failedHotkeys;
}

// Tray/main window "Import ShareX settings": import, reload everything, show the report.
void ImportShareXNow() {
    std::wstring report;
    bool ok = ImportShareXSettings(report);
    if (ok) {
        SetSettingBool(L"ShareXImported", true);
        LoadSettings();
        RegisterHotkeys();
        MainWindowRefresh();
        if (!g_failedHotkeys.empty()) report += L"\n\n" + BusyHotkeysText();
    }
    MessageBoxW(nullptr, report.c_str(), Tr(L"Импорт настроек ShareX", L"Import ShareX settings"),
                MB_OK | MB_SETFOREGROUND | (ok ? MB_ICONINFORMATION : MB_ICONWARNING));
}

void Run(int cmd) {
    switch (cmd) {
    case C_Full: CaptureFull(); break;
    case C_Region: CaptureRegion(false, false); break;
    case C_Window: CaptureWindow(); break;
    case C_ActiveMonitor: CaptureActiveMonitor(); break;
    case C_LastRegion: CaptureLastRegion(); break;
    case C_Video: ToggleRecord(false); break;
    case C_Gif: ToggleRecord(true); break;
    case C_Folder: {
        std::wstring p = NewOutputPath(L"png");
        p = p.substr(0, p.find_last_of(L'\\'));
        ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case C_Settings:
        ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + ExeDir() + L"\\zipshot.ini\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case C_Rehook:
        RegisterHotkeys();
        if (g_failedHotkeys.empty()) Balloon(Tr(L"Все хоткеи активны", L"All hotkeys are active"), NIIF_INFO);
        else Balloon(Tr(L"Заняты: ", L"Taken: ") + g_failedHotkeys);
        break;
    case C_EditLast:
        if (!g_lastPath.empty()) {
            auto img = std::make_shared<Image>();
            if (LoadImageFile(g_lastPath, *img)) OpenEditor(img, g_lastPath);
        }
        break;
    case C_Quit: DestroyWindow(g_main); break;
    case C_ShowMain: ShowMainWindow(); break;
    case C_ToggleAutostart: SetAutostart(!AutostartEnabled()); break;
    case C_ImportShareX: ImportShareXNow(); break;
    case C_LangRu:
    case C_LangEn:
        SetEnglish(cmd == C_LangEn);
        MainWindowRefresh();
        break;
    }
}

void ShowMenu() {
    HMENU m = CreatePopupMenu();
    auto item = [&](int cmd, const wchar_t* ru, const wchar_t* en) {
        AppendMenuW(m, MF_STRING, cmd, (Tr(ru, en) + HotkeyFor(cmd)).c_str());
    };
    item(C_Region, L"Захват области", L"Capture region");
    item(C_Full, L"Захват всего экрана", L"Capture entire screen");
    item(C_Window, L"Захват активного окна", L"Capture active window");
    item(C_ActiveMonitor, L"Захват активного монитора", L"Capture active monitor");
    RECT lastRegion;
    if (LoadLastRegion(lastRegion)) item(C_LastRegion, L"Захват последней области", L"Capture last region");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (RecorderActive()) {
        AppendMenuW(m, MF_STRING, kTrayPause, RecorderPaused() ? Tr(L"Продолжить запись", L"Resume") : Tr(L"Пауза", L"Pause"));
        AppendMenuW(m, MF_STRING, C_Video, Tr(L"Остановить запись", L"Stop recording"));
        AppendMenuW(m, MF_STRING, kTrayAbort, Tr(L"Прервать запись", L"Abort"));
    } else {
        item(C_Video, L"Запись экрана", L"Screen recording");
        item(C_Gif, L"Запись экрана (GIF)", L"Screen recording (GIF)");
    }
    g_audioMenu = CreatePopupMenu();  // filled on WM_INITMENUPOPUP: listing devices runs ffmpeg
    AppendMenuW(g_audioMenu, MF_STRING | MF_GRAYED, 0, Tr(L"Поиск устройств...", L"Searching devices..."));
    AppendMenuW(m, MF_POPUP, (UINT_PTR)g_audioMenu, Tr(L"Источник звука", L"Audio source"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_lastPath.empty() ? MF_GRAYED : 0), C_EditLast,
                Tr(L"Редактировать последний", L"Edit last screenshot"));
    AppendMenuW(m, MF_STRING, C_Folder, Tr(L"Папка со скриншотами...", L"Screenshots folder..."));
    AppendMenuW(m, MF_STRING, C_Settings, Tr(L"Настройки (zipshot.ini)", L"Settings (zipshot.ini)"));
    AppendMenuW(m, MF_STRING, C_ImportShareX, Tr(L"Импорт настроек из ShareX", L"Import ShareX settings"));
    HMENU lang = CreatePopupMenu();
    AppendMenuW(lang, MF_STRING | (IsEnglish() ? 0 : MF_CHECKED), C_LangRu, L"Русский");
    AppendMenuW(lang, MF_STRING | (IsEnglish() ? MF_CHECKED : 0), C_LangEn, L"English");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)lang, L"Язык / Language");
    AppendMenuW(m, MF_STRING, C_Rehook, Tr(L"Перехватить хоткеи заново", L"Re-register hotkeys"));
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), C_ToggleAutostart,
                Tr(L"Запускать при входе в Windows", L"Run at Windows startup"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, C_ShowMain, Tr(L"Открыть главное окно", L"Open main window"));
    AppendMenuW(m, MF_STRING, C_Quit, Tr(L"Выход", L"Exit"));
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_main);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_main, nullptr);
    DestroyMenu(m);
    g_audioMenu = nullptr;
    PostMessageW(g_main, WM_NULL, 0, 0);
    if (cmd == kTrayPause) RecorderPauseResume();
    else if (cmd == kTrayAbort) RecorderAbort();
    else if (cmd >= kAudioNone && cmd <= kAudioNone + (int)g_audioDevices.size()) {
        EnsureUnicodeIni();  // device names are often not ASCII
        WritePrivateProfileStringW(L"record", L"AudioSource", cmd == kAudioNone ? L"" : g_audioDevices[cmd - kAudioNone - 1].c_str(),
                                   ZipshotIni().c_str());
    } else if (cmd) Run(cmd);
}

// "Audio source" submenu (ShareX FFmpegOptions.AudioSource): none + the dshow audio devices, current one checked.
void FillAudioMenu(HMENU menu) {
    static DWORD listedAt = 0;
    if (g_audioDevices.empty() || GetTickCount() - listedAt > 60000) {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        g_audioDevices = ListAudioDevices();
        listedAt = GetTickCount();
        SetCursor(old);
    }
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
    std::wstring cur = IniString(L"record", L"AudioSource", L"");
    AppendMenuW(menu, MF_STRING | (cur.empty() ? MF_CHECKED : 0), kAudioNone, Tr(L"Нет", L"None"));
    for (size_t i = 0; i < g_audioDevices.size() && i < 200; ++i)
        AppendMenuW(menu, MF_STRING | (cur == g_audioDevices[i] ? MF_CHECKED : 0), kAudioNone + 1 + (int)i, g_audioDevices[i].c_str());
    if (!cur.empty() && std::find(g_audioDevices.begin(), g_audioDevices.end(), cur) == g_audioDevices.end())
        AppendMenuW(menu, MF_STRING | MF_CHECKED | MF_GRAYED, 0, (cur + Tr(L" (не найдено)", L" (not found)")).c_str());
    if (g_audioDevices.empty())
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Tr(L"Устройства DirectShow не найдены", L"No DirectShow audio devices"));
}

int ParseCmd(const std::wstring& s) {
    if (s == L"--full") return C_Full;
    if (s == L"--region") return C_Region;
    if (s == L"--window") return C_Window;
    if (s == L"--video") return C_Video;
    if (s == L"--gif") return C_Gif;
    if (s == L"--monitor") return C_ActiveMonitor;
    if (s == L"--last-region") return C_LastRegion;
    if (s == L"--import-sharex") return C_ImportShareX;
    if (s == L"--quit") return C_Quit;
    if (s == L"--show-main") return C_ShowMain;
    return 0;
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HOTKEY:
        if (w >= 1 && w <= g_bound.size()) Run(g_bound[w - 1].cmd);
        return 0;
    case WM_TRAY:
        // Single click = region capture, but only once the double-click time has passed without a second
        // click; a double click opens the main window and takes no screenshot.
        if (LOWORD(l) == WM_LBUTTONUP) {
            if (!g_trayDouble) SetTimer(h, kTrayClickTimer, GetDoubleClickTime(), nullptr);
            g_trayDouble = false;
        } else if (LOWORD(l) == WM_LBUTTONDBLCLK) {
            KillTimer(h, kTrayClickTimer);
            g_trayDouble = true;  // swallow the button-up that follows
            Run(C_ShowMain);
        } else if (LOWORD(l) == WM_RBUTTONUP) {
            ShowMenu();
        }
        return 0;
    case WM_TIMER:
        if (w == kTrayClickTimer) {
            KillTimer(h, kTrayClickTimer);
            Run(C_Region);
            return 0;
        }
        break;
    case WM_INITMENUPOPUP:
        if (g_audioMenu && (HMENU)w == g_audioMenu) {
            FillAudioMenu(g_audioMenu);
            return 0;
        }
        break;
    case WM_RUN: {
        auto* fn = (std::function<void()>*)l;
        (*fn)();
        delete fn;
        return 0;
    }
    case WM_COPYDATA: {
        auto* cd = (COPYDATASTRUCT*)l;
        // WM_COPYDATA is public IPC: reject malformed/non-wide payloads before reading them.
        if (!cd || !cd->lpData || !cd->cbData || cd->cbData > 128 * 1024 || cd->cbData % sizeof(wchar_t)) return FALSE;
        std::wstring s((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        if (!s.empty() && s.back() == L'\0') s.pop_back();
        if (s.find(L'\0') != std::wstring::npos) return FALSE;
        if (s.rfind(L"--edit ", 0) == 0) {
            std::wstring file = s.substr(7);
            RunOnMain([file] {
                auto img = std::make_shared<Image>();
                if (LoadImageFile(file, *img)) OpenEditor(img, file);
            });
        } else if (int c = ParseCmd(s)) {
            RunOnMain([c] { Run(c); });  // return from SendMessage first
        } else return FALSE;
        return TRUE;
    }
    case WM_DESTROY:
        if (RecorderActive()) RecorderStop();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (m == g_taskbarCreated && g_taskbarCreated) {
        AddTray();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int MessageLoop() {
    MSG msg{};
    BOOL result;
    while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return result < 0 ? 1 : (int)msg.wParam;
}

// Headless self-tests used during development; they never touch hotkeys or the tray.
int RunTest(int argc, wchar_t** argv) {
    std::wstring t = argv[1];
    if (t == L"--test-full" && argc >= 3) {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        Image img = CaptureRect(VirtualScreen(), true);
        QueryPerformanceCounter(&b);
        Log(L"test-full: cold grab %.1f ms", (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart);
        QueryPerformanceCounter(&a);
        img = CaptureRect(VirtualScreen(), true);
        QueryPerformanceCounter(&b);
        double grab = (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
        std::wstring path = argv[2];
        bool ok = SaveImageAuto(img, path);
        QueryPerformanceCounter(&a);
        Log(L"test-full: %dx%d grab %.1f ms, grab+encode %.1f ms -> %s", img.w, img.h, grab,
            (a.QuadPart - b.QuadPart) * 1000.0 / f.QuadPart + grab, path.c_str());
        return ok ? 0 : 1;
    }
    if (t == L"--test-overlay" && argc >= 3) {
        RECT vs = VirtualScreen();
        Image img = CaptureRect(vs, false);
        POINT cur{-vs.left + 900, 500};
        RECT sel{};
        if (argc >= 4 && std::wstring(argv[3]) == L"drag") sel = {-vs.left + 400, 200, -vs.left + 900, 500};
        return DumpOverlayTest(img, vs, cur, sel, argv[2]) ? 0 : 1;
    }
    if (t == L"--test-editor" && argc >= 4) return EditorTest(argv[2], argv[3]) ? 0 : 1;
    if (t == L"--test-demo" && argc >= 4) return EditorDemoTest(argv[2], argv[3]) ? 0 : 1;
    if (t == L"--test-region" && argc >= 3) return RegionWindowTest(argv[2]) ? 0 : 1;
    if (t == L"--test-stickers") {  // opens and closes the real sticker picker off-screen (the crash path)
        SetEnvironmentVariableW(L"ZIPSHOT_TEST_STICKERS", L"1");
        std::wstring path;
        int size = 64;
        bool ok = shapes_ext::PickSticker(g_main, {0, 0}, 1.5f, path, size);
        Log(L"test-stickers: %s %s", ok ? L"picked" : L"none", path.c_str());
        return ok ? 0 : 1;
    }
    if (t == L"--test-gif-export" && argc >= 4) return GifExportTest(argv[2], argv[3]) ? 0 : 1;
    if (t == L"--test-picker" && argc >= 3) return ColorPickerTest(argv[2]) ? 0 : 1;
    if (t == L"--test-editor-window" && argc >= 4) return EditorWindowTest(argv[2], argv[3]) ? 0 : 1;
    if (t == L"--test-imageops" && argc >= 4) return ImageOpsTest(argv[2], argv[3]) ? 0 : 1;
    if (t == L"--test-record" && argc >= 8) {
        // --test-record gif|video seconds x y w h
        bool gif = std::wstring(argv[2]) == L"gif";
        int secs = _wtoi(argv[3]);
        RECT r{_wtoi(argv[4]), _wtoi(argv[5]), _wtoi(argv[4]) + _wtoi(argv[6]), _wtoi(argv[5]) + _wtoi(argv[7])};
        // Optional 8th argument "pause": pause after a third of the time, resume after two thirds (segments + concat).
        bool pause = argc >= 9 && std::wstring(argv[8]) == L"pause";
        RecorderStart(r, gif);
        if (!RecorderActive()) return 1;
        // ZIPSHOT_TEST_PANEL=<png>: PrintWindow the control panel (it is excluded from screen capture) while recording/paused.
        static wchar_t panelPng[MAX_PATH] = {};
        GetEnvironmentVariableW(L"ZIPSHOT_TEST_PANEL", panelPng, MAX_PATH);
        auto dumpPanel = [](const wchar_t* suffix) {
            if (!*panelPng) return;
            std::wstring out = std::wstring(panelPng) + suffix + L".png";
            RunOnMain([out] {
                HWND p = FindWindowW(L"ZipshotRecPanel", nullptr);
                RECT r;
                if (!p || !GetWindowRect(p, &r)) return;
                Image img;
                img.w = r.right - r.left;
                img.h = r.bottom - r.top;
                void* bits;
                HBITMAP bmp = CreateDib(img.w, img.h, &bits);
                HDC dc = CreateCompatibleDC(nullptr);
                HGDIOBJ old = SelectObject(dc, bmp);
                SendMessageW(p, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT);
                GdiFlush();
                img.px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)img.w * img.h);
                for (auto& px : img.px) px |= 0xFF000000u;
                SelectObject(dc, old);
                DeleteDC(dc);
                DeleteObject(bmp);
                EncodeImage(img, out, false);
            });
        };
        std::thread([secs, pause, dumpPanel] {
            if (pause) {
                Sleep(secs * 1000 / 3);
                dumpPanel(L"_rec");
                RunOnMain([] { RecorderPauseResume(); });
                Sleep(secs * 1000 / 3);
                dumpPanel(L"_paused");
                RunOnMain([] { RecorderPauseResume(); });
                Sleep(secs * 1000 - 2 * (secs * 1000 / 3));
            } else {
                Sleep(secs * 1000);
            }
            RunOnMain([] { RecorderStop(); });
            for (;;) {
                Sleep(200);
                bool done = false;
                HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                RunOnMain([&done, ev] { done = !RecorderActive() && !RecorderBusy(); SetEvent(ev); });
                WaitForSingleObject(ev, INFINITE);
                CloseHandle(ev);
                if (done) { RunOnMain([] { PostQuitMessage(0); }); return; }
            }
        }).detach();
        MessageLoop();
        Log(L"test-record done");
        return 0;
    }
    if (t == L"--test-hotkey-path" && argc >= 2) {
        // Same code path as Ctrl/Alt/PrtScn without the overlay: full + window.
        g_testMode = true;
        RunOnMain([] { CaptureFull(); });
        return MessageLoop();
    }
    return -1;
}

}  // namespace

void RunCommand(int cmd) { Run(cmd); }

bool LoadLastRegion(RECT& r) {
    int x, y, w, h;
    if (swscanf_s(IniString(L"region", L"LastRegion", L"").c_str(), L"%d , %d , %d , %d", &x, &y, &w, &h) != 4 || w < 1 || h < 1)
        return false;
    r = {x, y, x + w, y + h};
    return true;
}

void SaveLastRegion(const RECT& r) {
    wchar_t b[96];
    swprintf_s(b, L"%d,%d,%d,%d", r.left, r.top, r.right - r.left, r.bottom - r.top);
    WritePrivateProfileStringW(L"region", L"LastRegion", b, ZipshotIni().c_str());
}

std::vector<HotkeyInfo> HotkeyStatus() {
    std::vector<HotkeyInfo> v;
    for (const Bound& b : g_bound) v.push_back({HotkeyDisplayText(b.mods, b.vk), Tr(b.job->ru, b.job->en), b.ok});
    return v;
}

void RunOnMain(std::function<void()> fn) {
    PostMessageW(g_main, WM_RUN, 0, (LPARAM) new std::function<void()>(std::move(fn)));
}

void TrimMemory() { SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1); }

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, fmt, ap);
    va_end(ap);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(tmp) + L"zipshot.log").c_str(), L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring arg1 = argc > 1 ? argv[1] : L"";
    bool test = arg1.rfind(L"--test-", 0) == 0;

    if (!test) {
        HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\ZipshotSingleton");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            // Forward the command to the running instance.
            if (HWND other = FindWindowW(kMainClass, nullptr)) {
                std::wstring msg = arg1;
                if (arg1 == L"--edit" && argc > 2) msg += L" " + std::wstring(argv[2]);
                if (!msg.empty()) {
                    COPYDATASTRUCT cd{0, (DWORD)(msg.size() * sizeof(wchar_t)), msg.data()};
                    AllowSetForegroundWindow(ASFW_ANY);
                    DWORD_PTR result = 0;
                    LRESULT sent = SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result);
                    if (!sent || !result) {
                        Log(L"command forwarding failed or was rejected");
                        LocalFree(argv);
                        return 1;
                    }
                }
            }
            return 0;
        }
        (void)mutex;
    }

    OleInitialize(nullptr);  // STA + OLE drag and drop (toast, main window)
    // First start after ShareX: take over its settings once.
    std::wstring importReport;
    bool imported = false;
    if (!test && !GetSettingBool(L"ShareXImported", false)) {
        imported = ImportShareXSettings(importReport);
        if (imported) SetSettingBool(L"ShareXImported", true);
    }
    LoadSettings();
    g_icon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON));
    if (!g_icon) g_icon = MakeIcon(GetSystemMetrics(SM_CXSMICON));

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    RegisterClassW(&wc);
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, L"Zipshot", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    if (test) {
        int rc = RunTest(argc, argv);
        DestroyWindow(g_main);
        LocalFree(argv);
        OleUninitialize();
        return rc;
    }

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_main, g_taskbarCreated, MSGFLT_ALLOW, nullptr);
    AddTray();
    RegisterHotkeys();
    if (!g_failedHotkeys.empty()) Balloon(BusyHotkeysText());
    else if (imported) Balloon(Tr(L"Настройки ShareX импортированы в zipshot.ini", L"ShareX settings imported into zipshot.ini"), NIIF_INFO);
    if (imported) Log(L"%s", importReport.substr(0, 1800).c_str());
    if (arg1 == L"--edit" && argc > 2) {
        std::wstring file = argv[2];
        RunOnMain([file] {
            auto img = std::make_shared<Image>();
            if (LoadImageFile(file, *img)) OpenEditor(img, file);
        });
    } else if (int c = ParseCmd(arg1)) {
        std::wstring ready = c == C_ShowMain && argc > 3 && std::wstring(argv[2]) == L"--ready-file" ? argv[3] : L"";
        RunOnMain([c, ready] {
            Run(c);
            if (!ready.empty() && !MainWindowWriteReady(ready)) Log(L"main window readiness report failed");
        });
    }
    LocalFree(argv);
    // Warm up the GPU capture path so the first hotkey is as fast as the rest.
    RunOnMain([] {
        CaptureRect({0, 0, 1, 1}, false);
        TrimMemory();
    });
    return MessageLoop();
}
