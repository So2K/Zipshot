#include "common.h"
#include "combine.h"
#include "gallery_model.h"
#include "clipboard_shelf.h"
#include "cursor_video.h"
#include "sharex_import.h"
#include "updater.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <string_view>
#include <unordered_set>

// Main window laid out like ShareX 18's: the left menu panel with Fugue icons and, on the right, either the
// "Hotkey | Description" table (no captures yet) or the capture history as ShareX's thumbnail view.
// Plain GDI, double-buffered. Thumbnails come from the shell (Windows thumbnail cache) on one worker thread.

using Microsoft::WRL::ComPtr;

namespace {

// ShareX dark theme.
const COLORREF kBg = RGB(39, 39, 39), kLightBg = RGB(46, 46, 46), kDarkBg = RGB(34, 34, 34), kText = RGB(231, 233, 234),
               kTextOff = RGB(118, 119, 120), kBorder = RGB(31, 31, 31), kSepLight = RGB(44, 44, 44), kSepDark = RGB(31, 31, 31),
               kHover = RGB(51, 51, 51), kAccent = RGB(0, 138, 255), kOk = RGB(80, 190, 80), kFail = RGB(222, 62, 62),
               kTile = RGB(46, 46, 46), kTileHover = RGB(56, 56, 56), kTileSel = RGB(52, 60, 70), kScroll = RGB(110, 110, 110);

enum Act { A_None, A_Capture, A_AfterCapture, A_AppSettings, A_Settings, A_Hotkeys, A_Folder, A_About };
struct Item { const wchar_t* ru; const wchar_t* en; int icon; bool sub; Act act; };  // ru == nullptr: separator
const Item kItems[] = {
    {L"Захват", L"Capture", 200, true, A_Capture},
    {L"Загрузка", L"Upload", 201, true, A_None},
    {L"Рабочие процессы", L"Workflows", 202, true, A_None},
    {L"Инструменты", L"Tools", 203, true, A_None},
    {nullptr, nullptr, 0, false, A_None},
    {L"Задачи после захвата", L"After capture tasks", 204, true, A_AfterCapture},
    {L"Задачи после загрузки", L"After upload tasks", 205, true, A_None},
    {L"Места назначения", L"Destinations", 206, true, A_None},
    {nullptr, nullptr, 0, false, A_None},
    {L"Настройки приложения...", L"Application settings...", 207, true, A_AppSettings},
    {L"Настройки задач...", L"Task settings...", 208, false, A_Settings},
    {L"Настройки хоткеев...", L"Hotkey settings...", 209, false, A_Hotkeys},
    {L"Настройки мест назначения...", L"Destination settings...", 210, false, A_None},
    {L"Свои загрузчики...", L"Custom uploader settings...", 211, false, A_None},
    {nullptr, nullptr, 0, false, A_None},
    {L"Папка скриншотов...", L"Screenshots folder...", 212, false, A_Folder},
    {L"История...", L"History...", 213, false, A_Folder},
    {L"История изображений...", L"Image history...", 214, false, A_Folder},
    {nullptr, nullptr, 0, false, A_None},
    {L"Отладка", L"Debug", 215, true, A_None},
    {L"Пожертвовать...", L"Donate...", 216, false, A_None},
    {L"Подписаться на @ShareX...", L"Follow @ShareX...", 217, false, A_None},
    {L"Discord...", L"Discord...", 218, false, A_None},
    {L"О программе...", L"About...", 219, false, A_About},
};
constexpr int kNumItems = (int)(sizeof(kItems) / sizeof(kItems[0]));
const wchar_t* Text(const Item& it) { return Tr(it.ru, it.en); }

struct CaptureEntry { const wchar_t* ru; const wchar_t* en; int icon; int cmd; bool restore; };
const CaptureEntry kCapture[] = {
    {L"Область\tCtrl+PrtScn", L"Region\tCtrl+PrtScn", 220, C_Region, true},
    {L"Весь экран\tPrtScn", L"Fullscreen\tPrtScn", 221, C_Full, true},
    {L"Активный монитор", L"Active monitor", 132, C_ActiveMonitor, true},
    {L"Последняя область", L"Last region", 220, C_LastRegion, true},
    {L"Активное окно\tAlt+PrtScn", L"Active window\tAlt+PrtScn", 222, C_Window, true},
    {L"Запись экрана\tShift+PrtScn", L"Screen recording\tShift+PrtScn", 223, C_Video, false},
    {L"Запись экрана (GIF)\tCtrl+Shift+PrtScn", L"Screen recording (GIF)\tCtrl+Shift+PrtScn", 224, C_Gif, false},
};
struct ToggleEntry { const wchar_t* ru; const wchar_t* en; const wchar_t* key; bool def; };
const ToggleEntry kAfter[] = {
    {L"Копировать изображение в буфер", L"Copy image to clipboard", L"AfterCopy", true},
    {L"Сохранить изображение в файл", L"Save image to file", L"AfterSave", true},
    {L"Показать уведомление", L"Show notification", L"AfterToast", true},
    {L"Открыть в редакторе", L"Open in image editor", L"AfterEditor", false},
};
constexpr UINT kCapBase = 1, kAfterBase = 20, kLangRu = 40, kLangEn = 41, kImport = 42, kOpenIni = 43, kAutostart = 44,
               kSound = 45, kAutoUpdate = 46, kCheckUpdate = 47, kInstallUpdate = 48;
enum : UINT { M_Open = 100, M_Editor, M_CopyImage, M_CopyFile, M_CopyPath, M_ShowFolder, M_Delete, M_Combine, M_CursorFollow };
constexpr UINT_PTR kTimerRun = 1, kTimerFlash = 2;
constexpr size_t kMaxThumbs = 240;
constexpr UINT kSearch = 400, kPaste = 401, kNewest = 402, kOldest = 403, kUpdatePill = 404, kFilterBase = 410;

struct IconBmp { HBITMAP normal = nullptr, gray = nullptr; };

using Kind = gallery::Kind;
constexpr Kind K_Image = Kind::Image, K_Gif = Kind::Gif, K_Mp4 = Kind::Video;
struct Entry : gallery::Item {
    HBITMAP thumb = nullptr;
    int tw = 0, th = 0;
    bool done = false;  // thumbnail arrived (thumb may still be null when the shell had none)
};

struct State {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT menuFont = nullptr, tableFont = nullptr, badgeFont = nullptr;
    std::map<int, IconBmp> icons;
    std::vector<HotkeyInfo> rows;
    int panelW = 0, menuH = 0;
    int hover = -1, open = -1, pressed = -1;
    bool tracking = false;
    int flash = 0;
    int pendingCmd = 0;
    bool pendingRestore = false;
    // Capture history.
    std::vector<gallery::Item> catalog;
    std::vector<Entry> files;
    std::wstring query, notice;
    gallery::Filter filter = gallery::Filter::All;
    bool oldestFirst = false, pasting = false;
    HWND search = nullptr;
    WNDPROC searchProc = nullptr;
    HBRUSH searchBrush = nullptr;
    int toolbarHover = -1;
    unsigned paintCount = 0;
    bool showTable = false;  // "Hotkey settings..." toggles the table over the history
    int scroll = 0, hoverTile = -1, sel = -1;
    int downTile = -1;
    POINT downPt{};
    bool dragArmed = false, scrollDrag = false;
    int scrollGrab = 0;
    unsigned session = 0;
    // Multi-select: paths in the order they were picked (that is the combine order). `sel` is the focused tile.
    std::vector<std::wstring> picked;
    int anchor = -1;
    bool collapseOnUp = false;  // plain click on a multi-selection collapses it on release unless a drag starts
    // OLE drop target: hovered tile and side (0 left, 1 right, 2 top, 3 bottom) while something is dragged over.
    IDropTarget* drop = nullptr;
    int dropTile = -1, dropSide = -1;
};
State* g_mw = nullptr;
unsigned g_sessionCounter = 0;
std::atomic_bool g_cursorRenderBusy{false};
std::atomic_uint g_pasteJobs{0};

int S(int v) { return MulDiv(v, (int)g_mw->dpi, 96); }
bool Enabled(const Item& it) { return it.act != A_None; }

// Logical metrics (ShareX toolstrip proportions).
constexpr int kItemH = 21, kSepH = 7, kPad = 3, kIconX = 5, kTextGap = 7, kArrowW = 18, kMinPanelW = 167, kContentW = 680;
// Thumbnail view metrics (ShareX TaskThumbnailView, 200x150 thumbnails).
constexpr int kThumbW = 200, kThumbH = 150, kTilePad = 4, kTileGap = 8, kMargin = 10, kScrollW = 6;

// ---------- thumbnail worker ----------

struct ThumbJob { std::wstring path; int w, h; };
struct Worker {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<ThumbJob> queue;  // front = most wanted
    std::wstring inFlight;
    bool stop = false;
    unsigned session = 0;
};
std::shared_ptr<Worker> g_worker;

void OnThumb(unsigned session, const std::wstring& path, int w, int h, HBITMAP bmp, int bw, int bh);

// Shell thumbnail -> opaque top-down DIB aspect-fitted into w x h, composited on the tile colour.
HBITMAP MakeThumb(const std::wstring& path, int w, int h, int& outW, int& outH) {
    ComPtr<IShellItemImageFactory> fac;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&fac)))) return nullptr;
    HBITMAP hb = nullptr;
    int req = std::max(w, h);
    if (FAILED(fac->GetImage(SIZE{req, req}, SIIGBF_BIGGERSIZEOK | SIIGBF_RESIZETOFIT, &hb)) || !hb) return nullptr;
    BITMAP bm{};
    GetObjectW(hb, sizeof(bm), &bm);
    int sw = bm.bmWidth, sh = std::abs(bm.bmHeight);
    if (sw <= 0 || sh <= 0) {
        DeleteObject(hb);
        return nullptr;
    }
    std::vector<uint32_t> px((size_t)sw * sh);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), sw, -sh, 1, 32, BI_RGB};
    HDC dc = CreateCompatibleDC(nullptr);
    int got = GetDIBits(dc, hb, 0, (UINT)sh, px.data(), &bi, DIB_RGB_COLORS);
    DeleteObject(hb);
    if (got != sh) {
        DeleteDC(dc);
        return nullptr;
    }
    bool anyAlpha = false, straight = false;
    for (uint32_t p : px) {
        uint32_t a = p >> 24;
        if (a) anyAlpha = true;
        if ((p & 255) > a || ((p >> 8) & 255) > a || ((p >> 16) & 255) > a) straight = true;
    }
    const uint32_t bg[3] = {kTile >> 16 & 255, kTile >> 8 & 255, kTile & 255};  // B, G, R
    if (anyAlpha) {
        for (uint32_t& p : px) {
            uint32_t a = p >> 24, out = 0xFF000000;
            for (int c = 0; c < 3; ++c) {
                uint32_t v = (p >> (c * 8)) & 255;
                v = straight ? (v * a + bg[c] * (255 - a)) / 255 : std::min<uint32_t>(255, v + bg[c] * (255 - a) / 255);
                out |= v << (c * 8);
            }
            p = out;
        }
    } else {
        for (uint32_t& p : px) p |= 0xFF000000;
    }
    double k = std::min({1.0, (double)w / sw, (double)h / sh});
    int dw = std::max(1, (int)(sw * k + 0.5)), dh = std::max(1, (int)(sh * k + 0.5));
    void* sbits = nullptr;
    HBITMAP src = CreateDib(sw, sh, &sbits);
    if (!src) {
        DeleteDC(dc);
        return nullptr;
    }
    memcpy(sbits, px.data(), px.size() * 4);
    if (dw == sw && dh == sh) {
        DeleteDC(dc);
        outW = sw;
        outH = sh;
        return src;
    }
    void* dbits = nullptr;
    HBITMAP dst = CreateDib(dw, dh, &dbits);
    if (dst) {
        HDC sdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o1 = SelectObject(sdc, src), o2 = SelectObject(dc, dst);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, 0, 0, dw, dh, sdc, 0, 0, sw, sh, SRCCOPY);
        GdiFlush();
        SelectObject(sdc, o1);
        SelectObject(dc, o2);
        DeleteDC(sdc);
        outW = dw;
        outH = dh;
    }
    DeleteObject(src);
    DeleteDC(dc);
    return dst;
}

void WorkerProc(std::shared_ptr<Worker> wk) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        ThumbJob job;
        {
            std::unique_lock<std::mutex> lk(wk->mu);
            wk->cv.wait(lk, [&] { return wk->stop || !wk->queue.empty(); });
            if (wk->stop) break;
            job = std::move(wk->queue.front());
            wk->queue.erase(wk->queue.begin());
            wk->inFlight = job.path;
        }
        int bw = 0, bh = 0;
        HBITMAP bmp = MakeThumb(job.path, job.w, job.h, bw, bh);
        bool stopped;
        {
            std::lock_guard<std::mutex> lk(wk->mu);
            wk->inFlight.clear();
            stopped = wk->stop;
        }
        if (stopped) {
            if (bmp) DeleteObject(bmp);
            break;
        }
        unsigned session = wk->session;
        RunOnMain([session, path = job.path, w = job.w, h = job.h, bmp, bw, bh] { OnThumb(session, path, w, h, bmp, bw, bh); });
    }
    if (SUCCEEDED(hr)) CoUninitialize();
}

void StartWorker() {
    g_worker = std::make_shared<Worker>();
    g_worker->session = g_mw->session;
    std::thread(WorkerProc, g_worker).detach();
}

void StopWorker() {
    if (!g_worker) return;
    {
        std::lock_guard<std::mutex> lk(g_worker->mu);
        g_worker->stop = true;
        g_worker->queue.clear();
    }
    g_worker->cv.notify_all();
    g_worker.reset();  // the thread keeps its own reference and exits after the current item
}

void SetThumbQueue(std::vector<ThumbJob>&& jobs) {
    if (!g_worker) return;
    {
        std::lock_guard<std::mutex> lk(g_worker->mu);
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const ThumbJob& j) { return j.path == g_worker->inFlight; }),
                   jobs.end());
        g_worker->queue = std::move(jobs);
    }
    g_worker->cv.notify_all();
}

// ---------- capture history files ----------

bool IsCaptureExt(const std::wstring& name, Kind& kind) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const wchar_t* e = name.c_str() + dot + 1;
    if (!_wcsicmp(e, L"png") || !_wcsicmp(e, L"jpg") || !_wcsicmp(e, L"jpeg") ||
        !_wcsicmp(e, L"bmp") || !_wcsicmp(e, L"webp") || !_wcsicmp(e, L"tif") || !_wcsicmp(e, L"tiff")) kind = K_Image;
    else if (!_wcsicmp(e, L"gif")) kind = K_Gif;
    else if (!_wcsicmp(e, L"mp4") || !_wcsicmp(e, L"webm") || !_wcsicmp(e, L"mkv") ||
             !_wcsicmp(e, L"mov") || !_wcsicmp(e, L"avi")) kind = K_Mp4;
    else return false;
    return true;
}

ULONGLONG FtToU64(FILETIME ft) { return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

void ScanDir(const std::wstring& dir, int depth, std::vector<gallery::Item>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) ScanDir(dir + L"\\" + fd.cFileName, depth - 1, out);
            continue;
        }
        gallery::Item e;
        if (!IsCaptureExt(fd.cFileName, e.kind)) continue;
        e.name = fd.cFileName;
        e.path = dir + L"\\" + fd.cFileName;
        e.time = FtToU64(fd.ftLastWriteTime);
        e.shelf = shelf::IsShelfPath(e.path);
        out.push_back(std::move(e));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

void FreeThumb(Entry& e) {
    if (e.thumb) DeleteObject(e.thumb);
    e.thumb = nullptr;
    e.done = false;
}

void FreeAllThumbs() {
    for (Entry& e : g_mw->files) FreeThumb(e);
}

void ClampScroll();
void LayoutToolbar();

void RebuildView(bool resetScroll = false) {
    const auto pathAt = [](int at) -> std::wstring {
        return at >= 0 && at < (int)g_mw->files.size() ? g_mw->files[at].path : L"";
    };
    const std::wstring selected = pathAt(g_mw->sel), anchor = pathAt(g_mw->anchor);
    std::vector<Entry> v;
    for (size_t at : gallery::Select(g_mw->catalog, g_mw->query, g_mw->filter, g_mw->oldestFirst)) {
        Entry e;
        static_cast<gallery::Item&>(e) = g_mw->catalog[at];
        v.push_back(std::move(e));
    }
    // Keep thumbnails of files that are still listed.
    std::map<std::wstring, Entry*> old;
    for (Entry& e : g_mw->files) old[e.path] = &e;
    for (Entry& e : v) {
        auto it = old.find(e.path);
        if (it != old.end() && it->second->time == e.time) {
            std::swap(e.thumb, it->second->thumb);
            e.tw = it->second->tw;
            e.th = it->second->th;
            e.done = it->second->done;
        }
    }
    FreeAllThumbs();
    g_mw->files = std::move(v);
    g_mw->sel = g_mw->anchor = -1;
    for (int i = 0; i < (int)g_mw->files.size(); ++i) {
        if (g_mw->files[i].path == selected) g_mw->sel = i;
        if (g_mw->files[i].path == anchor) g_mw->anchor = i;
    }
    if (!g_mw->picked.empty()) {
        // Paths belong to files, which remain stable while only picked is pruned.
        // A hash membership check avoids scanning the whole gallery per selection.
        std::unordered_set<std::wstring_view> visible;
        visible.reserve(g_mw->files.size());
        for (const Entry& e : g_mw->files) visible.insert(e.path);
        std::erase_if(g_mw->picked, [&](const std::wstring& p) { return !visible.contains(p); });
    }
    g_mw->hoverTile = g_mw->dropTile = g_mw->downTile = -1;
    g_mw->dragArmed = g_mw->collapseOnUp = g_mw->scrollDrag = false;
    if (GetCapture() == g_mw->hwnd) ReleaseCapture();
    if (resetScroll) g_mw->scroll = 0;
    ClampScroll();
    LayoutToolbar();
    InvalidateRect(g_mw->hwnd, nullptr, FALSE);
}

void Rescan() {
    std::vector<gallery::Item> all;
    std::wstring root = SaveDirRoot();
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    if (!root.empty()) ScanDir(root, 1, all);
    g_mw->catalog = std::move(all);
    RebuildView();
}

bool IsPicked(const std::wstring& path) {
    return std::find(g_mw->picked.begin(), g_mw->picked.end(), path) != g_mw->picked.end();
}

bool IsImageEntry(const Entry& e) { return e.kind == K_Image || e.kind == K_Gif; }

// ---------- icons ----------

IconBmp DecodeIcon(int id, int size) {
    IconBmp out;
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return out;
    void* data = LockResource(LoadResource(g_inst, r));
    DWORD bytes = SizeofResource(g_inst, r);
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    ComPtr<IWICStream> s;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> conv;
    if (!f || FAILED(f->CreateStream(&s)) || FAILED(s->InitializeFromMemory((BYTE*)data, bytes)) ||
        FAILED(f->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), size, size, WICBitmapInterpolationModeFant)) ||
        FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return out;
    void* bits = nullptr;
    out.normal = CreateDib(size, size, &bits);
    if (!out.normal) return out;
    UINT n = (UINT)size * size;
    conv->CopyPixels(nullptr, size * 4, n * 4, (BYTE*)bits);
    void* gbits = nullptr;
    out.gray = CreateDib(size, size, &gbits);
    if (out.gray) {
        auto* src = (const uint32_t*)bits;
        auto* dst = (uint32_t*)gbits;
        for (UINT i = 0; i < n; ++i) {  // premultiplied grey at ~40% opacity, like a disabled ToolStrip image
            uint32_t p = src[i];
            uint32_t a = p >> 24, b = p & 255, g = (p >> 8) & 255, rr = (p >> 16) & 255;
            uint32_t y = (rr * 77 + g * 150 + b * 29) >> 8;
            y = y * 2 / 5;
            a = a * 2 / 5;
            dst[i] = (a << 24) | (y << 16) | (y << 8) | y;
        }
    }
    return out;
}

const IconBmp& Icon(int id) {
    auto it = g_mw->icons.find(id);
    if (it == g_mw->icons.end()) it = g_mw->icons.emplace(id, DecodeIcon(id, S(16))).first;
    return it->second;
}

void FreeIcons() {
    for (auto& [id, b] : g_mw->icons) {
        if (b.normal) DeleteObject(b.normal);
        if (b.gray) DeleteObject(b.gray);
    }
    g_mw->icons.clear();
}

void DrawIcon(HDC dc, HDC mem, int id, bool enabled, int x, int y) {
    const IconBmp& b = Icon(id);
    HBITMAP bmp = enabled ? b.normal : b.gray;
    if (!bmp) return;
    HGDIOBJ old = SelectObject(mem, bmp);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, x, y, S(16), S(16), mem, 0, 0, S(16), S(16), bf);
    SelectObject(mem, old);
}

// ---------- layout ----------

void Fill(HDC dc, RECT r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void FrameRectC(HDC dc, RECT r, COLORREF c, int t = 1) {
    Fill(dc, {r.left, r.top, r.right, r.top + t}, c);
    Fill(dc, {r.left, r.bottom - t, r.right, r.bottom}, c);
    Fill(dc, {r.left, r.top, r.left + t, r.bottom}, c);
    Fill(dc, {r.right - t, r.top, r.right, r.bottom}, c);
}

int TextW(HDC dc, const std::wstring& s) {
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    return sz.cx;
}

RECT ItemRect(int i) {
    int y = S(kPad);
    for (int k = 0; k < i; ++k) y += kItems[k].ru ? S(kItemH) : S(kSepH);
    return {0, y, g_mw->panelW - 1, y + (kItems[i].ru ? S(kItemH) : S(kSepH))};
}

int HitItem(POINT p) {
    if (p.x < 0 || p.x >= g_mw->panelW - 1) return -1;
    for (int i = 0; i < kNumItems; ++i) {
        RECT r = ItemRect(i);
        if (kItems[i].ru && PtInRect(&r, p)) return i;
    }
    return -1;
}

void CreateFonts() {
    if (g_mw->menuFont) DeleteObject(g_mw->menuFont);
    if (g_mw->tableFont) DeleteObject(g_mw->tableFont);
    if (g_mw->badgeFont) DeleteObject(g_mw->badgeFont);
    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfHeight = -MulDiv(9, (int)g_mw->dpi, 72);
    g_mw->menuFont = CreateFontIndirectW(&lf);
    lf.lfHeight = -MulDiv(105, (int)g_mw->dpi, 720);
    g_mw->tableFont = CreateFontIndirectW(&lf);
    lf.lfHeight = -MulDiv(7, (int)g_mw->dpi, 72);
    lf.lfWeight = FW_BOLD;
    g_mw->badgeFont = CreateFontIndirectW(&lf);
}

void Measure() {
    HDC dc = GetDC(g_mw->hwnd);
    HGDIOBJ old = SelectObject(dc, g_mw->menuFont);
    int w = 0;
    for (const Item& it : kItems)
        if (it.ru) w = std::max(w, TextW(dc, Text(it)));
    SelectObject(dc, old);
    ReleaseDC(g_mw->hwnd, dc);
    g_mw->panelW = std::max(S(kMinPanelW), S(kIconX + 16 + kTextGap + kArrowW + 4) + w) + 1;  // +1: border line
    g_mw->menuH = ItemRect(kNumItems - 1).bottom + S(kPad);
}

SIZE WindowSizeFor(UINT dpi) {
    RECT r{0, 0, g_mw->panelW + S(kContentW), g_mw->menuH};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    return {r.right - r.left, r.bottom - r.top};
}

bool RefreshRows() {
    std::vector<HotkeyInfo> v = HotkeyStatus();
    bool same = v.size() == g_mw->rows.size();
    for (size_t i = 0; same && i < v.size(); ++i)
        same = v[i].keys == g_mw->rows[i].keys && v[i].description == g_mw->rows[i].description &&
               v[i].registered == g_mw->rows[i].registered;
    if (!same) g_mw->rows = std::move(v);
    return !same;
}

// ---------- thumbnail grid geometry ----------

bool GridMode() { return !g_mw->showTable; }

struct ToolbarLayout {
    RECT search{}, paste{}, sort{}, filters[5]{}, status{};
    int height = 0;
};

ToolbarLayout GetToolbarLayout() {
    RECT rc{};
    GetClientRect(g_mw->hwnd, &rc);
    ToolbarLayout t;
    const int left = g_mw->panelW + S(12), right = rc.right - S(12), gap = S(8);
    const bool wide = right - left >= S(620);
    const int pasteW = S(130), sortW = S(170), h = S(30);
    t.paste = {right - pasteW - (wide ? sortW + gap : 0), S(10),
               right - (wide ? sortW + gap : 0), S(10) + h};
    t.search = {left, S(10), std::max<LONG>(left + S(80), t.paste.left - gap), S(10) + h};
    t.sort = wide ? RECT{right - sortW, S(10), right, S(10) + h}
                  : RECT{left, S(86), left + sortW, S(116)};
    int x = left;
    const int widths[] = {44, 60, 48, 64, 66};
    for (int i = 0; i < 5; ++i) {
        t.filters[i] = {x, S(50), x + S(widths[i]), S(78)};
        x += S(widths[i] + 6);
    }
    t.status = wide ? RECT{x + gap, S(50), right, S(78)}
                    : RECT{t.sort.right + gap, S(86), right, S(116)};
    t.height = S(wide ? 88 : 124);
    return t;
}

RECT ContentRect() {
    RECT rc;
    GetClientRect(g_mw->hwnd, &rc);
    rc.left = g_mw->panelW;
    if (GridMode()) rc.top = GetToolbarLayout().height;
    return rc;
}

struct Grid { int x0, y0, tileW, tileH, stepX, stepY, cols, rows, contentH, textH; };

Grid GetGrid() {
    Grid g{};
    RECT a = ContentRect();
    HDC dc = GetDC(g_mw->hwnd);
    HGDIOBJ old = SelectObject(dc, g_mw->tableFont);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g_mw->hwnd, dc);
    g.textH = tm.tmHeight + S(6);
    g.tileW = S(kThumbW) + 2 * S(kTilePad);
    g.tileH = S(kThumbH) + 2 * S(kTilePad) + g.textH;
    g.stepX = g.tileW + S(kTileGap);
    g.stepY = g.tileH + S(kTileGap);
    int avail = (int)(a.right - a.left) - 2 * S(kMargin) + S(kTileGap);
    g.cols = std::max(1, avail / g.stepX);
    g.rows = ((int)g_mw->files.size() + g.cols - 1) / g.cols;
    g.contentH = g.rows ? g.rows * g.stepY - S(kTileGap) + 2 * S(kMargin) : 0;
    // Centre the columns horizontally like a flow layout with even side margins.
    int used = g.cols * g.stepX - S(kTileGap);
    g.x0 = a.left + std::max(S(kMargin), ((int)(a.right - a.left) - used) / 2);
    g.y0 = a.top + S(kMargin);
    return g;
}

int MaxScroll(const Grid& g) {
    RECT a = ContentRect();
    return std::max(0, g.contentH - (int)(a.bottom - a.top));
}

void ClampScroll() {
    if (!g_mw->hwnd) return;
    Grid g = GetGrid();
    g_mw->scroll = std::clamp(g_mw->scroll, 0, MaxScroll(g));
}

RECT TileRect(const Grid& g, int i) {
    int c = i % g.cols, r = i / g.cols;
    int x = g.x0 + c * g.stepX, y = g.y0 + r * g.stepY - g_mw->scroll;
    return {x, y, x + g.tileW, y + g.tileH};
}

int HitTile(POINT p) {
    if (!GridMode()) return -1;
    RECT a = ContentRect();
    if (!PtInRect(&a, p)) return -1;
    Grid g = GetGrid();
    int c = (p.x - g.x0) / g.stepX, r = (p.y - g.y0 + g_mw->scroll) / g.stepY;
    if (p.x < g.x0 || p.y - g.y0 + g_mw->scroll < 0 || c >= g.cols) return -1;
    int i = r * g.cols + c;
    if (i < 0 || i >= (int)g_mw->files.size()) return -1;
    RECT t = TileRect(g, i);
    return PtInRect(&t, p) ? i : -1;
}

// Overlay scrollbar thumb in client coordinates; empty when nothing overflows.
RECT ScrollThumb(const Grid& g) {
    RECT a = ContentRect();
    int viewH = a.bottom - a.top, maxS = MaxScroll(g);
    if (maxS <= 0 || viewH <= 0) return {};
    int trackTop = a.top + S(2), trackH = viewH - S(4);
    int th = std::max(S(24), (int)((long long)trackH * viewH / g.contentH));
    int ty = trackTop + (int)((long long)(trackH - th) * g_mw->scroll / maxS);
    return {a.right - S(kScrollW) - S(2), ty, a.right - S(2), ty + th};
}

void EnsureVisible(int i) {
    if (i < 0 || !GridMode()) return;
    Grid g = GetGrid();
    RECT a = ContentRect();
    RECT t = TileRect(g, i);
    if (t.top < a.top + S(kMargin)) g_mw->scroll -= a.top + S(kMargin) - t.top;
    else if (t.bottom > a.bottom - S(kMargin)) g_mw->scroll += t.bottom - (a.bottom - S(kMargin));
    ClampScroll();
}

void SelectPath(const std::wstring& path);
void PasteToShelf();

bool ClipboardHasMedia() {
    return IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(CF_DIBV5) ||
           IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG"));
}

LRESULT CALLBACK SearchProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!g_mw || !g_mw->searchProc) return DefWindowProcW(h, m, w, l);
    if ((m == WM_KEYDOWN && w == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) && ClipboardHasMedia()) ||
        (m == WM_PASTE && ClipboardHasMedia())) {
        PasteToShelf();
        return 0;
    }
    if (m == WM_CHAR && w == 22 && ClipboardHasMedia()) return 0;
    if (m == WM_KEYDOWN) {
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        if (w == VK_ESCAPE) {
            SetWindowTextW(h, L"");
            SetFocus(g_mw->hwnd);
            return 0;
        }
        if (w == VK_RETURN || w == VK_DOWN || w == VK_TAB) {
            SetFocus(g_mw->hwnd);
            return 0;
        }
    }
    LRESULT r = CallWindowProcW(g_mw->searchProc, h, m, w, l);
    if (m == WM_SETFOCUS || m == WM_KILLFOCUS) InvalidateRect(g_mw->hwnd, nullptr, FALSE);
    return r;
}

void LayoutToolbar() {
    if (!g_mw || !g_mw->hwnd || !g_mw->search) return;
    const ToolbarLayout t = GetToolbarLayout();
    SetWindowPos(g_mw->search, nullptr, t.search.left + S(28), t.search.top + S(6),
                 std::max(S(30), (int)(t.search.right - t.search.left) - S(36)), S(20),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_mw->search, GridMode() ? SW_SHOWNA : SW_HIDE);
    SendMessageW(g_mw->search, WM_SETFONT, (WPARAM)g_mw->menuFont, TRUE);
    SendMessageW(g_mw->search, EM_SETCUEBANNER, TRUE, (LPARAM)Tr(L"Поиск по имени...", L"Search by filename..."));
}

void CreateToolbar() {
    g_mw->searchBrush = CreateSolidBrush(kLightBg);
    g_mw->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                                  0, 0, 0, 0, g_mw->hwnd, (HMENU)(UINT_PTR)kSearch, g_inst, nullptr);
    if (g_mw->search) {
        g_mw->searchProc = (WNDPROC)SetWindowLongPtrW(g_mw->search, GWLP_WNDPROC, (LONG_PTR)SearchProc);
        SendMessageW(g_mw->search, EM_SETLIMITTEXT, 512, 0);
    }
    LayoutToolbar();
}

void PasteToShelf() {
    if (!g_mw || g_mw->pasting || g_pasteJobs.load()) return;
    shelf::Payload data;
    std::wstring error;
    if (!shelf::ReadClipboard(g_mw->hwnd, data, error)) {
        g_mw->notice = error.empty() ? Tr(L"В буфере нет изображения или медиафайла", L"No image or media file in the clipboard") : error;
        InvalidateRect(g_mw->hwnd, nullptr, FALSE);
        return;
    }
    g_mw->pasting = true;
    g_mw->notice = Tr(L"Добавляем на полку...", L"Adding to shelf...");
    g_mw->showTable = false;
    LayoutToolbar();
    InvalidateRect(g_mw->hwnd, nullptr, FALSE);
    const unsigned session = g_mw->session;
    ++g_pasteJobs;
    try {
      std::thread([session, data = std::move(data), readError = std::move(error)]() mutable {
        shelf::SaveResult result;
        try { result = shelf::Store(std::move(data)); }
        catch (...) { result.error = Tr(L"Не удалось добавить файлы на полку", L"Could not add files to the shelf"); }
        --g_pasteJobs;
        if (!readError.empty()) {
            if (!result.error.empty()) readError += L"\n" + result.error;
            result.error = std::move(readError);
        }
        RunOnMain([session, result = std::move(result)] {
            if (!g_mw) return;
            if (g_mw->session != session) { Rescan(); return; }
            g_mw->pasting = false;
            if (!result.paths.empty()) {
                g_mw->filter = gallery::Filter::Shelf;
                SetWindowTextW(g_mw->search, L"");
                g_mw->query.clear();
                Rescan();
                SelectPath(result.paths.back());
                g_mw->notice = result.error.empty()
                    ? std::to_wstring(result.paths.size()) + Tr(L" добавлено на полку", L" added to shelf")
                    : result.error;
            } else g_mw->notice = result.error.empty() ? Tr(L"Не удалось вставить изображение", L"Could not paste image") : result.error;
            InvalidateRect(g_mw->hwnd, nullptr, FALSE);
        });
      }).detach();
    } catch (...) {
        --g_pasteJobs;
        g_mw->pasting = false;
        g_mw->notice = Tr(L"Не удалось начать вставку", L"Could not start the paste operation");
        InvalidateRect(g_mw->hwnd, nullptr, FALSE);
    }
}

int HitToolbar(POINT p) {
    if (!GridMode()) return -1;
    const ToolbarLayout t = GetToolbarLayout();
    if (PtInRect(&t.paste, p)) return kPaste;
    if (PtInRect(&t.sort, p)) return kNewest;
    for (int i = 0; i < 5; ++i) if (PtInRect(&t.filters[i], p)) return kFilterBase + i;
    if ((updater::HasUpdate() || updater::Downloading()) && t.status.right > t.status.left && PtInRect(&t.status, p))
        return kUpdatePill;
    return -1;
}

void ToolbarCommand(UINT id) {
    if (id == kPaste) PasteToShelf();
    else if (id == kUpdatePill) {
        if (updater::HasUpdate() && !updater::Downloading()) RunCommand(C_InstallUpdate);
    }
    else if (id >= kFilterBase && id < kFilterBase + 5) {
        g_mw->filter = (gallery::Filter)(id - kFilterBase);
        g_mw->notice.clear();
        RebuildView(true);
    } else if (id == kNewest || id == kOldest) {
        g_mw->oldestFirst = id == kOldest;
        SetSettingBool(L"GalleryOldestFirst", g_mw->oldestFirst);
        RebuildView(true);
    }
}

void OpenSortMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, kNewest, Tr(L"Сначала новые", L"Newest first"));
    AppendMenuW(m, MF_STRING, kOldest, Tr(L"Сначала старые", L"Oldest first"));
    CheckMenuRadioItem(m, kNewest, kOldest, g_mw->oldestFirst ? kOldest : kNewest, MF_BYCOMMAND);
    RECT r = GetToolbarLayout().sort;
    MapWindowPoints(g_mw->hwnd, nullptr, (POINT*)&r, 2);
    UINT id = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, r.left, r.bottom, g_mw->hwnd, nullptr);
    DestroyMenu(m);
    if (id && g_mw) ToolbarCommand(id);
}

// ---------- painting ----------

void PaintToolbar(HDC dc) {
    const ToolbarLayout t = GetToolbarLayout();
    RECT rc{};
    GetClientRect(g_mw->hwnd, &rc);
    Fill(dc, {g_mw->panelW, 0, rc.right, t.height}, kBg);
    Fill(dc, {g_mw->panelW, t.height - 1, rc.right, t.height}, kBorder);
    SelectObject(dc, g_mw->menuFont);
    auto button = [&](RECT r, const wchar_t* text, bool selected, bool hover, bool accent = false) {
        COLORREF bg = accent ? kAccent : selected ? kTileSel : hover ? kTileHover : kLightBg;
        SetDCBrushColor(dc, bg);
        SetDCPenColor(dc, selected ? kAccent : kBorder);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
        RoundRect(dc, r.left, r.top, r.right, r.bottom, S(6), S(6));
        SelectObject(dc, ob); SelectObject(dc, op);
        SetTextColor(dc, accent ? RGB(255,255,255) : selected ? RGB(120,190,255) : kText);
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    };
    button(t.search, L"", GetFocus() == g_mw->search, false);
    HPEN pen = CreatePen(PS_SOLID, std::max(1,S(1)), kTextOff);
    HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    int x = t.search.left + S(10), y = t.search.top + S(9);
    Ellipse(dc, x, y, x+S(10), y+S(10)); MoveToEx(dc, x+S(8), y+S(8), nullptr); LineTo(dc, x+S(14), y+S(14));
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(pen);
    button(t.paste, (g_mw->pasting || g_pasteJobs.load()) ? Tr(L"Добавляем...", L"Adding...") : Tr(L"+ Вставить  Ctrl+V", L"+ Paste  Ctrl+V"), false, g_mw->toolbarHover == kPaste, true);
    button(t.sort, g_mw->oldestFirst ? Tr(L"↑ Сначала старые", L"↑ Oldest first") : Tr(L"↓ Сначала новые", L"↓ Newest first"), false, g_mw->toolbarHover == kNewest);
    const wchar_t* names[] = {Tr(L"Все",L"All"),Tr(L"Фото",L"Photos"),L"GIF",Tr(L"Видео",L"Video"),Tr(L"Полка",L"Shelf")};
    for (int i=0;i<5;++i) button(t.filters[i], names[i], (int)g_mw->filter == i, g_mw->toolbarHover == (int)kFilterBase+i);
    if (t.status.right > t.status.left) {
        if (updater::HasUpdate() || updater::Downloading()) {
            std::wstring upLabel = updater::Downloading()
                ? (std::wstring(Tr(L"Обновление ", L"Updating ")) + std::to_wstring((int)(updater::Progress() * 100)) + L"%")
                : (std::wstring(Tr(L"↓ Обновить ", L"↓ Update ")) + updater::AvailableVersion());
            int pillW = std::min<int>(t.status.right - t.status.left, std::max(S(120), TextW(dc, upLabel) + S(24)));
            RECT pill{t.status.right - pillW, t.status.top, t.status.right, t.status.bottom};
            button(pill, upLabel.c_str(), false, g_mw->toolbarHover == (int)kUpdatePill, true);
            if (updater::Downloading()) {
                int barW = (int)((pill.right - pill.left - S(12)) * std::clamp(updater::Progress(), 0.0, 1.0));
                RECT track{pill.left + S(6), pill.bottom - S(4), pill.right - S(6), pill.bottom - S(2)};
                Fill(dc, track, RGB(0, 90, 180));
                if (barW > 0) Fill(dc, {track.left, track.top, track.left + barW, track.bottom}, RGB(255, 255, 255));
            }
        } else {
            std::wstring status = g_mw->notice;
            if (status.empty() && g_cursorRenderBusy.load()) status = Tr(L"Создаём видео со слежением...", L"Creating cursor-follow video...");
            if (status.empty()) status = std::to_wstring(g_mw->files.size()) + L" / " + std::to_wstring(g_mw->catalog.size());
            SetTextColor(dc, kTextOff);
            RECT r = t.status;
            DrawTextW(dc, status.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
}

void PaintMenu(HDC dc, HDC mem) {
    Fill(dc, {0, 0, g_mw->panelW - 1, 1 << 20}, kBg);
    Fill(dc, {g_mw->panelW - 1, 0, g_mw->panelW, 1 << 20}, kBorder);
    SelectObject(dc, g_mw->menuFont);
    for (int i = 0; i < kNumItems; ++i) {
        const Item& it = kItems[i];
        RECT r = ItemRect(i);
        if (!it.ru) {
            int y = (r.top + r.bottom) / 2 - 1;
            Fill(dc, {r.left + S(4), y, r.right - S(4), y + 1}, kSepDark);
            Fill(dc, {r.left + S(4), y + 1, r.right - S(4), y + 2}, kSepLight);
            continue;
        }
        bool on = Enabled(it);
        if (on && (i == g_mw->hover || i == g_mw->open)) {
            RECT h{r.left + S(2), r.top, r.right - S(2), r.bottom};
            Fill(dc, h, kHover);
            if (i == g_mw->open || i == g_mw->pressed) FrameRectC(dc, h, kAccent);
        }
        DrawIcon(dc, mem, it.icon, on, S(kIconX), r.top + (r.bottom - r.top - S(16)) / 2);
        SetTextColor(dc, on ? kText : kTextOff);
        RECT tr{S(kIconX + 16 + kTextGap), r.top, r.right - S(kArrowW), r.bottom};
        DrawTextW(dc, Text(it), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (it.sub) {  // WinForms-style filled arrow
            int cx = r.right - S(10), cy = (r.top + r.bottom) / 2, a = std::max(3, S(3));
            POINT tri[3] = {{cx - a / 2, cy - a}, {cx - a / 2, cy + a}, {cx - a / 2 + a, cy}};
            COLORREF c = on ? kText : kTextOff;
            SetDCBrushColor(dc, c);
            SetDCPenColor(dc, c);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
            Polygon(dc, tri, 3);
            SelectObject(dc, ob);
            SelectObject(dc, op);
        }
    }
}

void PaintTable(HDC dc, RECT area) {
    Fill(dc, area, kLightBg);
    if (g_mw->rows.empty()) return;
    SelectObject(dc, g_mw->tableFont);
    const wchar_t* h1 = Tr(L"Хоткей", L"Hotkey");
    const wchar_t* h2 = Tr(L"Описание", L"Description");
    int padX = S(12), bar = S(4);
    int c1 = TextW(dc, h1), c2 = TextW(dc, h2);
    for (const HotkeyInfo& r : g_mw->rows) {
        c1 = std::max(c1, TextW(dc, r.keys));
        c2 = std::max(c2, TextW(dc, r.description));
    }
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    int rowH = tm.tmHeight + S(12);
    c1 += padX * 2 + bar + 2;
    c2 += padX * 2 + 2;
    int w = c1 + c2 + 1, h = rowH * (int)(g_mw->rows.size() + 1) + 1;
    // ShareX docks the hotkey list at the top-left of the content area.
    int x = area.left + S(22);
    int y = area.top + S(14);

    auto cell = [&](RECT r, const std::wstring& s, UINT align, COLORREF bg) {
        Fill(dc, r, bg);
        RECT t{r.left + padX, r.top, r.right - padX, r.bottom};
        DrawTextW(dc, s.c_str(), (int)s.size(), &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | align);
    };
    SetTextColor(dc, kText);
    int cy = y + 1;
    cell({x + 1, cy, x + c1, cy + rowH - 1}, h1, DT_CENTER, kDarkBg);
    cell({x + c1 + 1, cy, x + w - 1, cy + rowH - 1}, h2, DT_LEFT, kDarkBg);
    for (const HotkeyInfo& r : g_mw->rows) {
        cy += rowH;
        Fill(dc, {x + 1, cy, x + 1 + bar, cy + rowH - 1}, r.registered ? kOk : kFail);
        cell({x + 1 + bar, cy, x + c1, cy + rowH - 1}, r.keys, DT_CENTER, kBg);
        cell({x + c1 + 1, cy, x + w - 1, cy + rowH - 1}, r.description, DT_LEFT, kBg);
    }
    // Grid lines.
    for (int i = 0; i <= (int)g_mw->rows.size() + 1; ++i) Fill(dc, {x, y + i * rowH, x + w, y + i * rowH + 1}, kBorder);
    Fill(dc, {x, y, x + 1, y + h}, kBorder);
    Fill(dc, {x + c1, y, x + c1 + 1, y + h}, kBorder);
    Fill(dc, {x + w - 1, y, x + w, y + h}, kBorder);
    if (g_mw->flash & 1) FrameRectC(dc, {x - S(2), y - S(2), x + w + S(2), y + h + S(2)}, kAccent, S(2));
}

void FillAlpha(HDC dc, RECT r, COLORREF c, BYTE alpha) {
    void* bits = nullptr;
    HBITMAP px = CreateDib(1, 1, &bits);
    if (!px) return;
    *(uint32_t*)bits = 0xFF000000u | GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c);
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, px);
    BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, 0};
    AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, mem, 0, 0, 1, 1, bf);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(px);
}

const wchar_t* SideLabel(int side) {
    switch (side) {
    case 0: return Tr(L"← Слева", L"← Left");
    case 1: return Tr(L"Справа →", L"Right →");
    case 2: return Tr(L"↑ Сверху", L"↑ Above");
    default: return Tr(L"↓ Снизу", L"↓ Below");
    }
}

// Where a dropped image will go: the tile's half on that side tinted, an accent bar on the edge and a label.
void PaintDropZone(HDC dc, RECT t, int side) {
    int cx = (t.left + t.right) / 2, cy = (t.top + t.bottom) / 2, bar = S(5);
    RECT half = t, edge = t;
    switch (side) {
    case 0: half.right = cx; edge.right = t.left + bar; break;
    case 1: half.left = cx; edge.left = t.right - bar; break;
    case 2: half.bottom = cy; edge.bottom = t.top + bar; break;
    default: half.top = cy; edge.top = t.bottom - bar; break;
    }
    FillAlpha(dc, t, RGB(0, 0, 0), 70);
    FillAlpha(dc, half, kAccent, 90);
    FrameRectC(dc, t, kAccent, std::max(1, S(2) - 1));
    Fill(dc, edge, kAccent);
    SelectObject(dc, g_mw->menuFont);
    const wchar_t* s = SideLabel(side);
    int w = TextW(dc, s) + S(18), h = S(24);
    int lx = (half.left + half.right - w) / 2, ly = (half.top + half.bottom - h) / 2;
    SetDCBrushColor(dc, kAccent);
    SetDCPenColor(dc, kAccent);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
    RoundRect(dc, lx, ly, lx + w, ly + h, S(8), S(8));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    RECT lr{lx, ly, lx + w, ly + h};
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, s, -1, &lr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

void PaintGrid(HDC dc, HDC mem, RECT area) {
    Fill(dc, area, kBg);
    Grid g = GetGrid();
    int saved = SaveDC(dc);
    IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
    int n = (int)g_mw->files.size();
    if (!n) {
        SelectObject(dc, g_mw->tableFont);
        SetTextColor(dc, kText);
        RECT title{area.left + S(20), area.top + S(65), area.right - S(20), area.top + S(98)};
        const bool shelf = g_mw->filter == gallery::Filter::Shelf;
        const wchar_t* text = !g_mw->query.empty() ? Tr(L"Ничего не найдено", L"No matching files")
                              : shelf ? Tr(L"На полке пока пусто", L"Your shelf is empty")
                              : g_mw->catalog.empty() ? Tr(L"Здесь будут ваши скриншоты", L"Your captures will appear here")
                              : Tr(L"Нет файлов этого типа", L"No files of this type");
        DrawTextW(dc, text, -1, &title, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, g_mw->menuFont);
        SetTextColor(dc, kTextOff);
        RECT hint{area.left + S(24), title.bottom + S(6), area.right - S(24), title.bottom + S(60)};
        DrawTextW(dc, !g_mw->query.empty() ? Tr(L"Измените имя или выберите другой фильтр", L"Try another filename or filter")
                                             : Tr(L"Нажмите Ctrl+V, чтобы сохранить картинку на полку", L"Press Ctrl+V to keep an image on your shelf"),
                  -1, &hint, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    }
    int firstRow = std::max(0, (g_mw->scroll - S(kMargin)) / g.stepY);
    int lastRow = std::min(g.rows - 1, (g_mw->scroll + (int)(area.bottom - area.top)) / g.stepY);
    std::vector<ThumbJob> want;
    // Visible rows first, then one row below and one above for smooth scrolling.
    auto queueRow = [&](int r) {
        if (r < 0 || r >= g.rows) return;
        for (int i = r * g.cols; i < std::min(n, (r + 1) * g.cols); ++i)
            if (!g_mw->files[i].done) want.push_back({g_mw->files[i].path, S(kThumbW), S(kThumbH)});
    };
    for (int r = firstRow; r <= lastRow; ++r) queueRow(r);
    queueRow(lastRow + 1);
    queueRow(firstRow - 1);
    SetThumbQueue(std::move(want));

    for (int i = firstRow * g.cols; i < std::min(n, (lastRow + 1) * g.cols); ++i) {
        const Entry& e = g_mw->files[i];
        RECT t = TileRect(g, i);
        bool sel = IsPicked(e.path), hov = i == g_mw->hoverTile;
        Fill(dc, t, sel ? kTileSel : hov ? kTileHover : kTile);
        if (sel) FrameRectC(dc, t, kAccent, std::max(1, S(2) - 1));
        else FrameRectC(dc, t, hov ? RGB(70, 70, 70) : kBorder);
        RECT box{t.left + S(kTilePad), t.top + S(kTilePad), t.left + S(kTilePad) + S(kThumbW), t.top + S(kTilePad) + S(kThumbH)};
        if (e.thumb) {
            int x = box.left + (S(kThumbW) - e.tw) / 2, y = box.top + (S(kThumbH) - e.th) / 2;
            HGDIOBJ old = SelectObject(mem, e.thumb);
            BitBlt(dc, x, y, e.tw, e.th, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
        } else if (e.done) {  // no thumbnail available: show the extension
            SelectObject(dc, g_mw->tableFont);
            SetTextColor(dc, kTextOff);
            std::wstring ext = e.name.substr(e.name.find_last_of(L'.') + 1);
            CharUpperW(ext.data());
            DrawTextW(dc, ext.c_str(), -1, &box, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
        if (e.kind != K_Image) {
            SelectObject(dc, g_mw->badgeFont);
            std::wstring badge = e.kind == K_Gif ? L"GIF" : e.name.substr(e.name.find_last_of(L'.') + 1);
            CharUpperBuffW(badge.data(), (DWORD)badge.size());
            const wchar_t* b = badge.c_str();
            int bw = TextW(dc, b) + S(8), bh = S(15);
            RECT br{box.left + S(4), box.top + S(4), box.left + S(4) + bw, box.top + S(4) + bh};
            Fill(dc, br, e.kind == K_Gif ? RGB(0, 120, 215) : RGB(200, 60, 60));
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, b, -1, &br, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
        if (e.shelf) {
            SelectObject(dc, g_mw->badgeFont);
            const wchar_t* label = Tr(L"ПОЛКА", L"SHELF");
            const int bw = TextW(dc, label) + S(8), bh = S(15);
            RECT br{box.left + S(4), box.bottom - S(4) - bh, box.left + S(4) + bw, box.bottom - S(4)};
            Fill(dc, br, RGB(36, 83, 112));
            SetTextColor(dc, RGB(200, 230, 255));
            DrawTextW(dc, label, -1, &br, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
        SelectObject(dc, g_mw->tableFont);
        SetTextColor(dc, kText);
        RECT tr{t.left + S(kTilePad) + S(2), box.bottom, t.right - S(kTilePad) - S(2), t.bottom - S(1)};
        DrawTextW(dc, e.name.c_str(), (int)e.name.size(), &tr,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (sel && g_mw->picked.size() >= 2) {  // combine order
            int order = (int)(std::find(g_mw->picked.begin(), g_mw->picked.end(), e.path) - g_mw->picked.begin()) + 1;
            std::wstring s = std::to_wstring(order);
            SelectObject(dc, g_mw->badgeFont);
            int d = std::max(S(18), TextW(dc, s) + S(8));
            RECT br{box.right - S(4) - d, box.top + S(4), box.right - S(4), box.top + S(4) + S(18)};
            SetDCBrushColor(dc, kAccent);
            SetDCPenColor(dc, RGB(255, 255, 255));
            HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
            RoundRect(dc, br.left, br.top, br.right, br.bottom, S(18), S(18));
            SelectObject(dc, ob);
            SelectObject(dc, op);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, s.c_str(), -1, &br, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
        if (i == g_mw->dropTile) PaintDropZone(dc, t, g_mw->dropSide);
    }
    RECT th = ScrollThumb(g);
    if (th.bottom > th.top) Fill(dc, th, g_mw->scrollDrag ? RGB(150, 150, 150) : kScroll);
    RestoreDC(dc, saved);
}

void Paint(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        HDC mem = CreateCompatibleDC(wdc);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, g_mw->menuFont);
        PaintMenu(dc, mem);
        RECT area{g_mw->panelW, 0, rc.right, rc.bottom};
        if (GridMode()) {
            PaintToolbar(dc);
            area.top = GetToolbarLayout().height;
            PaintGrid(dc, mem, area);
        }
        else PaintTable(dc, area);
        SelectObject(dc, oldFont);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        DeleteDC(mem);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    EndPaint(h, &ps);
    ++g_mw->paintCount;
}

void Redraw() { InvalidateRect(g_mw->hwnd, nullptr, FALSE); }

// Thumbnail result from the worker, on the UI thread.
void OnThumb(unsigned session, const std::wstring& path, int w, int h, HBITMAP bmp, int bw, int bh) {
    if (!g_mw || g_mw->session != session || w != S(kThumbW) || h != S(kThumbH)) {
        if (bmp) DeleteObject(bmp);
        return;
    }
    auto& files = g_mw->files;
    auto it = std::find_if(files.begin(), files.end(), [&](const Entry& e) { return e.path == path; });
    if (it == files.end() || it->done) {
        if (bmp) DeleteObject(bmp);
        return;
    }
    it->thumb = bmp;
    it->tw = bw;
    it->th = bh;
    it->done = true;
    // Bound memory: drop the thumbnails farthest from this one when too many are cached.
    size_t cached = 0;
    for (const Entry& e : files) cached += e.thumb != nullptr;
    if (cached > kMaxThumbs) {
        int at = (int)(it - files.begin());
        for (int d = (int)files.size(); d > 0 && cached > kMaxThumbs * 3 / 4; --d)
            for (int i : {at - d, at + d})
                if (i >= 0 && i < (int)files.size() && files[i].thumb) {
                    FreeThumb(files[i]);
                    --cached;
                }
    }
    if (GridMode()) {
        Grid g = GetGrid();
        RECT t = TileRect(g, (int)(it - files.begin()));
        InvalidateRect(g_mw->hwnd, &t, FALSE);
    }
}

// ---------- dark popup menus (undocumented uxtheme ordinals, Win10 1903+) ----------

void AllowDarkMenus(HWND h) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetMode = int(WINAPI*)(int);
    using AllowWnd = BOOL(WINAPI*)(HWND, BOOL);
    using Flush = void(WINAPI*)();
    static auto setMode = (SetMode)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    static auto allow = (AllowWnd)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(133));
    static auto flush = (Flush)(void*)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1);  // AllowDark: only windows opted in below, following the system theme
    if (allow) allow(h, TRUE);
    if (flush) flush();
}

// ---------- file actions ----------

const Entry* Selected() {
    return g_mw->sel >= 0 && g_mw->sel < (int)g_mw->files.size() ? &g_mw->files[g_mw->sel] : nullptr;
}

void ShellOpen(const std::wstring& path) { ShellExecuteW(g_mw->hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void OpenInEditor(const std::wstring& path) {
    auto img = std::make_shared<Image>();
    if (LoadImageFile(path, *img)) OpenEditor(img, path);
    else ShellOpen(path);
}

void OpenEntry(const Entry& e) {
    if (e.kind == K_Image) OpenInEditor(e.path);
    else ShellOpen(e.path);
}

void CopyImage(const std::wstring& path) {
    Image img;
    if (LoadImageFile(path, img)) CopyImageToClipboard(img);
}

void CopyPathText(const std::wstring& path) {
    if (!OpenClipboard(g_mw->hwnd)) return;
    EmptyClipboard();
    size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, path.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

void ShowInFolder(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        CoTaskMemFree(pidl);
        if (SUCCEEDED(hr)) return;
    }
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void RemoveEntry(int i) {
    const std::wstring removed = g_mw->files[i].path;
    std::erase_if(g_mw->catalog, [&](const gallery::Item& item) { return item.path == removed; });
    std::erase(g_mw->picked, g_mw->files[i].path);
    if (g_mw->dropTile == i) g_mw->dropTile = -1;
    FreeThumb(g_mw->files[i]);
    g_mw->files.erase(g_mw->files.begin() + i);
    if (g_mw->sel == i) g_mw->sel = std::min(i, (int)g_mw->files.size() - 1);
    else if (g_mw->sel > i) --g_mw->sel;
    if (g_mw->anchor == i) g_mw->anchor = g_mw->sel;
    else if (g_mw->anchor > i) --g_mw->anchor;
    g_mw->hoverTile = -1;
    ClampScroll();
}

void DeletePaths(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    std::wstring from;
    for (const std::wstring& p : paths) from += p + L'\0';  // double-null terminated list
    SHFILEOPSTRUCTW op{};
    op.hwnd = g_mw->hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO;
    SHFileOperationW(&op);
    if (!g_mw) return;
    bool any = false;
    for (const std::wstring& path : paths) {
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        for (int k = 0; k < (int)g_mw->files.size(); ++k)
            if (g_mw->files[k].path == path) {
                RemoveEntry(k);
                any = true;
                break;
            }
    }
    if (any) Redraw();
}

void DeleteEntry(int i) {
    const std::wstring& path = g_mw->files[i].path;
    if (g_mw->picked.size() > 1 && IsPicked(path)) DeletePaths(std::vector<std::wstring>(g_mw->picked));
    else DeletePaths({path});
}

// Dragging a tile of a multi-selection drags every selected file, in selection order.
void StartDrag(int i) {
    std::vector<std::wstring> paths;
    if (g_mw->picked.size() > 1 && IsPicked(g_mw->files[i].path)) paths = g_mw->picked;
    else paths = {g_mw->files[i].path};
    std::vector<PIDLIST_ABSOLUTE> pidls;
    HRESULT hr = S_OK;
    for (const std::wstring& path : paths) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr);
        if (FAILED(hr)) {
            Log(L"mainwindow: SHParseDisplayName failed 0x%08X for %s", (unsigned)hr, path.c_str());
            continue;
        }
        pidls.push_back(pidl);
    }
    if (pidls.empty()) return;
    ComPtr<IShellItemArray> items;
    ComPtr<IDataObject> data;
    hr = SHCreateShellItemArrayFromIDLists((UINT)pidls.size(), (PCIDLIST_ABSOLUTE_ARRAY)pidls.data(), &items);
    for (PIDLIST_ABSOLUTE p : pidls) CoTaskMemFree(p);
    if (SUCCEEDED(hr)) hr = items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    if (FAILED(hr) || !data) {
        Log(L"mainwindow: no data object for drag, 0x%08X", (unsigned)hr);
        return;
    }
    DWORD effect = 0;
    hr = SHDoDragDrop(g_mw->hwnd, data.Get(), nullptr, DROPEFFECT_COPY, &effect);
    if (hr == CO_E_NOTINITIALIZED) Log(L"mainwindow: SHDoDragDrop needs OleInitialize on the UI thread (CO_E_NOTINITIALIZED)");
    else if (FAILED(hr)) Log(L"mainwindow: SHDoDragDrop failed 0x%08X", (unsigned)hr);
}

void SelectTile(int i);

// Combined result: saved, put at the top of the grid by MainWindowNotifyCapture; select it.
void SelectPath(const std::wstring& path) {
    if (!g_mw) return;
    for (int k = 0; k < (int)g_mw->files.size(); ++k)
        if (g_mw->files[k].path == path) {
            SelectTile(k);
            return;
        }
}

std::vector<std::wstring> PickedImages() {
    std::vector<std::wstring> v;
    for (const std::wstring& p : g_mw->picked)
        for (const Entry& e : g_mw->files)
            if (e.path == p && IsImageEntry(e)) v.push_back(p);
    return v;
}

void CombineDialog() {
    std::vector<std::wstring> paths = PickedImages();
    if (paths.size() < 2) return;
    CombineOptions o = LoadCombineOptions();
    if (ShowCombineDialog(g_mw->hwnd, paths, o)) CombineFilesAsync(std::move(paths), o, SelectPath);
}

bool HasCursorTrack(const Entry& entry) {
    if (entry.kind != K_Mp4) return false;
    const DWORD attributes = GetFileAttributesW(cursor_video::TrackPath(entry.path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring NewFocusVideoPath(const std::wstring& original) {
    const size_t slash = original.find_last_of(L"\\/"), dot = original.find_last_of(L'.');
    const size_t end = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? dot : original.size();
    const std::wstring stem = original.substr(0, end) + L"_focus";
    for (unsigned index = 0; index < 10000; ++index) {
        const std::wstring candidate = stem + (index ? L" (" + std::to_wstring(index) + L")" : L"") + L".mp4";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return candidate;
        return {};
    }
    return {};
}

void StartCursorFollow(const std::wstring& original) {
    if (!g_mw) return;
    Kind kind;
    if (!IsCaptureExt(original, kind) || kind != K_Mp4) {
        g_mw->notice = Tr(L"Выберите видео, записанное Zipshot", L"Choose a video recorded by Zipshot");
        Redraw();
        return;
    }
    Entry entry;
    entry.kind = kind;
    entry.path = original;
    if (!HasCursorTrack(entry)) {
        g_mw->notice = Tr(L"Нужна новая запись Zipshot с треком курсора", L"A new Zipshot recording with a cursor track is required");
        Redraw();
        return;
    }
    if (RecorderActive() || RecorderBusy()) {
        g_mw->notice = Tr(L"Сначала завершите запись экрана", L"Finish the screen recording first");
        Redraw();
        return;
    }
    const std::wstring ffmpeg = FindFFmpeg();
    if (ffmpeg.empty()) {
        g_mw->notice = Tr(L"Для обработки видео нужен FFmpeg", L"FFmpeg is required to process this video");
        Redraw();
        return;
    }
    if (g_cursorRenderBusy.exchange(true)) {
        g_mw->notice = Tr(L"Видео со слежением уже создаётся", L"A cursor-follow video is already being created");
        Redraw();
        return;
    }
    g_mw->notice = Tr(L"Создаём видео со слежением...", L"Creating cursor-follow video...");
    Redraw();
    try {
        std::thread([original, ffmpeg] {
            cursor_video::Result result;
            std::wstring output;
            try {
                cursor_video::Track track;
                if (!cursor_video::LoadTrack(original, track, &result.error)) {
                    if (result.error.empty()) result.error = Tr(L"Не удалось прочитать трек курсора", L"Could not read the cursor track");
                } else if ((output = NewFocusVideoPath(original)).empty()) {
                    result.error = Tr(L"Не удалось выбрать новое имя видео", L"Could not choose a new video filename");
                } else result = cursor_video::Render(ffmpeg, original, track, output, true);
            } catch (...) {
                result.error = Tr(L"Не удалось создать видео со слежением", L"Could not create the cursor-follow video");
            }
            RunOnMain([result = std::move(result), output = std::move(output)] {
                g_cursorRenderBusy.store(false);
                if (!g_mw) return;
                if (result.ok) {
                    g_mw->showTable = false;
                    g_mw->filter = gallery::Filter::Videos;
                    SetWindowTextW(g_mw->search, L"");
                    g_mw->query.clear();
                    Rescan();
                    SelectPath(output);
                    g_mw->notice = Tr(L"Видео со слежением готово. Исходник сохранён", L"Cursor-follow video is ready. Original retained");
                } else g_mw->notice = result.error.empty()
                    ? Tr(L"Не удалось обработать видео. Исходник сохранён", L"Could not process the video. Original retained") : result.error;
                Redraw();
            });
        }).detach();
    } catch (...) {
        g_cursorRenderBusy.store(false);
        g_mw->notice = Tr(L"Не удалось начать обработку видео", L"Could not start video processing");
        Redraw();
    }
}

HMENU BuildTileMenu(const Entry& e, bool multi) {
    HMENU m = CreatePopupMenu();
    if (multi && PickedImages().size() >= 2) {
        AppendMenuW(m, MF_STRING, M_Combine, Tr(L"Объединить изображения...", L"Combine images..."));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, M_Open, Tr(L"Открыть", L"Open"));
    if (e.kind == K_Image) AppendMenuW(m, MF_STRING, M_Editor, Tr(L"Открыть в редакторе", L"Open in editor"));
    if (e.kind == K_Mp4) {
        const bool track = HasCursorTrack(e);
        AppendMenuW(m, MF_STRING | (!track || g_cursorRenderBusy.load() || RecorderActive() || RecorderBusy() ? MF_GRAYED : 0), M_CursorFollow,
                    Tr(L"Сделать видео со слежением за курсором...", L"Create cursor-follow video..."));
        if (!track) AppendMenuW(m, MF_STRING | MF_GRAYED, 0,
                               Tr(L"Нужна новая запись Zipshot с треком курсора", L"Requires a new Zipshot recording with a cursor track"));
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (e.kind == K_Image) AppendMenuW(m, MF_STRING, M_CopyImage, Tr(L"Копировать изображение\tCtrl+C", L"Copy image\tCtrl+C"));
    AppendMenuW(m, MF_STRING, M_CopyFile, Tr(L"Копировать файл", L"Copy file"));
    AppendMenuW(m, MF_STRING, M_CopyPath, Tr(L"Копировать путь", L"Copy file path"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, M_ShowFolder, Tr(L"Показать в папке", L"Show in folder"));
    std::wstring del = multi ? std::wstring(Tr(L"Удалить выбранные (", L"Delete selected (")) + std::to_wstring(g_mw->picked.size()) +
                                   L")\tDelete"
                             : Tr(L"Удалить\tDelete", L"Delete\tDelete");
    AppendMenuW(m, MF_STRING, M_Delete, del.c_str());
    SetMenuDefaultItem(m, multi && PickedImages().size() >= 2 ? M_Combine : e.kind == K_Image ? M_Editor : M_Open, FALSE);
    return m;
}

void TileMenu(int i, POINT screen) {
    if (i < 0 || i >= (int)g_mw->files.size()) return;
    const Entry e = g_mw->files[i];
    const bool multi = g_mw->picked.size() > 1 && IsPicked(e.path);
    HMENU m = BuildTileMenu(e, multi);
    HWND h = g_mw->hwnd;
    UINT id = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, h,
                                     nullptr);
    DestroyMenu(m);
    if (!g_mw) return;
    switch (id) {
    case M_Combine: CombineDialog(); break;
    case M_Open: ShellOpen(e.path); break;
    case M_Editor: OpenInEditor(e.path); break;
    case M_CopyImage: CopyImage(e.path); break;
    case M_CopyFile: CopyFileToClipboard(e.path); break;
    case M_CopyPath: CopyPathText(e.path); break;
    case M_CursorFollow: StartCursorFollow(e.path); break;
    case M_ShowFolder: ShowInFolder(e.path); break;
    case M_Delete: {
        for (int k = 0; k < (int)g_mw->files.size(); ++k)
            if (g_mw->files[k].path == e.path) {
                DeleteEntry(k);
                break;
            }
        break;
    }
    default: break;
    }
}

// ---------- menu actions ----------

void ScheduleCommand(int cmd, bool restore) {
    g_mw->pendingCmd = cmd;
    g_mw->pendingRestore = restore;
    ShowWindow(g_mw->hwnd, SW_HIDE);
    SetTimer(g_mw->hwnd, kTimerRun, 200, nullptr);
}

void ShowSubmenu(int i) {
    HWND h = g_mw->hwnd;
    HMENU m = CreatePopupMenu();
    if (kItems[i].act == A_Capture) {
        for (UINT k = 0; k < std::size(kCapture); ++k) {
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP;
            mi.wID = kCapBase + k;
            mi.dwTypeData = (LPWSTR)Tr(kCapture[k].ru, kCapture[k].en);
            mi.hbmpItem = Icon(kCapture[k].icon).normal;
            InsertMenuItemW(m, k, TRUE, &mi);
        }
    } else if (kItems[i].act == A_AppSettings) {
        HMENU lang = CreatePopupMenu();
        AppendMenuW(lang, MF_STRING, kLangRu, L"Русский");
        AppendMenuW(lang, MF_STRING, kLangEn, L"English");
        CheckMenuRadioItem(lang, kLangRu, kLangEn, IsEnglish() ? kLangEn : kLangRu, MF_BYCOMMAND);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)lang, Tr(L"Язык", L"Language"));  // destroyed with m
        AppendMenuW(m, MF_STRING | (SoundEnabled() ? MF_CHECKED : 0), kSound, Tr(L"Звуки", L"Sounds"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kImport, Tr(L"Импорт настроек из ShareX", L"Import ShareX settings"));
        AppendMenuW(m, MF_STRING, kOpenIni, Tr(L"Открыть zipshot.ini", L"Open zipshot.ini"));
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kAutostart,
                    Tr(L"Запускать при входе в Windows", L"Run at Windows startup"));
        AppendMenuW(m, MF_STRING | (GetSettingBool(L"AutoUpdate", true) ? MF_CHECKED : 0), kAutoUpdate,
                    Tr(L"Автопроверка обновлений", L"Check for updates automatically"));
        if (updater::HasUpdate() && !updater::Downloading()) {
            std::wstring inst = std::wstring(Tr(L"Установить обновление ", L"Install update ")) + updater::AvailableVersion();
            AppendMenuW(m, MF_STRING, kInstallUpdate, inst.c_str());
        } else {
            AppendMenuW(m, MF_STRING | ((updater::Checking() || updater::Downloading()) ? MF_GRAYED : 0), kCheckUpdate,
                        updater::Downloading() ? Tr(L"Скачивание обновления...", L"Downloading update...")
                        : updater::Checking()  ? Tr(L"Проверка обновлений...", L"Checking for updates...")
                                               : Tr(L"Проверить обновления", L"Check for updates"));
        }
    } else {
        for (UINT k = 0; k < std::size(kAfter); ++k)
            AppendMenuW(m, MF_STRING | (GetSettingBool(kAfter[k].key, kAfter[k].def) ? MF_CHECKED : 0), kAfterBase + k,
                        Tr(kAfter[k].ru, kAfter[k].en));
    }
    RECT r = ItemRect(i);
    MapWindowPoints(h, nullptr, (POINT*)&r, 2);
    TPMPARAMS tp{sizeof(tp), r};
    g_mw->open = i;
    InvalidateRect(h, nullptr, FALSE);
    UpdateWindow(h);
    UINT id = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_VERTICAL,
                                     r.right, r.top, h, &tp);
    DestroyMenu(m);
    if (!g_mw) return;
    g_mw->open = -1;
    g_mw->hover = -1;
    InvalidateRect(h, nullptr, FALSE);
    if (id >= kCapBase && id < kCapBase + std::size(kCapture)) {
        const CaptureEntry& c = kCapture[id - kCapBase];
        ScheduleCommand(c.cmd, c.restore);
    } else if (id >= kAfterBase && id < kAfterBase + std::size(kAfter)) {
        const ToggleEntry& t = kAfter[id - kAfterBase];
        SetSettingBool(t.key, !GetSettingBool(t.key, t.def));
    } else if (id == kLangRu || id == kLangEn) {
        RunCommand(id == kLangEn ? C_LangEn : C_LangRu);
    } else if (id == kImport) {
        RunCommand(C_ImportShareX);
    } else if (id == kAutostart) {
        SetAutostart(!AutostartEnabled());
    } else if (id == kSound) {
        RunCommand(C_ToggleSound);
    } else if (id == kAutoUpdate) {
        SetSettingBool(L"AutoUpdate", !GetSettingBool(L"AutoUpdate", true));
    } else if (id == kCheckUpdate) {
        RunCommand(C_CheckUpdate);
    } else if (id == kInstallUpdate) {
        RunCommand(C_InstallUpdate);
    } else if (id == kOpenIni) {
        RunCommand(C_Settings);
    }
}

void Activate(int i) {
    switch (kItems[i].act) {
    case A_Capture:
    case A_AfterCapture:
    case A_AppSettings: ShowSubmenu(i); break;
    case A_Settings: RunCommand(C_Settings); break;
    case A_Folder: RunCommand(C_Folder); break;
    case A_Hotkeys:
        RefreshRows();
        g_mw->showTable = !g_mw->showTable;
        LayoutToolbar();
        if (!GridMode()) {
            g_mw->flash = 6;
            SetTimer(g_mw->hwnd, kTimerFlash, 150, nullptr);
        }
        InvalidateRect(g_mw->hwnd, nullptr, FALSE);
        break;
    case A_About: {
        std::wstring about = std::wstring(L"Zipshot ") + updater::kAppVersion +
            Tr(L" — нативная замена ShareX\n\n"
               L"Звуки: ShareX (GPLv3), https://getsharex.com\n"
               L"Иконки: Fugue Icons, Yusuke Kamiyamane (CC BY 3.0)",
               L" — native ShareX replacement\n\n"
               L"Sounds: ShareX (GPLv3), https://getsharex.com\n"
               L"Icons: Fugue Icons, Yusuke Kamiyamane (CC BY 3.0)");
        MessageBoxW(g_mw->hwnd, about.c_str(), Tr(L"О программе", L"About"), MB_OK | MB_ICONINFORMATION);
        break;
    }
    default: break;
    }
}

void ApplyDpi(UINT dpi) {
    bool changed = g_mw->dpi != dpi;
    g_mw->dpi = dpi;
    FreeIcons();
    CreateFonts();
    Measure();
    if (changed) FreeAllThumbs();  // re-requested at the new size
    LayoutToolbar();
}

void SetHoverTile(int i) {
    if (i == g_mw->hoverTile) return;
    if (GridMode()) {
        Grid g = GetGrid();
        for (int k : {g_mw->hoverTile, i})
            if (k >= 0 && k < (int)g_mw->files.size()) {
                RECT t = TileRect(g, k);
                InvalidateRect(g_mw->hwnd, &t, FALSE);
            }
    }
    g_mw->hoverTile = i;
}

void SelectTile(int i) {
    bool valid = i >= 0 && i < (int)g_mw->files.size();
    if (i == g_mw->sel && (valid ? g_mw->picked.size() == 1 && g_mw->picked[0] == g_mw->files[i].path : g_mw->picked.empty()))
        return;
    g_mw->sel = valid ? i : -1;
    g_mw->anchor = g_mw->sel;
    g_mw->picked.clear();
    if (valid) g_mw->picked.push_back(g_mw->files[i].path);
    EnsureVisible(g_mw->sel);
    Redraw();
}

void ToggleTile(int i) {
    const std::wstring& p = g_mw->files[i].path;
    if (IsPicked(p)) std::erase(g_mw->picked, p);
    else g_mw->picked.push_back(p);
    g_mw->sel = g_mw->anchor = i;
    Redraw();
}

// Shift: anchor..i, added oldest first (the grid is newest first) so a range combines in capture order.
void RangeSelect(int i, bool add) {
    int a = g_mw->anchor >= 0 && g_mw->anchor < (int)g_mw->files.size() ? g_mw->anchor : i;
    if (!add) g_mw->picked.clear();
    int first = g_mw->oldestFirst ? std::min(a, i) : std::max(a, i);
    int last = g_mw->oldestFirst ? std::max(a, i) : std::min(a, i);
    for (int k = first;; k += g_mw->oldestFirst ? 1 : -1) {
        if (!IsPicked(g_mw->files[k].path)) g_mw->picked.push_back(g_mw->files[k].path);
        if (k == last) break;
    }
    g_mw->sel = i;
    g_mw->anchor = a;
    EnsureVisible(i);
    Redraw();
}

void SelectAll() {
    g_mw->picked.clear();
    if (g_mw->oldestFirst) for (const Entry& e : g_mw->files) g_mw->picked.push_back(e.path);
    else for (int k = (int)g_mw->files.size() - 1; k >= 0; --k) g_mw->picked.push_back(g_mw->files[k].path);
    Redraw();
}

void ScrollBy(int dy) {
    int before = g_mw->scroll;
    g_mw->scroll += dy;
    ClampScroll();
    if (g_mw->scroll != before) Redraw();
}

void ScrollToThumbY(int y) {
    Grid g = GetGrid();
    RECT a = ContentRect();
    RECT th = ScrollThumb(g);
    int trackH = (a.bottom - a.top) - S(4), thumbH = th.bottom - th.top;
    if (trackH - thumbH <= 0) return;
    int pos = y - g_mw->scrollGrab - (a.top + S(2));
    g_mw->scroll = (int)((long long)pos * MaxScroll(g) / (trackH - thumbH));
    ClampScroll();
    Redraw();
}

bool KeyNav(WPARAM vk) {
    if (!GridMode()) return false;
    int n = (int)g_mw->files.size();
    if (!n) return false;
    Grid g = GetGrid();
    RECT a = ContentRect();
    int pageRows = std::max(1, (int)(a.bottom - a.top) / g.stepY);
    int s = g_mw->sel;
    switch (vk) {
    case VK_LEFT: s = s < 0 ? 0 : s - 1; break;
    case VK_RIGHT: s = s < 0 ? 0 : s + 1; break;
    case VK_UP: s = s < 0 ? 0 : s - g.cols; break;
    case VK_DOWN: s = s < 0 ? 0 : s + g.cols; break;
    case VK_PRIOR: s = s < 0 ? 0 : s - g.cols * pageRows; break;
    case VK_NEXT: s = s < 0 ? 0 : s + g.cols * pageRows; break;
    case VK_HOME: s = 0; break;
    case VK_END: s = n - 1; break;
    default: return false;
    }
    s = std::clamp(s, 0, n - 1);
    if (GetKeyState(VK_SHIFT) & 0x8000) RangeSelect(s, false);
    else SelectTile(s);
    return true;
}

// ---------- drop a picture on a tile: combine ----------

bool IsDropImage(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const wchar_t* e = path.c_str() + dot + 1;
    for (const wchar_t* x : {L"png", L"jpg", L"jpeg", L"bmp", L"gif"})
        if (!_wcsicmp(e, x)) return true;
    return false;
}

std::vector<std::wstring> DroppedImages(IDataObject* data) {
    std::vector<std::wstring> v;
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (!data || FAILED(data->GetData(&fe, &sm))) return v;
    if (auto drop = (HDROP)GlobalLock(sm.hGlobal)) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            std::wstring p(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
            p.resize(DragQueryFileW(drop, i, p.data(), (UINT)p.size()));
            if (IsDropImage(p) && !(GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) v.push_back(p);
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return v;
}

// Diagonals split the tile into four triangles; the one under the cursor is the side the dropped image goes to.
int DropSide(RECT t, POINT p) {
    double dx = (p.x - (t.left + t.right) / 2.0) / std::max(1L, (t.right - t.left) / 2);
    double dy = (p.y - (t.top + t.bottom) / 2.0) / std::max(1L, (t.bottom - t.top) / 2);
    if (std::fabs(dx) >= std::fabs(dy)) return dx < 0 ? 0 : 1;
    return dy < 0 ? 2 : 3;
}

class DropTarget : public IDropTarget {
public:
    DropTarget() { CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_)); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return (ULONG)r;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        files_ = DroppedImages(data);
        Update(pt, effect, true);
        if (helper_) helper_->DragEnter(g_mw ? g_mw->hwnd : nullptr, data, (POINT*)&pt, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        Update(pt, effect, true);
        if (helper_) helper_->DragOver((POINT*)&pt, *effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        SetZone(-1, -1);
        files_.clear();
        if (helper_) helper_->DragLeave();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
        if (files_.empty()) files_ = DroppedImages(data);
        Update(pt, effect, false);
        if (helper_) helper_->Drop(data, (POINT*)&pt, *effect);
        if (g_mw && *effect != DROPEFFECT_NONE) {
            std::wstring target = g_mw->files[g_mw->dropTile].path;
            int side = g_mw->dropSide;
            std::vector<std::wstring> paths;
            if (side == 1 || side == 3) paths.push_back(target);
            paths.insert(paths.end(), files_.begin(), files_.end());
            if (side == 0 || side == 2) paths.push_back(target);
            CombineOptions o;
            o.horizontal = side <= 1;
            o.alignment = 1;
            o.space = 0;
            o.wrapAfter = 0;
            o.autoFill = true;
            CombineFilesAsync(std::move(paths), o, SelectPath);
        }
        SetZone(-1, -1);
        files_.clear();
        return S_OK;
    }

private:
    void Update(POINTL pt, DWORD* effect, bool autoscroll) {
        DWORD allowed = *effect;
        *effect = DROPEFFECT_NONE;
        if (!g_mw) return;
        int tile = -1, side = -1;
        if (GridMode() && !files_.empty()) {
            POINT p{pt.x, pt.y};
            ScreenToClient(g_mw->hwnd, &p);
            RECT a = ContentRect();
            if (autoscroll && PtInRect(&a, p)) {  // OLE calls DragOver repeatedly, so this scrolls while held at an edge
                int edge = S(28), before = g_mw->scroll;
                if (p.y < a.top + edge) g_mw->scroll -= S(14);
                else if (p.y > a.bottom - edge) g_mw->scroll += S(14);
                ClampScroll();
                if (g_mw->scroll != before) Redraw();
            }
            int t = HitTile(p);
            bool self = t >= 0 && std::any_of(files_.begin(), files_.end(), [&](const std::wstring& f) {
                            return !_wcsicmp(f.c_str(), g_mw->files[t].path.c_str());
                        });
            if (t >= 0 && !self && IsImageEntry(g_mw->files[t]) && (allowed & DROPEFFECT_COPY)) {
                tile = t;
                side = DropSide(TileRect(GetGrid(), t), p);
                *effect = DROPEFFECT_COPY;
            }
        }
        SetZone(tile, side);
    }
    void SetZone(int tile, int side) {
        if (!g_mw) return;
        if (tile != g_mw->dropTile || side != g_mw->dropSide) {
            int old = g_mw->dropTile;
            g_mw->dropTile = tile;
            g_mw->dropSide = side;
            if (GridMode()) {
                Grid g = GetGrid();
                for (int k : {old, tile})
                    if (k >= 0 && k < (int)g_mw->files.size()) {
                        RECT r = TileRect(g, k);
                        InvalidateRect(g_mw->hwnd, &r, FALSE);
                    }
            }
        }
        if (GetUpdateRect(g_mw->hwnd, nullptr, FALSE)) {  // paint now; the drag loop would delay WM_PAINT
            if (helper_) helper_->Show(FALSE);
            UpdateWindow(g_mw->hwnd);
            if (helper_) helper_->Show(TRUE);
        }
    }
    LONG ref_ = 1;
    ComPtr<IDropTargetHelper> helper_;
    std::vector<std::wstring> files_;
};

LRESULT CALLBACK MainWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!g_mw) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(h);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) {
            RefreshRows();
            if (g_mw->search) Rescan();
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_CTLCOLOREDIT:
        if ((HWND)l == g_mw->search) {
            SetTextColor((HDC)w, kText);
            SetBkColor((HDC)w, kLightBg);
            return (LRESULT)g_mw->searchBrush;
        }
        break;
    case WM_COMMAND: {
        const UINT id = LOWORD(w);
        if (id == kSearch && HIWORD(w) == EN_CHANGE) {
            const int n = GetWindowTextLengthW(g_mw->search);
            std::wstring query((size_t)n + 1, L'\0');
            GetWindowTextW(g_mw->search, query.data(), n + 1);
            query.resize(n);
            g_mw->query = std::move(query);
            g_mw->notice.clear();
            RebuildView(true);
            return 0;
        }
        if (id == kPaste || id == kNewest || id == kOldest || (id >= kFilterBase && id < kFilterBase + 5)) {
            ToolbarCommand(id);
            return 0;
        }
        break;
    }
    case WM_SIZE:
        LayoutToolbar();
        ClampScroll();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        if (!g_mw || !g_mw->panelW) break;
        SIZE s = WindowSizeFor(g_mw->dpi);
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize = {std::max<LONG>(s.cx * 2 / 3, g_mw->panelW + S(385)), s.cy};
        return 0;
    }
    case WM_DPICHANGED: {
        ApplyDpi(HIWORD(w));
        const RECT* r = (const RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_mw->tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            g_mw->tracking = true;
        }
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        if (g_mw->scrollDrag) {
            ScrollToThumbY(p.y);
            return 0;
        }
        if (g_mw->dragArmed) {
            if (abs(p.x - g_mw->downPt.x) > GetSystemMetrics(SM_CXDRAG) ||
                abs(p.y - g_mw->downPt.y) > GetSystemMetrics(SM_CYDRAG)) {
                g_mw->dragArmed = g_mw->collapseOnUp = false;
                ReleaseCapture();
                int t = g_mw->downTile;
                if (t >= 0 && t < (int)g_mw->files.size()) StartDrag(t);
            }
            return 0;
        }
        int i = HitItem(p);
        if (i >= 0 && !Enabled(kItems[i])) i = -1;
        if (i != g_mw->hover) {
            g_mw->hover = i;
            if (i != g_mw->pressed) g_mw->pressed = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        SetHoverTile(HitTile(p));
        const int hover = HitToolbar(p);
        if (g_mw->toolbarHover != hover) {
            g_mw->toolbarHover = hover;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_mw->tracking = false;
        if (g_mw->toolbarHover != -1) {
            g_mw->toolbarHover = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        if (g_mw->hover >= 0 || g_mw->pressed >= 0) {
            g_mw->hover = g_mw->pressed = -1;
            InvalidateRect(h, nullptr, FALSE);
        }
        SetHoverTile(-1);
        return 0;
    case WM_MOUSEWHEEL: {
        if (!GridMode()) return 0;
        Grid g = GetGrid();
        ScrollBy(-GET_WHEEL_DELTA_WPARAM(w) * (g.stepY / 2) / WHEEL_DELTA);
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        ScreenToClient(h, &p);
        SetHoverTile(HitTile(p));
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        const int toolbar = HitToolbar(p);
        if (toolbar >= 0) {
            SetFocus(h);
            if (toolbar == kNewest) OpenSortMenu();
            else ToolbarCommand(toolbar);
            return 0;
        }
        if (GridMode()) {
            const ToolbarLayout layout = GetToolbarLayout();
            if (PtInRect(&layout.search, p)) {
                SetFocus(g_mw->search);
                return 0;
            }
        }
        if (p.x >= g_mw->panelW) {
            if (GridMode()) {
                Grid g = GetGrid();
                RECT th = ScrollThumb(g);
                RECT a = ContentRect();
                if (th.bottom > th.top && p.x >= a.right - S(kScrollW) - S(6)) {  // scrollbar strip
                    if (!PtInRect(&th, p)) {
                        g_mw->scrollGrab = (th.bottom - th.top) / 2;  // jump so the thumb centres on the click
                    } else {
                        g_mw->scrollGrab = p.y - th.top;
                    }
                    g_mw->scrollDrag = true;
                    SetCapture(h);
                    ScrollToThumbY(p.y);
                    return 0;
                }
            }
            int t = HitTile(p);
            SetFocus(h);
            if (t >= 0) {
                bool ctrl = (w & MK_CONTROL) != 0, shift = (w & MK_SHIFT) != 0;
                g_mw->collapseOnUp = false;
                if (m == WM_LBUTTONDBLCLK && !ctrl && !shift) {
                    SelectTile(t);
                    OpenEntry(g_mw->files[t]);
                    return 0;
                }
                if (shift) RangeSelect(t, ctrl);
                else if (ctrl) ToggleTile(t);
                else if (g_mw->picked.size() > 1 && IsPicked(g_mw->files[t].path)) {
                    g_mw->sel = t;  // keep the selection so it can be dragged as a whole
                    g_mw->collapseOnUp = true;
                } else SelectTile(t);
                if (!IsPicked(g_mw->files[t].path)) return 0;
                g_mw->downTile = t;
                g_mw->downPt = p;
                g_mw->dragArmed = true;
                SetCapture(h);
            } else if (g_mw->sel >= 0 || !g_mw->picked.empty()) {
                g_mw->sel = g_mw->anchor = -1;
                g_mw->picked.clear();
                Redraw();
            }
            return 0;
        }
        int i = HitItem(p);
        if (i < 0 || !Enabled(kItems[i])) return 0;
        if (kItems[i].sub) {
            ShowSubmenu(i);  // ShareX opens dropdowns on press
        } else {
            g_mw->pressed = i;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_mw->scrollDrag || g_mw->dragArmed) {
            if (g_mw->dragArmed && g_mw->collapseOnUp) SelectTile(g_mw->downTile);
            g_mw->collapseOnUp = false;
            g_mw->scrollDrag = g_mw->dragArmed = false;
            ReleaseCapture();
            Redraw();
            return 0;
        }
        int i = HitItem({(short)LOWORD(l), (short)HIWORD(l)});
        int p = g_mw->pressed;
        g_mw->pressed = -1;
        InvalidateRect(h, nullptr, FALSE);
        if (i >= 0 && i == p) Activate(i);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_mw && (HWND)l != h) g_mw->scrollDrag = g_mw->dragArmed = false;
        return 0;
    case WM_RBUTTONDOWN: {
        int t = HitTile({(short)LOWORD(l), (short)HIWORD(l)});
        if (t >= 0 && IsPicked(g_mw->files[t].path)) g_mw->sel = t;  // right click keeps a multi-selection
        else if (t >= 0) SelectTile(t);
        return 0;
    }
    case WM_CONTEXTMENU: {
        if (!GridMode()) break;
        POINT sp{(short)LOWORD(l), (short)HIWORD(l)};
        int t;
        if (sp.x == -1 && sp.y == -1) {  // keyboard: menu at the selected tile
            t = g_mw->sel;
            if (t < 0) return 0;
            RECT r = TileRect(GetGrid(), t);
            sp = {r.left + S(20), r.top + S(20)};
            ClientToScreen(h, &sp);
        } else {
            POINT cp = sp;
            ScreenToClient(h, &cp);
            t = HitTile(cp);
        }
        if (t >= 0) TileMenu(t, sp);
        else {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kPaste, Tr(L"Вставить на полку\tCtrl+V", L"Paste to shelf\tCtrl+V"));
            UINT id = (UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, h, nullptr);
            DestroyMenu(menu);
            if (id && g_mw) ToolbarCommand(id);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && w == 'F') {
            g_mw->showTable = false;
            LayoutToolbar();
            SetFocus(g_mw->search);
            SendMessageW(g_mw->search, EM_SETSEL, 0, -1);
            Redraw();
            return 0;
        }
        if (ctrl && w == 'V') {
            PasteToShelf();
            return 0;
        }
        if (w == VK_F5) { Rescan(); return 0; }
        if (w == VK_ESCAPE && !g_mw->query.empty()) { SetWindowTextW(g_mw->search, L""); return 0; }
        if (!GridMode()) break;
        if (KeyNav(w)) return 0;
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SelectAll();
            return 0;
        }
        const Entry* e = Selected();
        if (!e) break;
        if (w == VK_RETURN) OpenEntry(*e);
        else if (w == VK_DELETE) DeleteEntry(g_mw->sel);
        else if (w == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (e->kind == K_Image) CopyImage(e->path);
            else CopyFileToClipboard(e->path);
        } else break;
        return 0;
    }
    case WM_TIMER:
        if (w == kTimerRun) {
            KillTimer(h, kTimerRun);
            int cmd = g_mw->pendingCmd;
            bool restore = g_mw->pendingRestore;
            g_mw->pendingCmd = 0;
            RunOnMain([cmd, restore] {
                RunCommand(cmd);
                if (restore && g_mw && !IsWindowVisible(g_mw->hwnd)) ShowWindow(g_mw->hwnd, SW_SHOWNA);
            });
        } else if (w == kTimerFlash) {
            if (--g_mw->flash <= 0) {
                g_mw->flash = 0;
                KillTimer(h, kTimerFlash);
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_mw->drop) {
            RevokeDragDrop(h);
            g_mw->drop->Release();
            g_mw->drop = nullptr;
        }
        return 0;
    case WM_NCDESTROY: {
        StopWorker();
        FreeAllThumbs();
        g_mw->files.clear();
        g_mw->files.shrink_to_fit();
        FreeIcons();
        if (g_mw->menuFont) DeleteObject(g_mw->menuFont);
        if (g_mw->tableFont) DeleteObject(g_mw->tableFont);
        if (g_mw->badgeFont) DeleteObject(g_mw->badgeFont);
        if (g_mw->searchBrush) DeleteObject(g_mw->searchBrush);
        delete g_mw;
        g_mw = nullptr;
        LRESULT r = DefWindowProcW(h, m, w, l);
        TrimMemory();
        return r;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool MainWindowMediaBusy() {
    return g_cursorRenderBusy.load() || g_pasteJobs.load() != 0 || (g_mw && g_mw->pasting);
}

void ShowMainWindow() {
    if (g_mw) {
        HWND h = g_mw->hwnd;
        if (IsIconic(h) || !IsWindowVisible(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        RefreshRows();
        Rescan();
        InvalidateRect(h, nullptr, FALSE);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = MainWndProc;
        wc.hInstance = g_inst;
        wc.hIcon = g_icon;
        wc.hIconSm = g_icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ZipshotMainWindow";
        registered = RegisterClassExW(&wc) != 0;
    }
    g_mw = new State;
    g_mw->session = ++g_sessionCounter;
    HWND h = CreateWindowExW(0, L"ZipshotMainWindow", L"Zipshot", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
                             nullptr, nullptr, g_inst, nullptr);
    if (!h) {
        delete g_mw;
        g_mw = nullptr;
        return;
    }
    g_mw->hwnd = h;
    if (g_icon) {
        SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)g_icon);
        SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)g_icon);
    }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE
    AllowDarkMenus(h);

    // Centre on the monitor under the cursor, sized for that monitor's DPI.
    POINT cur;
    GetCursorPos(&cur);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(h, nullptr, mi.rcWork.left, mi.rcWork.top, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    ApplyDpi(GetDpiForWindow(h));
    g_mw->oldestFirst = GetSettingBool(L"GalleryOldestFirst", false);
    CreateToolbar();
    SIZE s = WindowSizeFor(g_mw->dpi);
    RECT& wa = mi.rcWork;
    int x = wa.left + std::max(0L, (wa.right - wa.left - s.cx) / 2), y = wa.top + std::max(0L, (wa.bottom - wa.top - s.cy) / 2);
    SetWindowPos(h, nullptr, x, y, s.cx, s.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    RefreshRows();
    StartWorker();
    Rescan();
    g_mw->drop = new DropTarget;
    HRESULT hr = RegisterDragDrop(h, g_mw->drop);
    if (FAILED(hr)) {
        Log(L"mainwindow: RegisterDragDrop failed 0x%08X", (unsigned)hr);
        g_mw->drop->Release();
        g_mw->drop = nullptr;
    }
    ShowWindow(h, SW_SHOWNORMAL);
    SetForegroundWindow(h);
}

void MainWindowNotifyCapture(const std::wstring& path) {
    if (!g_mw) return;
    Entry e;
    size_t slash = path.find_last_of(L"\\/");
    e.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (!IsCaptureExt(e.name, e.kind)) return;
    e.path = path;
    e.shelf = shelf::IsShelfPath(path);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) e.time = FtToU64(fa.ftLastWriteTime);
    std::erase_if(g_mw->catalog, [&](const gallery::Item& item) { return item.path == path; });
    g_mw->catalog.push_back(static_cast<const gallery::Item&>(e));
    RebuildView();
}

IDropTarget* MainWindowDropTargetForTest() { return g_mw ? g_mw->drop : nullptr; }

void MainWindowRefresh() {
    if (!g_mw) return;
    CreateFonts();
    Measure();
    LayoutToolbar();
    RefreshRows();
    ClampScroll();
    Redraw();
}

bool MainWindowWriteReady(const std::wstring& path) {
    if (!g_mw || !IsWindowVisible(g_mw->hwnd) || !g_mw->search || !IsWindowVisible(g_mw->search)) return false;
    UpdateWindow(g_mw->hwnd);
    RECT r{};
    if (!g_mw->paintCount || !GetWindowRect(g_mw->hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return false;
    FILE* out = nullptr;
    if (_wfopen_s(&out, path.c_str(), L"wb") || !out) return false;
    const bool ok = fprintf(out, "{\"visible\":true,\"search_visible\":true,\"paint_count\":%u,\"width\":%ld,\"height\":%ld,\"dpi\":%u,\"items\":%zu,\"oldest_first\":%s}\n",
                            g_mw->paintCount, r.right-r.left, r.bottom-r.top, g_mw->dpi, g_mw->files.size(),
                            g_mw->oldestFirst ? "true" : "false") > 0;
    return fclose(out) == 0 && ok;
}
