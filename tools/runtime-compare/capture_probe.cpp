// Runs ONLY in the disposable Windows Sandbox guest. All input and pixels stay there.
#include "../../src/capture.cpp"
#include <cstdio>
#include <cstdarg>
#include <string>

static std::wstring out;
static HWND fixture;
static int width, height, received = 0, phase = 0;
static LARGE_INTEGER frequency{}, go{}, sent[2]{}, seen[2]{};
static UINT injected[2]{};
static bool baseline_ok = false;

void Log(const wchar_t* fmt, ...) {
    if (out.empty()) return;
    FILE* f = nullptr;
    _wfopen_s(&f, (out + L"\\capture_native.log").c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfwprintf(f, fmt, ap); va_end(ap);
    fwprintf(f, L"\n"); fclose(f);
}

static bool save_bmp(const Image& image, const wchar_t* name) {
    if (image.empty()) return false;
    FILE* f = nullptr;
    _wfopen_s(&f, (out + L"\\" + name).c_str(), L"wb");
    if (!f) return false;
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + (DWORD)(image.px.size() * 4);
    info.biSize = sizeof(info); info.biWidth = image.w; info.biHeight = -image.h;
    info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
    bool ok = fwrite(&file, sizeof(file), 1, f) == 1 && fwrite(&info, sizeof(info), 1, f) == 1 &&
              fwrite(image.px.data(), 4, image.px.size(), f) == image.px.size();
    fclose(f); return ok;
}

static void write_result() {
    FILE* f = nullptr;
    _wfopen_s(&f, (out + L"\\capture_native_result.json").c_str(), L"w");
    if (!f) return;
    wchar_t username[128]{}; DWORD n = 128; GetUserNameW(username, &n);
    DWORD session = 0; ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const bool ok = baseline_ok && received == 2 && injected[0] == 2 && injected[1] == 4;
    fprintf(f, "{\"schema_version\":1,\"status\":\"%s\",\"guest_username\":\"%ls\",\"guest_pid\":%lu,\"guest_session_id\":%lu,\"remote_session\":%s,\"width\":%d,\"height\":%d,\"colour_fixture_verified\":%s,\"hotkeys_received\":%d,\"inputs\":[",
            ok ? "pass" : "fail", username, GetCurrentProcessId(), session,
            GetSystemMetrics(SM_REMOTESESSION) ? "true" : "false", width, height, baseline_ok ? "true" : "false", received);
    for (int i = 0; i < 2; ++i)
        fprintf(f, "%s{\"key\":\"%s\",\"method\":\"guest SendInput\",\"events_inserted\":%u,\"send_qpc\":%lld,\"hotkey_received_qpc\":%lld,\"dispatch_ms\":%.6f}",
                i ? "," : "", i ? "Ctrl+PrintScreen" : "PrintScreen", injected[i], sent[i].QuadPart, seen[i].QuadPart,
                seen[i].QuadPart ? (seen[i].QuadPart - sent[i].QuadPart) * 1000.0 / frequency.QuadPart : -1.0);
    fprintf(f, "],\"qpc_frequency\":%lld,\"scope\":\"Isolation capability probe, not a Zipshot versus ShareX result\"}\n", frequency.QuadPart);
    fclose(f);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        RECT client{}; GetClientRect(hwnd, &client);
        HBRUSH bg = CreateSolidBrush(RGB(24, 26, 30)); FillRect(dc, &client, bg); DeleteObject(bg);
        const COLORREF colours[] = {RGB(255,0,0), RGB(0,255,0), RGB(0,0,255), RGB(255,255,255)};
        for (int i = 0; i < 4; ++i) {
            RECT stripe{i * width / 4 + 12, 100, (i + 1) * width / 4 - 12, height - 100};
            HBRUSH brush = CreateSolidBrush(colours[i]); FillRect(dc, &stripe, brush); DeleteObject(brush);
        }
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(240,242,246));
        HFONT font = CreateFontW(-32, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto old = SelectObject(dc, font);
        RECT title{24, 20, width-24, 90};
        DrawTextW(dc, L"Zipshot / ShareX isolated Windows runtime lab", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT marker{0, height-80, width, height};
        HBRUSH marker_brush = CreateSolidBrush(received == 0 ? RGB(32,35,40) : received == 1 ? RGB(255,220,0) : RGB(255,0,255));
        FillRect(dc, &marker, marker_brush); DeleteObject(marker_brush);
        SetTextColor(dc, received ? RGB(0,0,0) : RGB(240,242,246));
        DrawTextW(dc, received == 0 ? L"Waiting for guest keyboard input" : received == 1 ? L"PrintScreen received" : L"Ctrl+PrintScreen received", -1, &marker, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old); DeleteObject(font); EndPaint(hwnd, &ps); return 0;
    }
    if (message == WM_HOTKEY && (wp == 1 || wp == 2)) {
        const int index = (int)wp - 1;
        QueryPerformanceCounter(&seen[index]);
        ++received; InvalidateRect(hwnd, nullptr, FALSE); UpdateWindow(hwnd); return 0;
    }
    if (message == WM_TIMER) {
        if (!go.QuadPart) {
            if (GetFileAttributesW((out + L"\\capture_go.flag").c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
            QueryPerformanceCounter(&go);
        }
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        double ms = (now.QuadPart-go.QuadPart)*1000.0/frequency.QuadPart;
        if ((phase == 0 && ms >= 300) || (phase == 1 && ms >= 1200)) {
            if (GetForegroundWindow() != hwnd) { Log(L"Guest foreground fixture lost; refusing input"); write_result(); DestroyWindow(hwnd); return 0; }
            INPUT keys[4]{};
            UINT count = phase ? 4 : 2;
            for (auto& key : keys) key.type = INPUT_KEYBOARD;
            if (!phase) { keys[0].ki.wVk = keys[1].ki.wVk = VK_SNAPSHOT; keys[1].ki.dwFlags = KEYEVENTF_KEYUP; }
            else {
                keys[0].ki.wVk = keys[3].ki.wVk = VK_CONTROL;
                keys[1].ki.wVk = keys[2].ki.wVk = VK_SNAPSHOT;
                keys[2].ki.dwFlags = keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
            }
            QueryPerformanceCounter(&sent[phase]); injected[phase] = SendInput(count, keys, sizeof(INPUT)); ++phase;
        }
        if (ms >= 2300) {
            save_bmp(CaptureRect({0,0,width,height}, false), L"capture_after_input.bmp");
            write_result(); DestroyWindow(hwnd);
        }
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    // This guard runs before creating any window, capturing pixels or inserting input.
    wchar_t user[128]{}; DWORD size = 128;
    if (!GetUserNameW(user, &size) || _wcsicmp(user, L"WDAGUtilityAccount")) return 20;
    out = L"C:\\ZipshotLab\\output";
    if (GetFileAttributesW(out.c_str()) == INVALID_FILE_ATTRIBUTES) return 21;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&frequency);
    Sleep(2000); // Let the host's owned client-window resize finish its RDP display update.
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
        mode.dmPelsWidth = 1280; mode.dmPelsHeight = 720;
        mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        Log(L"Guest-only ChangeDisplaySettings 1280x720 result=%ld", ChangeDisplaySettingsW(&mode, 0));
    }
    width = GetSystemMetrics(SM_CXSCREEN); height = GetSystemMetrics(SM_CYSCREEN);
    WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = instance; wc.lpszClassName = L"ZipshotIsolatedCaptureProbe";
    RegisterClassW(&wc);
    fixture = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Isolated runtime colour fixture", WS_POPUP,
                              0, 0, width, height, nullptr, nullptr, instance, nullptr);
    if (!fixture) return 22;
    ShowWindow(fixture, SW_SHOW); SetWindowPos(fixture, HWND_TOPMOST, 0,0,width,height,SWP_SHOWWINDOW);
    SetForegroundWindow(fixture); UpdateWindow(fixture); DwmFlush(); Sleep(200);
    auto image = CaptureRect({0,0,width,height}, false);
    const uint32_t expected[] = {0xff0000,0x00ff00,0x0000ff,0xffffff};
    baseline_ok = image.w == width && image.h == height;
    if (baseline_ok) for (int i=0;i<4;++i)
        baseline_ok = baseline_ok && ((image.px[(size_t)(height/2)*width + (i*2+1)*width/8] & 0xffffff) == expected[i]);
    save_bmp(image, L"capture_fixture.bmp");
    if (!baseline_ok || !RegisterHotKey(fixture, 1, MOD_NOREPEAT, VK_SNAPSHOT) ||
        !RegisterHotKey(fixture, 2, MOD_CONTROL | MOD_NOREPEAT, VK_SNAPSHOT)) { write_result(); return 23; }
    FILE* ready = nullptr; _wfopen_s(&ready, (out + L"\\capture_native_ready.flag").c_str(), L"w"); if (ready) { fputs("ready",ready); fclose(ready); }
    SetTimer(fixture, 1, 20, nullptr);
    MSG message{}; while (GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return baseline_ok && received == 2 ? 0 : 24;
}
