#include "common.h"
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

Image Image::crop(RECT r) const {
    Image o;
    r.left = std::max<LONG>(r.left, 0);
    r.top = std::max<LONG>(r.top, 0);
    r.right = std::min<LONG>(r.right, w);
    r.bottom = std::min<LONG>(r.bottom, h);
    if (r.right <= r.left || r.bottom <= r.top) return o;
    o.w = r.right - r.left;
    o.h = r.bottom - r.top;
    o.alpha = alpha;
    o.px.resize((size_t)o.w * o.h);
    for (int y = 0; y < o.h; ++y)
        memcpy(&o.px[(size_t)y * o.w], &px[(size_t)(y + r.top) * w + r.left], (size_t)o.w * 4);
    return o;
}

RECT VirtualScreen() {
    RECT r;
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

HBITMAP CreateDib(int w, int h, void** bits) {
    if (bits) *bits = nullptr;
    if (w <= 0 || h <= 0) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

static void DrawCursor(HDC dc, POINT origin) {
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    DrawIconEx(dc, ci.ptScreenPos.x - (int)ii.xHotspot - origin.x, ci.ptScreenPos.y - (int)ii.yHotspot - origin.y,
               ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
}

// ---------- Desktop Duplication: the GPU hands over the composed desktop, ~10x faster than BitBlt ----------

namespace {

struct DdaAdapter {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    std::vector<ComPtr<IDXGIOutput1>> outputs;
};
std::vector<DdaAdapter> g_dda;  // created on first capture, kept (a few hundred KB)

void InitDda() {
    if (!g_dda.empty()) return;
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return;
    ComPtr<IDXGIAdapter1> a;
    for (UINT ai = 0; SUCCEEDED(f->EnumAdapters1(ai, &a)); ++ai, a.Reset()) {
        DdaAdapter da;
        ComPtr<IDXGIOutput> o;
        for (UINT oi = 0; SUCCEEDED(a->EnumOutputs(oi, &o)); ++oi, o.Reset()) {
            ComPtr<IDXGIOutput1> o1;
            DXGI_OUTPUT_DESC d;
            if (FAILED(o->GetDesc(&d))) continue;
            if (d.AttachedToDesktop && d.Rotation <= DXGI_MODE_ROTATION_IDENTITY && SUCCEEDED(o.As(&o1)))
                da.outputs.push_back(o1);
        }
        if (da.outputs.empty()) continue;
        if (FAILED(D3D11CreateDevice(a.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &da.dev, nullptr, &da.ctx)))
            continue;
        g_dda.push_back(std::move(da));
    }
}

// GDI copy of one part of the desktop into img (slow path, ~5x slower than duplication).
void GdiGrabInto(Image& img, RECT r, RECT part) {
    int pw = part.right - part.left, ph = part.bottom - part.top;
    void* bits = nullptr;
    HBITMAP bmp = CreateDib(pw, ph, &bits);
    if (!bmp) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, pw, ph, screen, part.left, part.top, SRCCOPY);
    GdiFlush();
    for (int y = 0; y < ph; ++y) {
        const uint32_t* s = (const uint32_t*)bits + (size_t)y * pw;
        uint32_t* t = &img.px[(size_t)(y + part.top - r.top) * img.w + (part.left - r.left)];
        for (int x = 0; x < pw; ++x) t[x] = s[x] | 0xFF000000u;
    }
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DeleteObject(bmp);
}

// True when a sparse sample of the part is pure black: what a not-yet-filled duplication frame looks like.
bool LooksBlack(const Image& img, RECT r, RECT part) {
    int pw = part.right - part.left, ph = part.bottom - part.top;
    for (int j = 0; j < 24; ++j)
        for (int i = 0; i < 24; ++i) {
            int x = part.left - r.left + (int)((i + 0.5) * pw / 24), y = part.top - r.top + (int)((j + 0.5) * ph / 24);
            if (img.px[(size_t)y * img.w + x] & 0xFFFFFF) return false;
        }
    return true;
}

enum class Grab { Ok, Gdi, DeviceLost };

// Copies the part of one output that intersects r into img.
Grab GrabOutput(DdaAdapter& da, IDXGIOutput1* out, RECT r, Image& img) {
    DXGI_OUTPUT_DESC d;
    if (FAILED(out->GetDesc(&d))) return Grab::DeviceLost;
    RECT part;
    if (!IntersectRect(&part, &r, &d.DesktopCoordinates)) return Grab::Ok;
    ComPtr<IDXGIOutputDuplication> dup;
    HRESULT hr = out->DuplicateOutput(da.dev.Get(), &dup);
    if (hr == E_ACCESSDENIED || hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE || hr == DXGI_ERROR_SESSION_DISCONNECTED)
        return Grab::Gdi;  // secure desktop, too many duplications, RDP...
    if (FAILED(hr)) return Grab::DeviceLost;
    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    // The first frame after DuplicateOutput normally carries the desktop at once. Sometimes it arrives
    // without an image yet (LastPresentTime == 0), which reads back as black, so give it two more tries.
    for (int attempt = 0; attempt < 3; ++attempt) {
        hr = E_FAIL;
        for (int i = 0; i < 3 && (hr = dup->AcquireNextFrame(attempt ? 40 : 100, &fi, &res)) == DXGI_ERROR_WAIT_TIMEOUT; ++i) {}
        if (FAILED(hr) || fi.LastPresentTime.QuadPart != 0 || attempt == 2) break;
        res.Reset();
        dup->ReleaseFrame();
    }
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_DEVICE_REMOVED) return Grab::DeviceLost;
    if (FAILED(hr)) return Grab::Gdi;
    ComPtr<ID3D11Texture2D> tex;
    if (!res || FAILED(res.As(&tex))) {
        dup->ReleaseFrame();
        return Grab::Gdi;
    }
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    int pw = part.right - part.left, ph = part.bottom - part.top;
    D3D11_TEXTURE2D_DESC sd{};
    sd.Width = pw;
    sd.Height = ph;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = td.Format;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    bool ok = false;
    if (td.Format == DXGI_FORMAT_B8G8R8A8_UNORM && SUCCEEDED(da.dev->CreateTexture2D(&sd, nullptr, &staging))) {
        D3D11_BOX box{(UINT)(part.left - d.DesktopCoordinates.left), (UINT)(part.top - d.DesktopCoordinates.top), 0,
                      (UINT)(part.right - d.DesktopCoordinates.left), (UINT)(part.bottom - d.DesktopCoordinates.top), 1};
        da.ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(da.ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < ph; ++y) {
                const uint32_t* s = (const uint32_t*)((const BYTE*)m.pData + (size_t)y * m.RowPitch);
                uint32_t* t = &img.px[(size_t)(y + part.top - r.top) * img.w + (part.left - r.left)];
                for (int x = 0; x < pw; ++x) t[x] = s[x] | 0xFF000000u;
            }
            da.ctx->Unmap(staging.Get(), 0);
            ok = true;
        }
    }
    tex.Reset();
    res.Reset();
    dup->ReleaseFrame();
    if (!ok || LooksBlack(img, r, part)) {
        Log(L"capture: duplication frame empty for output at %d,%d, using GDI", d.DesktopCoordinates.left, d.DesktopCoordinates.top);
        return Grab::Gdi;
    }
    return Grab::Ok;
}

bool CaptureDda(RECT r, Image& img) {
    InitDda();
    if (g_dda.empty()) return false;
    img.w = r.right - r.left;
    img.h = r.bottom - r.top;
    img.px.assign((size_t)img.w * img.h, 0xFF000000u);
    std::vector<RECT> covered;
    for (auto& da : g_dda)
        for (auto& o : da.outputs) {
            DXGI_OUTPUT_DESC od;
            if (FAILED(o->GetDesc(&od))) {
                g_dda.clear();
                return false;
            }
            covered.push_back(od.DesktopCoordinates);
            Grab res = GrabOutput(da, o.Get(), r, img);
            if (res == Grab::DeviceLost) {
                g_dda.clear();  // topology/device changed: rebuild next time, use GDI now
                return false;
            }
            if (res == Grab::Gdi) {
                DXGI_OUTPUT_DESC d;
                o->GetDesc(&d);
                RECT part;
                if (IntersectRect(&part, &r, &d.DesktopCoordinates)) GdiGrabInto(img, r, part);
            }
        }
    // Monitors no duplication output covers (driver without DDA support): GDI.
    struct Ctx { std::vector<RECT>* covered; Image* img; RECT r; } ctx{&covered, &img, r};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT mr, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        for (const RECT& cv : *c->covered)
            if (EqualRect(&cv, mr)) return TRUE;
        RECT part;
        if (IntersectRect(&part, mr, &c->r)) GdiGrabInto(*c->img, c->r, part);
        return TRUE;
    }, (LPARAM)&ctx);
    return true;
}

// Draws the cursor onto an already captured image via a small GDI patch.
void OverlayCursor(Image& img, RECT r) {
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;
    const int S = 128;
    RECT patch{ci.ptScreenPos.x - S / 2 - r.left, ci.ptScreenPos.y - S / 2 - r.top, 0, 0};
    patch.right = patch.left + S;
    patch.bottom = patch.top + S;
    void* bits = nullptr;
    HBITMAP bmp = CreateDib(S, S, &bits);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return; }
    auto* p = (uint32_t*)bits;
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            int ix = patch.left + x, iy = patch.top + y;
            p[y * S + x] = (ix >= 0 && iy >= 0 && ix < img.w && iy < img.h) ? img.px[(size_t)iy * img.w + ix] : 0;
        }
    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, bmp);
    DrawCursor(mem, {ci.ptScreenPos.x - S / 2, ci.ptScreenPos.y - S / 2});
    GdiFlush();
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            int ix = patch.left + x, iy = patch.top + y;
            if (ix >= 0 && iy >= 0 && ix < img.w && iy < img.h) img.px[(size_t)iy * img.w + ix] = p[y * S + x] | 0xFF000000u;
        }
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
}

}  // namespace

Image CaptureRect(RECT r, bool cursor) {
    Image img;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return img;
    if (CaptureDda(r, img)) {
        if (cursor) OverlayCursor(img, r);
        return img;
    }
    img = {};
    Log(L"capture: DDA unavailable, using GDI");
    void* bits = nullptr;
    HBITMAP bmp = CreateDib(w, h, &bits);
    if (!bmp) return img;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);
    if (cursor) DrawCursor(mem, {r.left, r.top});
    GdiFlush();
    img.w = w;
    img.h = h;
    img.px.resize((size_t)w * h);
    const uint32_t* src = (const uint32_t*)bits;
    uint32_t* dst = img.px.data();
    for (size_t i = 0, n = img.px.size(); i < n; ++i) dst[i] = src[i] | 0xFF000000u;
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DeleteObject(bmp);
    return img;
}

static bool IsIgnoredWindowClass(HWND hwnd) {
    wchar_t cls[64]{};
    if (!GetClassNameW(hwnd, cls, (int)std::size(cls))) return false;
    static const wchar_t* const kSkip[] = {
        L"Progman", L"WorkerW", L"SysShadow", L"tooltips_class32", L"MSCTFIME UI", L"IME",
        L"EdgeUiInputTopWndClass", L"EdgeUiInputWndClass", L"DummyDWMListenerWindow",
        L"NarratorHelperWindow", L"Shell_InputSwitchTopLevelWindow", L"TaskListThumbnailWnd",
        L"ForegroundStaging",
    };
    for (const wchar_t* s : kSkip)
        if (_wcsicmp(cls, s) == 0) return true;
    return false;
}

static BOOL CALLBACK EnumWin(HWND hwnd, LPARAM lp) {
    auto* out = (std::vector<WindowRect>*)lp;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return TRUE;
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0) return TRUE;
    }
    if (IsIgnoredWindowClass(hwnd)) return TRUE;
    RECT rgnBox{};
    if (GetWindowRgnBox(hwnd, &rgnBox) == NULLREGION) return TRUE;
    // Skip untitled non-activating or frameless layered tool windows (invisible/ghost helper squares).
    if ((ex & WS_EX_TOOLWINDOW) && GetWindowTextLengthW(hwnd) == 0) {
        if ((ex & WS_EX_NOACTIVATE) || ((ex & WS_EX_LAYERED) && !(style & (WS_CAPTION | WS_THICKFRAME)))) return TRUE;
    }
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
    RECT vs = VirtualScreen(), clipped{};
    if (!IntersectRect(&clipped, &r, &vs)) return TRUE;
    if (clipped.right - clipped.left < 24 || clipped.bottom - clipped.top < 24) return TRUE;
    out->push_back({hwnd, r});
    return TRUE;
}

std::vector<WindowRect> VisibleWindows() {
    std::vector<WindowRect> v;
    EnumWindows(EnumWin, (LPARAM)&v);
    return v;
}

std::vector<RECT> VisibleWindowRects() {
    std::vector<RECT> v;
    for (auto& w : VisibleWindows()) v.push_back(w.rect);
    return v;
}

// Deepest visible child control of `top` under `pt` (ShareX "detect controls"), in screen coords.
bool ControlRectAt(HWND top, POINT pt, RECT& out) {
    HWND cur = top, found = nullptr;
    for (int depth = 0; depth < 16; ++depth) {
        POINT cp = pt;
        ScreenToClient(cur, &cp);
        HWND child = ChildWindowFromPointEx(cur, cp, CWP_SKIPINVISIBLE | CWP_SKIPTRANSPARENT | CWP_SKIPDISABLED);
        if (!child || child == cur) break;
        if (GetWindowLongW(child, GWL_EXSTYLE) & WS_EX_TRANSPARENT) break;
        found = cur = child;
    }
    if (!found) return false;
    GetWindowRect(found, &out);
    return out.right - out.left >= 32 && out.bottom - out.top >= 32;
}
