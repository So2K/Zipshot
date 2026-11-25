#include "common.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>

// Small "captured" popup in the bottom-right corner.
// Left click: editor (images) / open file (video), middle: show in folder, right: close (and copy path as text for videos).
// Dragging it drops the saved file into any app (Telegram, Discord, Explorer, a browser...).

using Microsoft::WRL::ComPtr;

namespace {

struct Toast {
    HWND hwnd = nullptr;
    ImagePtr img;
    std::wstring path;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;
    int alpha = 255;
    bool hovering = false;
    bool armed = false, dragging = false;
    int fadeStep = 11;  // alpha per 30 ms tick
    POINT down{};
    ~Toast() {
        if (dc) { SelectObject(dc, old); DeleteDC(dc); }
        if (bmp) DeleteObject(bmp);
    }
};

Toast* g_toast = nullptr;
constexpr UINT_PTR kTimerHide = 1, kTimerFade = 2;

void CloseToast() {
    if (g_toast && !g_toast->dragging) DestroyWindow(g_toast->hwnd);
}

// Shell drag of the file: the shell supplies CF_HDROP, file contents and the thumbnail drag image.
bool DragFile(HWND h, const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) return false;
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
    CoTaskMemFree(pidl);
    ComPtr<IDataObject> data;
    if (FAILED(hr) || FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) return false;
    DWORD effect = 0;
    hr = SHDoDragDrop(h, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_LINK, &effect);
    if (FAILED(hr)) Log(L"toast: drag failed 0x%08X", hr);
    return hr == DRAGDROP_S_DROP;
}

LRESULT CALLBACK ToastProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* t = (Toast*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!t && m != WM_NCCREATE) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        BitBlt(dc, 0, 0, t->w, t->h, t->dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (t->armed && !t->dragging && !t->path.empty()) {
            POINT p{(short)LOWORD(l), (short)HIWORD(l)};
            if (std::abs(p.x - t->down.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(p.y - t->down.y) > GetSystemMetrics(SM_CYDRAG)) {
                ReleaseCapture();
                t->armed = false;
                t->dragging = true;
                KillTimer(h, kTimerHide);
                KillTimer(h, kTimerFade);
                ShowWindow(h, SW_HIDE);  // out of the way of the drop target; the shell shows the thumbnail
                bool dropped = DragFile(h, t->path);
                t->dragging = false;
                if (dropped) {
                    DestroyWindow(h);
                } else {
                    ShowWindow(h, SW_SHOWNOACTIVATE);
                    SetTimer(h, kTimerHide, 1500, nullptr);
                }
                return 0;
            }
        }
        if (!t->hovering) {
            t->hovering = true;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            KillTimer(h, kTimerHide);
            KillTimer(h, kTimerFade);
            t->alpha = 255;
            SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (t->dragging) return 0;
        t->hovering = false;
        SetTimer(h, kTimerHide, 1500, nullptr);
        return 0;
    case WM_TIMER:
        if (t->dragging) return 0;
        if (w == kTimerHide) {
            KillTimer(h, kTimerHide);
            SetTimer(h, kTimerFade, 30, nullptr);
        } else if (w == kTimerFade) {
            t->alpha -= t->fadeStep;
            if (t->alpha <= 0) DestroyWindow(h);
            else SetLayeredWindowAttributes(h, 0, (BYTE)t->alpha, LWA_ALPHA);
        }
        return 0;
    case WM_LBUTTONDOWN:
        t->armed = true;
        t->down = {(short)LOWORD(l), (short)HIWORD(l)};
        SetCapture(h);
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        if (!t->armed) return 0;
        t->armed = false;
        ImagePtr img = t->img;
        std::wstring path = t->path;
        DestroyWindow(h);
        if (img) OpenEditor(img, path);
        else if (!path.empty()) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    case WM_MBUTTONUP: {
        std::wstring path = t->path;
        DestroyWindow(h);
        if (!path.empty()) {
            std::wstring args = L"/select,\"" + path + L"\"";
            ShellExecuteW(nullptr, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        bool isVideo = !t->img;
        std::wstring path = t->path;
        DestroyWindow(h);
        if (isVideo && !path.empty()) {
            CopyTextToClipboard(path);
            PlayUiSound(S_Action);
        }
        return 0;
    }
    case WM_NCDESTROY:
        if (g_toast == t) g_toast = nullptr;
        delete t;
        TrimMemory();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void ShowToast(ImagePtr img, const std::wstring& path, const std::wstring& title) {
    CloseToast();
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ToastProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.lpszClassName = L"ZipshotToast";
        RegisterClassW(&wc);
        reg = true;
    }
    POINT cp;
    GetCursorPos(&cp);
    HMONITOR mon = MonitorFromPoint(cp, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    float ui = MonitorDpi(mon) / 96.0f;

    auto* t = new Toast;
    t->img = img;
    t->path = path;
    int cap = (int)(26 * ui);
    int tw = 0, th = 0;
    if (img && !img->empty()) {
        float s = std::min({1.0f, 400 * ui / img->w, 300 * ui / img->h});
        tw = std::max(1, (int)(img->w * s));
        th = std::max(1, (int)(img->h * s));
    }
    t->w = std::max(tw, (int)(300 * ui));
    t->h = th + cap;
    HDC sdc = GetDC(nullptr);
    void* bits = nullptr;
    t->bmp = CreateDib(t->w, t->h, &bits);
    if (!t->bmp || !bits) {
        ReleaseDC(nullptr, sdc);
        delete t;
        return;
    }
    t->dc = CreateCompatibleDC(sdc);
    ReleaseDC(nullptr, sdc);
    t->old = SelectObject(t->dc, t->bmp);
    RECT all{0, 0, t->w, t->h};
    HBRUSH bg = CreateSolidBrush(RGB(30, 30, 34));
    FillRect(t->dc, &all, bg);
    DeleteObject(bg);
    if (tw) {
        void* ib = nullptr;
        HBITMAP ibmp = CreateDib(img->w, img->h, &ib);
        if (ibmp && ib) {
            memcpy(ib, img->px.data(), img->px.size() * 4);
            HDC idc = CreateCompatibleDC(t->dc);
            HGDIOBJ io = SelectObject(idc, ibmp);
            SetStretchBltMode(t->dc, HALFTONE);
            SetBrushOrgEx(t->dc, 0, 0, nullptr);
            StretchBlt(t->dc, (t->w - tw) / 2, 0, tw, th, idc, 0, 0, img->w, img->h, SRCCOPY);
            SelectObject(idc, io);
            DeleteDC(idc);
        }
        if (ibmp) DeleteObject(ibmp);
    }
    HFONT f = CreateFontW((int)(-13 * ui), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(t->dc, f);
    SetBkMode(t->dc, TRANSPARENT);
    SetTextColor(t->dc, RGB(230, 230, 230));
    RECT tr{(int)(8 * ui), th, t->w - (int)(8 * ui), t->h};
    std::wstring text = path.empty() ? title : (title + L"  •  " + PathFindFileNameW(path.c_str()));
    DrawTextW(t->dc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(t->dc, of);
    DeleteObject(f);
    HBRUSH border = CreateSolidBrush(RGB(70, 70, 80));
    FrameRect(t->dc, &all, border);
    DeleteObject(border);

    int margin = (int)(12 * ui);
    int x = mi.rcWork.right - t->w - margin, y = mi.rcWork.bottom - t->h - margin;
    t->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, L"ZipshotToast", L"",
                              WS_POPUP, x, y, t->w, t->h, nullptr, nullptr, g_inst, t);
    if (!t->hwnd) { delete t; return; }
    // Keep the popup out of our own recordings and screenshots of the region.
    SetWindowDisplayAffinity(t->hwnd, WDA_EXCLUDEFROMCAPTURE);
    SetLayeredWindowAttributes(t->hwnd, 0, 255, LWA_ALPHA);
    g_toast = t;
    ShowWindow(t->hwnd, SW_SHOWNOACTIVATE);
    // ShareX ToastWindowDuration / ToastWindowFadeDuration (imported into zipshot.ini).
    auto secs = [](const wchar_t* key, double def) {
        wchar_t buf[32];
        GetPrivateProfileStringW(L"main", key, L"", buf, 32, (ExeDir() + L"\\zipshot.ini").c_str());
        return *buf ? _wtof(buf) : def;
    };
    double show = secs(L"ToastSeconds", 3), fade = secs(L"ToastFadeSeconds", 1);
    t->fadeStep = std::max(1, (int)(255 * 0.030 / std::max(0.05, fade)));
    SetTimer(t->hwnd, kTimerHide, (UINT)std::max(300.0, show * 1000), nullptr);
}
