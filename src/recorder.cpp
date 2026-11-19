#include "common.h"
#include "capture_ext.h"
#include "sharex_import.h"
#include "cursor_video.h"
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <initializer_list>
#include <thread>

// Screen recording through ffmpeg. Video: ddagrab (Desktop Duplication on the GPU) straight
// into NVENC. GIF: lossless intermediate, then the classic palettegen/paletteuse two-pass.
// ffmpeg only exists while recording; nothing runs when idle.
//
// Pause/resume (ShareX ScreenRecordForm): ffmpeg cannot pause a capture, so every stretch between
// start/resume and pause/stop is its own ffmpeg run into a numbered temp .mkv segment (Matroska
// survives an abrupt end). On stop the segments are joined with the concat demuxer: -c copy into the
// .mp4 (no re-encode), or fed straight into the GIF palette passes. Paused time is never recorded,
// and the panel timer adds up only the recorded stretches.

using Microsoft::WRL::ComPtr;

namespace {

// zipshot.ini [record], imported from ShareX (ScreenRecordFPS, GIFFPS, FFmpegOptions, ScreenRecordStartDelay,
// ScreenRecordAutoStart). Values that end up on the ffmpeg command line are whitelisted.
struct RecCfg {
    int videoFps = 30, gifFps = 15, crf = 23, nvencCq = 23, bayerScale = 2;
    bool cursor = true;
    bool followCursor = false;
    std::wstring preset = L"ultrafast", dither = L"sierra2_4a", stats = L"full";
    std::wstring audio;               // dshow device name, empty = no audio (ShareX AudioSource)
    std::wstring audioCodec = L"aac"; // aac | opus | mp3
    int audioBitrate = 128;           // kbps
    int startDelayMs = 0;             // ShareX ScreenRecordStartDelay (seconds in the ini)
    bool autoStart = true;            // ShareX ScreenRecordAutoStart: false = show the panel with Start first
};

std::wstring Pick(const std::wstring& v, std::initializer_list<const wchar_t*> allowed, const wchar_t* def) {
    for (auto a : allowed)
        if (_wcsicmp(v.c_str(), a) == 0) return a;
    return def;
}

RecCfg LoadRecCfg() {
    RecCfg c;
    c.videoFps = std::clamp(IniInt(L"record", L"VideoFps", c.videoFps), 1, 120);
    c.gifFps = std::clamp(IniInt(L"record", L"GifFps", c.gifFps), 1, 60);
    c.crf = std::clamp(IniInt(L"record", L"X264Crf", c.crf), 0, 51);
    c.nvencCq = std::clamp(IniInt(L"record", L"NvencCq", c.nvencCq), 0, 51);
    c.bayerScale = std::clamp(IniInt(L"record", L"GifBayerScale", c.bayerScale), 0, 5);
    c.cursor = IniInt(L"record", L"ShowCursor", 1) != 0;
    c.followCursor = IniInt(L"record", L"FollowCursor", 0) != 0;
    c.preset = Pick(IniString(L"record", L"X264Preset", L""),
                    {L"ultrafast", L"superfast", L"veryfast", L"faster", L"fast", L"medium", L"slow", L"slower", L"veryslow", L"placebo"},
                    L"ultrafast");
    c.dither = Pick(IniString(L"record", L"GifDither", L""),
                    {L"none", L"bayer", L"heckbert", L"floyd_steinberg", L"sierra2", L"sierra2_4a", L"sierra3", L"burkes", L"atkinson"},
                    L"sierra2_4a");
    c.stats = Pick(IniString(L"record", L"GifStatsMode", L""), {L"full", L"diff", L"single"}, L"full");
    c.audio = IniString(L"record", L"AudioSource", L"");
    c.audio.erase(std::remove(c.audio.begin(), c.audio.end(), L'"'), c.audio.end());
    c.audioCodec = Pick(IniString(L"record", L"AudioCodec", L""), {L"aac", L"opus", L"mp3"}, L"aac");
    c.audioBitrate = std::clamp(IniInt(L"record", L"AudioBitrate", c.audioBitrate), 32, 512);
    c.startDelayMs = std::clamp((int)(_wtof(IniString(L"record", L"StartDelay", L"0").c_str()) * 1000 + .5), 0, 600000);
    c.autoStart = IniInt(L"record", L"AutoStart", 1) != 0;
    return c;
}

enum RState { RS_Waiting, RS_Countdown, RS_Recording, RS_Paused };

struct Proc {
    PROCESS_INFORMATION pi{};
    HANDLE in = nullptr;
};

struct Rec {
    bool active = false;  // panel shown (waiting, countdown, recording or paused)
    bool busy = false;    // joining/converting after stop
    bool gif = false;
    bool aborted = false;
    RState state = RS_Waiting;
    RECT r{};
    Proc cur;                       // ffmpeg writing the current segment
    std::vector<Proc> closing;      // segments told to finish (pause), joined on stop
    std::vector<std::wstring> segs; // segment files in order
    std::wstring out, ffmpeg, base;
    std::vector<std::wstring> variants;  // command lines to try in order; %OUT% = segment file
    size_t variant = 0;
    DWORD segStart = 0, recordedMs = 0, countdownEnd = 0;
    int shownCount = -1;
    HWND frame = nullptr, panel = nullptr;
    bool followEnabled = false, followHotkey = false, cursorLimitReached = false;
    std::vector<cursor_follow::Sample> cursorSamples;
    size_t segmentSampleStart = 0;
    DWORD lastCursorTick = 0;
    RecCfg cfg;
} g;

std::wstring TempDir() {
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    return buf;
}

std::wstring LogPath() { return TempDir() + L"zipshot_ffmpeg.log"; }

bool Spawn(const std::wstring& cmdline, PROCESS_INFORMATION& pi, HANDLE* stdinWrite) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr;
    if (stdinWrite) {
        if (!CreatePipe(&inRead, &inWrite, &sa, 0)) return false;
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE log = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinWrite ? inRead : nul;
    si.hStdOutput = nul;
    si.hStdError = log;
    std::wstring cmd = cmdline;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                             nullptr, nullptr, &si, &pi);
    if (inRead) CloseHandle(inRead);
    CloseHandle(log);
    CloseHandle(nul);
    if (!ok) {
        if (inWrite) CloseHandle(inWrite);
        return false;
    }
    if (stdinWrite) *stdinWrite = inWrite;
    return true;
}

bool RunAndWait(const std::wstring& cmdline) {
    PROCESS_INFORMATION pi{};
    if (!Spawn(cmdline, pi, nullptr)) return false;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

// Index of the monitor among adapter 0's outputs, as ddagrab counts them; -1 if it is elsewhere.
int DdaOutputIndex(const RECT& mon) {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return -1;
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(f->EnumAdapters1(0, &a))) return -1;
    ComPtr<IDXGIOutput> o;
    for (UINT i = 0; a->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_OUTPUT_DESC d;
        o->GetDesc(&d);
        if (EqualRect(&d.DesktopCoordinates, &mon)) return (int)i;
        o.Reset();
    }
    return -1;
}

std::wstring Q(const std::wstring& s) { return L"\"" + s + L"\""; }

std::wstring WithTarget(std::wstring cmd, const std::wstring& file) {
    size_t p = cmd.find(L"%OUT%");
    if (p != std::wstring::npos) cmd.replace(p, 5, Q(file));
    return cmd;
}

// Encoders need even dimensions. Keep the minimum-size expansion inside the selected monitor,
// especially for tiny selections at its right/bottom edges; reject disjoint rectangles entirely.
bool NormalizeRecordingRect(RECT& r, const RECT& monitor) {
    if (monitor.right - monitor.left < 16 || monitor.bottom - monitor.top < 16 ||
        !IntersectRect(&r, &r, &monitor)) return false;
    LONG width = std::max<LONG>(16, (r.right - r.left) & ~1);
    LONG height = std::max<LONG>(16, (r.bottom - r.top) & ~1);
    r.left = std::min(r.left, monitor.right - width);
    r.top = std::min(r.top, monitor.bottom - height);
    r.right = r.left + width;
    r.bottom = r.top + height;
    return true;
}

void BuildVariants(RECT mon, int outIdx) {
    int w = g.r.right - g.r.left, h = g.r.bottom - g.r.top;
    const RecCfg& c = g.cfg;
    int fps = g.gif ? c.gifFps : c.videoFps;
    int mouse = c.cursor ? 1 : 0;
    wchar_t buf[512];
    std::wstring dda, gdi;
    if (outIdx >= 0) {
        swprintf_s(buf, L"-f lavfi -i ddagrab=output_idx=%d:framerate=%d:video_size=%dx%d:offset_x=%d:offset_y=%d:draw_mouse=%d",
                   outIdx, fps, w, h, g.r.left - mon.left, g.r.top - mon.top, mouse);
        dda = buf;
    }
    swprintf_s(buf, L"-f gdigrab -framerate %d -offset_x %d -offset_y %d -video_size %dx%d -draw_mouse %d -i desktop", fps,
               g.r.left, g.r.top, w, h, mouse);
    gdi = buf;
    std::wstring x264 = L" -c:v libx264 -preset " + c.preset + L" -crf " + std::to_wstring(c.crf);
    std::wstring nvenc = L" -c:v h264_nvenc -preset p4 -tune ll -rc vbr -cq " + std::to_wstring(c.nvencCq) + L" -b:v 0";
    std::wstring head = Q(g.ffmpeg) + L" -hide_banner -loglevel warning -y ";
    g.variants.clear();
    if (g.gif) {
        std::wstring enc = L" -c:v libx264rgb -preset ultrafast -qp 0 %OUT%";
        if (!dda.empty()) g.variants.push_back(head + dda + L" -vf hwdownload,format=bgra" + enc);
        g.variants.push_back(head + gdi + enc);
        return;
    }
    // ShareX FFmpegOptions.AudioSource: a dshow device as the second input, encoded next to the video.
    std::vector<std::pair<std::wstring, std::wstring>> audio;  // input args, encoder args
    if (!c.audio.empty()) {
        const wchar_t* enc = c.audioCodec == L"opus" ? L"libopus" : c.audioCodec == L"mp3" ? L"libmp3lame" : L"aac";
        audio.push_back({L" -f dshow -thread_queue_size 1024 -audio_buffer_size 80 -i audio=" + Q(c.audio),
                         std::wstring(L" -c:a ") + enc + L" -b:a " + std::to_wstring(c.audioBitrate) + L"k"});
    }
    audio.push_back({L"", L""});  // without audio: also the fallback when the device cannot be opened
    for (auto& a : audio) {
        if (!dda.empty()) {
            g.variants.push_back(head + dda + a.first + nvenc + a.second + L" %OUT%");
            g.variants.push_back(head + dda + a.first + L" -vf hwdownload,format=bgra,format=yuv420p" + x264 + a.second + L" %OUT%");
        }
        g.variants.push_back(head + gdi + a.first + L" -vf format=yuv420p" + x264 + a.second + L" %OUT%");
    }
}

// ---------- on-screen frame + control panel (excluded from capture) ----------

constexpr UINT_PTR kTimerTick = 1, kTimerCheck = 2, kTimerCursor = 3;
constexpr int kFollowHotkey = 0x5A46;
constexpr size_t kMaxCursorSamples = 2000000;

DWORD ElapsedAt(DWORD now) { return g.recordedMs + (g.state == RS_Recording ? now - g.segStart : 0); }
DWORD Elapsed() { return ElapsedAt(GetTickCount()); }

void RecordCursorSample(bool force = false) {
    // No background monitoring: waiting/countdown/paused/GIF never read cursor.
    if (!g.active || g.gif || g.state != RS_Recording) return;
    if (g.cursorLimitReached) return;
    const DWORD now = GetTickCount();
    if (!force && now - g.lastCursorTick < 15) return;
    g.lastCursorTick = now;
    cursor_follow::Sample sample;
    sample.t = ElapsedAt(now) / 1000.0;
    sample.enabled = g.followEnabled;
    if (g.cursorSamples.size() >= kMaxCursorSamples) {
        sample.enabled = sample.visible = false;
        g.cursorSamples.back() = sample;
        g.cursorLimitReached = true;
        g.followEnabled = false;
        return;
    }
    CURSORINFO cursor{sizeof(cursor)};
    const bool available = GetCursorInfo(&cursor) != FALSE;
    const int width = g.r.right - g.r.left, height = g.r.bottom - g.r.top;
    sample.visible = available && (cursor.flags & CURSOR_SHOWING) && width > 0 && height > 0 &&
                     cursor.ptScreenPos.x >= g.r.left && cursor.ptScreenPos.x < g.r.right &&
                     cursor.ptScreenPos.y >= g.r.top && cursor.ptScreenPos.y < g.r.bottom;
    if (sample.visible) {
        sample.x = double(cursor.ptScreenPos.x - g.r.left) / width;
        sample.y = double(cursor.ptScreenPos.y - g.r.top) / height;
    }
    // Keep segment boundaries separate so failed-start rollback cannot modify
    // a sample belonging to the previous successfully completed segment.
    if (g.cursorSamples.size() > g.segmentSampleStart && g.cursorSamples.back().t == sample.t)
        g.cursorSamples.back() = sample;
    else {
        try { g.cursorSamples.push_back(sample); }
        catch (const std::bad_alloc&) {
            sample.enabled = sample.visible = false;
            if (!g.cursorSamples.empty()) g.cursorSamples.back() = sample;
            g.cursorLimitReached = true;
            g.followEnabled = false;
        }
    }
}

void Redraw();
void ToggleFollow() {
    if (!g.active || g.gif || g.cursorLimitReached) return;
    g.followEnabled = !g.followEnabled;
    EnsureUnicodeIni();
    WritePrivateProfileStringW(L"record", L"FollowCursor", g.followEnabled ? L"1" : L"0", ZipshotIni().c_str());
    RecordCursorSample(true);
    Redraw();
}

// Border like ShareX's ScreenRecordForm: yellow while waiting/counting down/paused, red (GIF: orange) while recording.
// During the start delay the remaining seconds are shown big in the middle.
void DrawFrame() {
    if (!g.frame) return;
    RECT fr = g.r;
    InflateRect(&fr, 3, 3);
    int w = fr.right - fr.left, h = fr.bottom - fr.top;
    void* bits = nullptr;
    HBITMAP bmp = CreateDib(w, h, &bits);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return; }
    auto* p = (uint32_t*)bits;
    uint32_t color = g.state == RS_Recording ? (g.gif ? 0xFFFFB020u : 0xFFE53935u) : 0xFFF1C41Bu;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            bool edge = x < 2 || y < 2 || x >= w - 2 || y >= h - 2;
            p[(size_t)y * w + x] = edge ? color : 0;
        }
    HDC sdc = GetDC(nullptr);
    HDC mdc = CreateCompatibleDC(sdc);
    HGDIOBJ old = SelectObject(mdc, bmp);
    if (g.state == RS_Countdown && g.shownCount > 0) {
        std::wstring t = std::to_wstring(g.shownCount);
        int fh = std::clamp(std::min(w, h) / 3, 24, 160);
        HFONT f = CreateFontW(-fh, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(mdc, f);
        SIZE ts;
        GetTextExtentPoint32W(mdc, t.c_str(), (int)t.size(), &ts);
        int bw = std::min(w - 8, (int)ts.cx + fh), bh = std::min(h - 8, (int)ts.cy + fh / 4);
        RECT box{(w - bw) / 2, (h - bh) / 2, (w - bw) / 2 + bw, (h - bh) / 2 + bh};
        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 32));
        FillRect(mdc, &box, bg);
        DeleteObject(bg);
        SetBkMode(mdc, TRANSPARENT);
        SetTextColor(mdc, RGB(241, 196, 27));
        DrawTextW(mdc, t.c_str(), -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mdc, of);
        DeleteObject(f);
        GdiFlush();
        for (int y = std::max(0, (int)box.top); y < std::min(h, (int)box.bottom); ++y)  // GDI leaves alpha 0: make the box opaque
            for (int x = std::max(0, (int)box.left); x < std::min(w, (int)box.right); ++x) p[(size_t)y * w + x] |= 0xFF000000u;
    }
    POINT src{0, 0}, pos{fr.left, fr.top};
    SIZE sz{w, h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(g.frame, sdc, &pos, &sz, mdc, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mdc, old);
    DeleteDC(mdc);
    ReleaseDC(nullptr, sdc);
    DeleteObject(bmp);
}

enum PanelBtn { PB_Start, PB_Pause, PB_Stop, PB_Abort, PB_Follow };

std::vector<std::pair<PanelBtn, RECT>> PanelButtons() {
    std::vector<PanelBtn> kinds;
    switch (g.state) {
    case RS_Waiting: kinds = {PB_Start, PB_Abort}; break;
    case RS_Countdown: kinds = {PB_Abort}; break;
    default: kinds = {PB_Pause, PB_Stop, PB_Abort}; break;
    }
    if (!g.gif) kinds.insert(kinds.begin(), PB_Follow);
    RECT c;
    GetClientRect(g.panel, &c);
    const int weight = (int)kinds.size() * 3 + (g.gif ? 0 : 2);
    const int status = std::min(c.right / 3, c.bottom * 5);
    const int unit = std::max(1, std::min((int)c.bottom, ((int)c.right - status) / weight));
    int x = c.right;
    std::vector<std::pair<PanelBtn, RECT>> v(kinds.size());
    for (int i = (int)kinds.size() - 1; i >= 0; --i) {
        const int bw = unit * (kinds[i] == PB_Follow ? 5 : 3);
        v[i] = {kinds[i], {x - bw, 0, x, c.bottom}};
        x -= bw;
    }
    return v;
}

const wchar_t* PanelLabel(PanelBtn b) {
    switch (b) {
    case PB_Start: return Tr(L"Старт", L"Start");
    case PB_Pause: return g.state == RS_Paused ? Tr(L"Продолжить", L"Resume") : Tr(L"Пауза", L"Pause");
    case PB_Stop: return Tr(L"Стоп", L"Stop");
    case PB_Follow: return g.cursorLimitReached ? Tr(L"Слежение: лимит", L"Follow: limit") :
                          g.followEnabled ? Tr(L"Слежение ВКЛ", L"Follow ON") : Tr(L"Слежение ВЫКЛ", L"Follow OFF");
    default: return Tr(L"Прервать", L"Abort");
    }
}

// ShareX lblTimer: mm:ss:ff, counting down during the start delay.
std::wstring TimerText() {
    DWORD ms = g.state == RS_Countdown ? (DWORD)std::max<long>(0, (long)(g.countdownEnd - GetTickCount())) : Elapsed();
    wchar_t t[32];
    swprintf_s(t, L"%02u:%02u:%02u", ms / 60000, ms / 1000 % 60, ms / 10 % 100);
    return t;
}

void PanelClick(PanelBtn b);
void StartRecordingNow();

LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:  // tests: the panel is excluded from screen capture, so it is dumped this way
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = m == WM_PAINT ? BeginPaint(h, &ps) : (HDC)w;
        RECT c;
        GetClientRect(h, &c);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        HGDIOBJ ob = SelectObject(mdc, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 32));
        FillRect(mdc, &c, bg);
        DeleteObject(bg);
        HFONT f = CreateFontW(-(c.bottom * 45 / 100), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ of = SelectObject(mdc, f);
        SetBkMode(mdc, TRANSPARENT);
        auto buttons = PanelButtons();
        for (auto& b : buttons) {
            RECT r = b.second;
            InflateRect(&r, -2, -3);
            COLORREF fill = b.first == PB_Follow && g.followEnabled ? RGB(34, 103, 190) :
                            b.first == PB_Stop ? RGB(200, 45, 45) : b.first == PB_Start ? RGB(46, 140, 70) : RGB(58, 58, 64);
            HBRUSH br = CreateSolidBrush(fill);
            FillRect(mdc, &r, br);
            DeleteObject(br);
            SetTextColor(mdc, RGB(240, 240, 240));
            if (b.first == PB_Follow) {
                RECT label = b.second, hint = b.second;
                label.bottom = c.bottom * 62 / 100;
                hint.top = label.bottom - 1;
                DrawTextW(mdc, PanelLabel(b.first), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                HFONT hintFont = CreateFontW(-std::max(9L, c.bottom * 28 / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                          DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
                HGDIOBJ oldSmall = SelectObject(mdc, hintFont);
                const wchar_t* help = g.followHotkey ? L"Ctrl+Alt+F8" : Tr(L"Нажмите для переключения", L"Click to toggle");
                DrawTextW(mdc, help, -1, &hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(mdc, oldSmall);
                DeleteObject(hintFont);
            } else DrawTextW(mdc, PanelLabel(b.first), -1, &b.second, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        const wchar_t* status = g.state == RS_Waiting ? Tr(L"Ожидание", L"Waiting")
                              : g.state == RS_Countdown ? Tr(L"Старт через", L"Starting in")
                              : g.state == RS_Paused ? Tr(L"Пауза", L"Paused")
                              : (g.gif ? L"\x25CF GIF" : L"\x25CF REC");
        std::wstring t = std::wstring(L"  ") + status + L"  " + TimerText();
        SetTextColor(mdc, g.state == RS_Recording ? RGB(240, 240, 240) : RGB(241, 196, 27));
        RECT tr{0, 0, buttons.empty() ? c.right : buttons.front().second.left, c.bottom};
        DrawTextW(mdc, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mdc, of);
        DeleteObject(f);
        BitBlt(dc, 0, 0, c.right, c.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob);
        DeleteObject(bmp);
        DeleteDC(mdc);
        if (m == WM_PAINT) EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        for (auto& b : PanelButtons())
            if (PtInRect(&b.second, p)) {
                PanelClick(b.first);
                break;
            }
        return 0;
    }
    case WM_TIMER:
        if (w == kTimerCursor) {
            RecordCursorSample();
            return 0;
        }
        if (w == kTimerTick) {
            if (g.state == RS_Countdown) {
                long left = (long)(g.countdownEnd - GetTickCount());
                if (left <= 0) {
                    StartRecordingNow();
                    return 0;
                }
                int n = (int)((left + 999) / 1000);
                if (n != g.shownCount) {
                    g.shownCount = n;
                    DrawFrame();
                }
            }
            if (g.state != RS_Waiting && g.state != RS_Paused) InvalidateRect(h, nullptr, FALSE);
        }
        if (w == kTimerCheck) {
            KillTimer(h, kTimerCheck);
            // ffmpeg died at startup (e.g. no NVENC, audio device busy): fall through to the next variant.
            if (g.active && g.state == RS_Recording && g.cur.pi.hProcess && WaitForSingleObject(g.cur.pi.hProcess, 0) == WAIT_OBJECT_0) {
                CloseHandle(g.cur.pi.hProcess);
                CloseHandle(g.cur.pi.hThread);
                CloseHandle(g.cur.in);
                g.cur = {};
                const std::wstring& seg = g.segs.back();
                DeleteFileW(seg.c_str());
                while (++g.variant < g.variants.size()) {
                    Log(L"recorder: falling back to variant %zu", g.variant);
                    if (Spawn(WithTarget(g.variants[g.variant], seg), g.cur.pi, &g.cur.in)) break;
                }
                if (g.variant >= g.variants.size()) {
                    RecorderAbort();
                    MessageBoxW(nullptr, (Tr(L"ffmpeg не смог начать запись. Лог: ", L"ffmpeg could not start recording. Log: ") + LogPath()).c_str(),
                                L"Zipshot", MB_ICONERROR);
                    return 0;
                }
                g.segStart = GetTickCount();
                g.cursorSamples.resize(g.segmentSampleStart);
                g.cursorLimitReached = g.cursorSamples.size() >= kMaxCursorSamples;
                RecordCursorSample(true);
                SetTimer(h, kTimerCheck, 1500, nullptr);
            }
        }
        return 0;
    case WM_HOTKEY:
        if (w == kFollowHotkey) ToggleFollow();
        return 0;
    case WM_DESTROY:
        if (g.followHotkey) {
            UnregisterHotKey(h, kFollowHotkey);
            g.followHotkey = false;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void CreateUi() {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"ZipshotRecFrame";
        RegisterClassW(&wc);
        wc.lpfnWndProc = PanelProc;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = L"ZipshotRecPanel";
        RegisterClassW(&wc);
        reg = true;
    }
    g.frame = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              L"ZipshotRecFrame", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_inst, nullptr);
    SetWindowDisplayAffinity(g.frame, WDA_EXCLUDEFROMCAPTURE);
    DrawFrame();
    ShowWindow(g.frame, SW_SHOWNOACTIVATE);

    HMONITOR mon = MonitorFromRect(&g.r, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (auto fn = (GetDpiForMonitorFn)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")) {
        UINT dx, dy;
        if (SUCCEEDED(fn(mon, 0, &dx, &dy))) dpi = dx;
    }
    RECT work = mi.rcWork;
    if (work.right <= work.left || work.bottom <= work.top) work = mi.rcMonitor;
    const int pw = std::min<int>(MulDiv(g.gif ? 450 : 720, dpi, 96), std::max(1L, work.right - work.left));
    const int ph = std::min<int>(MulDiv(g.gif ? 30 : 36, dpi, 96), std::max(1L, work.bottom - work.top));
    int x = std::clamp<int>(g.r.right - pw, work.left, work.right - pw);
    int y = g.r.bottom + 6;
    if (y + ph > work.bottom) y = g.r.top - 6 - ph;
    y = std::clamp<int>(y, work.top, work.bottom - ph);
    g.panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"ZipshotRecPanel", L"", WS_POPUP, x, y, pw,
                              ph, nullptr, nullptr, g_inst, nullptr);
    SetWindowDisplayAffinity(g.panel, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(g.panel, SW_SHOWNOACTIVATE);
    if (g.panel) {
        SetTimer(g.panel, kTimerTick, 50, nullptr);
        if (!g.gif) {
            SetTimer(g.panel, kTimerCursor, 16, nullptr);
            g.followHotkey = RegisterHotKey(g.panel, kFollowHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F8) != FALSE;
        }
    }
}

void DestroyUi() {
    if (g.panel && g.followHotkey) {
        UnregisterHotKey(g.panel, kFollowHotkey);
        g.followHotkey = false;
    }
    if (g.frame) DestroyWindow(g.frame);
    if (g.panel) DestroyWindow(g.panel);
    g.frame = g.panel = nullptr;
}

void Redraw() {
    DrawFrame();
    if (g.panel) InvalidateRect(g.panel, nullptr, FALSE);
}

// Starts ffmpeg on a new segment; false if no command line works.
bool StartSegment() {
    std::wstring seg = g.base + std::to_wstring(g.segs.size()) + L".mkv";
    DeleteFileW(seg.c_str());
    for (; g.variant < g.variants.size(); ++g.variant)
        if (Spawn(WithTarget(g.variants[g.variant], seg), g.cur.pi, &g.cur.in)) break;
    if (g.variant >= g.variants.size()) return false;
    g.segs.push_back(seg);
    g.state = RS_Recording;
    g.segStart = GetTickCount();
    g.segmentSampleStart = g.cursorSamples.size();
    RecordCursorSample(true);
    if (g.panel) SetTimer(g.panel, kTimerCheck, 1500, nullptr);
    Redraw();
    return true;
}

// Asks the current ffmpeg to finish its segment ('q' on stdin); it is waited for on stop.
void EndSegment() {
    if (!g.cur.pi.hProcess) return;
    RecordCursorSample(true);
    if (g.cur.in) {
        DWORD wr;
        WriteFile(g.cur.in, "q", 1, &wr, nullptr);
        CloseHandle(g.cur.in);
        g.cur.in = nullptr;
    }
    g.closing.push_back(g.cur);
    g.cur = {};
    g.recordedMs += GetTickCount() - g.segStart;
    if (g.panel) KillTimer(g.panel, kTimerCheck);
}

void FailStart() {
    RecorderAbort();
    MessageBoxW(nullptr, Tr(L"Не удалось запустить ffmpeg.", L"Could not start ffmpeg."), L"Zipshot", MB_ICONERROR);
}

// Start button / hotkey while waiting: honour ScreenRecordStartDelay, then record.
void BeginStart() {
    if (g.cfg.startDelayMs > 0) {
        g.state = RS_Countdown;
        g.countdownEnd = GetTickCount() + g.cfg.startDelayMs;
        g.shownCount = (g.cfg.startDelayMs + 999) / 1000;
        Redraw();
        return;
    }
    if (!StartSegment()) FailStart();
}

void PanelClick(PanelBtn b) {
    switch (b) {
    case PB_Start: BeginStart(); break;
    case PB_Pause: RecorderPauseResume(); break;
    case PB_Stop: RecorderStop(); break;
    case PB_Abort: RecorderAbort(); break;
    case PB_Follow: ToggleFollow(); break;
    }
}

// Joins the finished segments into `out` (video: stream copy; GIF: palette passes). Runs on a worker thread.
bool Finalize(const std::wstring& ffmpeg, const std::vector<std::wstring>& segs, const std::wstring& out, bool gif,
              const RecCfg& cfg, const std::wstring& base) {
    std::vector<std::wstring> parts;
    for (auto& s : segs) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(s.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 2048)) parts.push_back(s);
        else Log(L"recorder: dropping empty segment %s", s.c_str());
    }
    if (parts.empty()) return false;
    std::wstring list = base + L"list.txt";
    std::string text;
    for (auto& s : parts) {
        std::wstring fwd = s;
        std::replace(fwd.begin(), fwd.end(), L'\\', L'/');
        int n = WideCharToMultiByte(CP_UTF8, 0, fwd.c_str(), (int)fwd.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, fwd.c_str(), (int)fwd.size(), u.data(), n, nullptr, nullptr);
        std::string esc;
        for (char ch : u) esc += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
        text += "file '" + esc + "'\n";
    }
    HANDLE h = CreateFileW(list.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr;
    WriteFile(h, text.data(), (DWORD)text.size(), &wr, nullptr);
    CloseHandle(h);
    std::wstring head = Q(ffmpeg) + L" -hide_banner -loglevel warning -y -f concat -safe 0 -i " + Q(list);
    bool ok;
    if (!gif) {
        ok = RunAndWait(head + L" -c copy -movflags +faststart " + Q(out));
    } else {
        std::wstring pal = base + L"palette.png";
        std::wstring use = L"paletteuse=dither=" + cfg.dither;
        if (cfg.dither == L"bayer") use += L":bayer_scale=" + std::to_wstring(cfg.bayerScale);
        if (cfg.stats == L"single") {
            // A palette per frame (ShareX "single"): one pass, palettegen streams instead of buffering.
            ok = RunAndWait(head + L" -lavfi \"split[a][b];[a]palettegen=stats_mode=single[p];[b][p]" + use + L":new=1\" -loop 0 " + Q(out));
        } else {
            ok = RunAndWait(head + L" -vf palettegen=stats_mode=" + cfg.stats + L" -update 1 -frames:v 1 " + Q(pal)) &&
                 RunAndWait(head + L" -i " + Q(pal) + L" -lavfi " + use + L" -loop 0 " + Q(out));
        }
        DeleteFileW(pal.c_str());
    }
    DeleteFileW(list.c_str());
    if (!ok) DeleteFileW(out.c_str());
    return ok && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring FocusedOutputPath(const std::wstring& original) {
    size_t dot = original.find_last_of(L'.'), slash = original.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) dot = original.size();
    const std::wstring stem = original.substr(0, dot);
    for (unsigned n = 0; n < 10000; ++n) {
        const std::wstring suffix = n ? L"_" + std::to_wstring(n + 1) : L"";
        std::wstring candidate = stem + L"_focus" + suffix + L".mp4";
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return {};
}

struct CursorCompletion { std::wstring focused, warning; };

CursorCompletion CompleteCursorVideo(const std::wstring& ffmpeg, const std::wstring& original,
                                     int width, int height, double fps, double duration,
                                     const std::vector<cursor_follow::Sample>& samples) {
    CursorCompletion completion;
    const cursor_video::Result saved = cursor_video::SaveTrack(original, width, height, fps, duration, samples);
    if (!saved.ok) {
        Log(L"recorder: cursor sidecar failed: %s", saved.error.c_str());
        completion.warning = Tr(L"Видео сохранено; данные курсора недоступны", L"Video saved; cursor data unavailable");
        return completion;
    }
    bool anyEnabled = false;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double end = i + 1 < samples.size() ? samples[i + 1].t : duration;
        anyEnabled |= samples[i].enabled && !samples[i].paused && end > samples[i].t;
    }
    if (!anyEnabled) return completion;  // Sidecar still supports later manual application.
    cursor_video::Track track;
    std::wstring error;
    std::wstring focused = FocusedOutputPath(original);
    bool ok = !focused.empty() && cursor_video::LoadTrack(original, track, &error);
    if (ok) {
        const auto rendered = cursor_video::Render(ffmpeg, original, track, focused);
        ok = rendered.ok;
        error = rendered.error;
    }
    if (ok) completion.focused = std::move(focused);
    else {
        Log(L"recorder: cursor render failed: %s", error.c_str());
        completion.warning = Tr(L"Исходник сохранён; приближение не применено", L"Original saved; cursor zoom could not be applied");
    }
    return completion;
}

void StartRecordingNow() {
    if (!g.active || g.state != RS_Countdown) return;
    g.shownCount = -1;
    if (!StartSegment()) FailStart();
}

}  // namespace

bool RecorderActive() { return g.active; }
bool RecorderBusy() { return g.busy; }
bool RecorderPaused() { return g.active && g.state == RS_Paused; }
bool RecorderFollowEnabled() { return g.active && !g.gif && g.followEnabled; }
void RecorderToggleFollow() { ToggleFollow(); }

void RecorderStart(RECT r, bool gif) {
    if (g.active || g.busy) return;
    g = {};
    g.cfg = LoadRecCfg();
    g.ffmpeg = FindFFmpeg();
    if (g.ffmpeg.empty()) {
        MessageBoxW(nullptr,
                    Tr(L"Не найден ffmpeg.exe (PATH, рядом с zipshot.exe или FFmpeg= в zipshot.ini).",
                       L"ffmpeg.exe not found (PATH, next to zipshot.exe or FFmpeg= in zipshot.ini)."),
                    L"Zipshot", MB_ICONERROR);
        return;
    }
    POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi) || !NormalizeRecordingRect(r, mi.rcMonitor)) {
        MessageBoxW(nullptr, Tr(L"Выберите область внутри экрана.", L"Select a region inside a display."), L"Zipshot", MB_ICONERROR);
        return;
    }
    g.r = r;
    g.gif = gif;
    g.followEnabled = !gif && g.cfg.followCursor;
    g.out = NewOutputPath(gif ? L"gif" : L"mp4");
    g.base = TempDir() + L"zipshot_rec_" + std::to_wstring(GetCurrentProcessId()) + L"_";
    DeleteFileW(LogPath().c_str());
    BuildVariants(mi.rcMonitor, DdaOutputIndex(mi.rcMonitor));
    if (!g.cfg.audio.empty() && !gif) Log(L"recorder: audio source %s (%s %dk)", g.cfg.audio.c_str(), g.cfg.audioCodec.c_str(), g.cfg.audioBitrate);
    g.active = true;
    g.state = RS_Waiting;
    CreateUi();
    if (g.cfg.autoStart) BeginStart();  // else the panel waits for Start (ShareX ScreenRecordAutoStart off)
}

void RecorderHotkey() {
    if (!g.active) return;
    if (g.state == RS_Waiting) BeginStart();
    else if (g.state == RS_Countdown) RecorderAbort();
    else RecorderStop();
}

void RecorderPauseResume() {
    if (!g.active) return;
    if (g.state == RS_Recording) {
        EndSegment();
        g.state = RS_Paused;
        Redraw();
    } else if (g.state == RS_Paused) {
        if (!StartSegment()) FailStart();
    } else if (g.state == RS_Waiting) {
        BeginStart();
    }
}

void RecorderAbort() {
    if (!g.active) return;
    g.aborted = true;
    RecorderStop();
}

void RecorderStop() {
    if (!g.active) return;
    if (g.state == RS_Recording) EndSegment();
    g.active = false;
    if (g.segs.empty()) g.aborted = true;  // stopped before anything was recorded
    g.busy = true;
    DestroyUi();
    std::vector<Proc> procs = g.closing;
    g.closing.clear();
    bool gif = g.gif, aborted = g.aborted;
    RecCfg cfg = g.cfg;
    std::vector<std::wstring> segs = g.segs;
    std::wstring out = g.out, ffmpeg = g.ffmpeg, base = g.base;
    auto cursorSamples = std::move(g.cursorSamples);
    const bool cursorLimitReached = g.cursorLimitReached;
    const int sourceWidth = g.r.right - g.r.left, sourceHeight = g.r.bottom - g.r.top;
    const double duration = g.recordedMs / 1000.0;
    std::thread([=, cursorSamples = std::move(cursorSamples)] {
        for (const Proc& p : procs) {
            if (WaitForSingleObject(p.pi.hProcess, 15000) == WAIT_TIMEOUT) TerminateProcess(p.pi.hProcess, 1);
            CloseHandle(p.pi.hProcess);
            CloseHandle(p.pi.hThread);
        }
        bool ok = !aborted && Finalize(ffmpeg, segs, out, gif, cfg, base);
        CursorCompletion cursor;
        if (ok && !gif) {
            try {
                cursor = CompleteCursorVideo(ffmpeg, out, sourceWidth, sourceHeight, cfg.videoFps, duration, cursorSamples);
                if (cursorLimitReached && cursor.warning.empty())
                    cursor.warning = Tr(L"Видео сохранено; достигнут лимит слежения", L"Video saved; cursor tracking limit reached");
            } catch (...) {
                Log(L"recorder: unexpected cursor processing failure; original retained");
                cursor.warning = Tr(L"Исходник сохранён; приближение не применено", L"Original saved; cursor zoom could not be applied");
            }
        }
        for (auto& s : segs) DeleteFileW(s.c_str());
        RunOnMain([=] {
            g.busy = false;
            if (ok) {
                const std::wstring delivered = cursor.focused.empty() ? out : cursor.focused;
                CopyFileToClipboard(delivered);
                PlayUiSound(S_TaskDone);
                const std::wstring title = !cursor.warning.empty() ? cursor.warning :
                    !cursor.focused.empty() ? Tr(L"Видео с приближением сохранено", L"Focused video saved") :
                    gif ? Tr(L"GIF сохранён", L"GIF saved") : Tr(L"Видео сохранено", L"Video saved");
                ShowToast(nullptr, delivered, title);
                MainWindowNotifyCapture(out);
                if (!cursor.focused.empty()) MainWindowNotifyCapture(cursor.focused);
            } else if (!aborted) {
                PlayUiSound(S_Error);
                MessageBoxW(nullptr, (Tr(L"Запись не удалась. Лог: ", L"Recording failed. Log: ") + LogPath()).c_str(), L"Zipshot",
                            MB_ICONERROR);
            }
            TrimMemory();
        });
    }).detach();
}

// `ffmpeg -list_devices true -f dshow -i dummy`: device names from stderr. Old builds list them under
// "DirectShow audio devices", new ones tag each line "(audio)"; "Alternative name" lines are skipped.
std::vector<std::wstring> ListAudioDevices() {
    std::vector<std::wstring> v;
    std::wstring ffmpeg = FindFFmpeg();
    if (ffmpeg.empty()) return v;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return v;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    std::wstring cmd = Q(ffmpeg) + L" -hide_banner -list_devices true -f dshow -i dummy";
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return v;
    }
    std::string bytes;
    DWORD start = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // writer gone: done
        if (avail) {
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || !got) break;
            bytes.append(buf, got);
            continue;
        }
        if (GetTickCount() - start > 8000) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        Sleep(15);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), text.data(), n);
    bool inAudio = false;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        pos = e == std::wstring::npos ? text.size() : e + 1;
        if (line.find(L"DirectShow audio devices") != std::wstring::npos) { inAudio = true; continue; }
        if (line.find(L"DirectShow video devices") != std::wstring::npos) { inAudio = false; continue; }
        if (line.find(L"Alternative name") != std::wstring::npos) continue;
        size_t q1 = line.find(L'"'), q2 = q1 == std::wstring::npos ? q1 : line.find(L'"', q1 + 1);
        if (q2 == std::wstring::npos || q2 == q1 + 1) continue;
        bool audio = line.find(L"(audio)") != std::wstring::npos ||
                     (inAudio && line.find(L"(video)") == std::wstring::npos && line.find(L"(none)") == std::wstring::npos);
        std::wstring name = line.substr(q1 + 1, q2 - q1 - 1);
        if (audio && std::find(v.begin(), v.end(), name) == v.end()) v.push_back(name);
    }
    return v;
}
