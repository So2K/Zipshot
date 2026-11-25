#include "common.h"
#include <algorithm>

// Region selection over a frozen screenshot. Only the parts of the window that change
// (selection, its label, the magnifier) are re-blitted on mouse move.

namespace {

constexpr int kMagCells = 15;  // odd, so there is a centre pixel
constexpr int kMagCell = 8;
constexpr int kMagSize = kMagCells * kMagCell;

struct Overlay {
    HWND hwnd = nullptr;
    RECT vs{};
    int W = 0, H = 0;
    HDC dcOrig = nullptr, dcDim = nullptr, dcBack = nullptr;
    HBITMAP bmOrig = nullptr, bmDim = nullptr, bmBack = nullptr;
    HGDIOBJ oldOrig = nullptr, oldDim = nullptr, oldBack = nullptr;
    HFONT font = nullptr;
    std::vector<RECT> wins;  // client coords
    POINT cur{};
    bool dragging = false;
    POINT anchor{};
    RECT sel{};
    RECT hover{};
    bool hasHover = false;
    bool forRecording = false;
    bool done = false, ok = false;
    RECT result{};
    std::vector<RECT> lastDirty;

    bool Init(const Image& screen, RECT v) {
        vs = v;
        W = screen.w;
        H = screen.h;
        HDC sdc = GetDC(nullptr);
        void *bo, *bd, *bb;
        bmOrig = CreateDib(W, H, &bo);
        bmDim = CreateDib(W, H, &bd);
        bmBack = CreateDib(W, H, &bb);
        if (!bmOrig || !bmDim || !bmBack) { ReleaseDC(nullptr, sdc); return false; }
        memcpy(bo, screen.px.data(), screen.px.size() * 4);
        const uint32_t* s = screen.px.data();
        uint32_t* d = (uint32_t*)bd;
        for (size_t i = 0, n = screen.px.size(); i < n; ++i) {
            uint32_t p = s[i];
            // ~60% brightness: (p>>1) + (p>>3) per channel without cross-channel bleed.
            d[i] = ((p >> 1) & 0x7F7F7F) + ((p >> 3) & 0x1F1F1F) | 0xFF000000u;
        }
        dcOrig = CreateCompatibleDC(sdc);
        dcDim = CreateCompatibleDC(sdc);
        dcBack = CreateCompatibleDC(sdc);
        ReleaseDC(nullptr, sdc);
        oldOrig = SelectObject(dcOrig, bmOrig);
        oldDim = SelectObject(dcDim, bmDim);
        oldBack = SelectObject(dcBack, bmBack);
        font = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        for (RECT r : VisibleWindowRects()) {
            OffsetRect(&r, -vs.left, -vs.top);
            RECT c;
            RECT all{0, 0, W, H};
            if (IntersectRect(&c, &r, &all)) wins.push_back(c);
        }
        return true;
    }

    ~Overlay() {
        if (dcOrig) { SelectObject(dcOrig, oldOrig); DeleteDC(dcOrig); }
        if (dcDim) { SelectObject(dcDim, oldDim); DeleteDC(dcDim); }
        if (dcBack) { SelectObject(dcBack, oldBack); DeleteDC(dcBack); }
        if (bmOrig) DeleteObject(bmOrig);
        if (bmDim) DeleteObject(bmDim);
        if (bmBack) DeleteObject(bmBack);
        if (font) DeleteObject(font);
    }

    bool CurrentRect(RECT& r) const {
        if (dragging) { r = sel; return r.right > r.left && r.bottom > r.top; }
        if (hasHover) { r = hover; return true; }
        return false;
    }

    RECT LabelRect(const RECT& r) const {
        RECT l{r.left, r.top - 24, r.left + 110, r.top - 2};
        if (l.top < 0) OffsetRect(&l, 0, 26);
        if (l.right > W) OffsetRect(&l, W - l.right, 0);
        return l;
    }

    RECT MagRect() const {
        int x = cur.x + 24, y = cur.y + 24;
        int h = kMagSize + 22;
        if (x + kMagSize > W) x = cur.x - 24 - kMagSize;
        if (y + h > H) y = cur.y - 24 - h;
        return {x, y, x + kMagSize, y + h};
    }

    std::vector<RECT> DirtyRects() const {
        std::vector<RECT> v;
        RECT r;
        if (CurrentRect(r)) {
            InflateRect(&r, 3, 3);
            v.push_back(r);
            RECT l = LabelRect(r);
            InflateRect(&l, 1, 1);
            v.push_back(l);
        }
        RECT m = MagRect();
        InflateRect(&m, 2, 2);
        v.push_back(m);
        return v;
    }

    void Text(HDC dc, const wchar_t* s, RECT r) {
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 240));
        HGDIOBJ of = SelectObject(dc, font);
        DrawTextW(dc, s, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }

    void Frame(HDC dc, RECT r, COLORREF c, int t) {
        HBRUSH b = CreateSolidBrush(c);
        RECT e;
        e = {r.left, r.top, r.right, r.top + t}; FillRect(dc, &e, b);
        e = {r.left, r.bottom - t, r.right, r.bottom}; FillRect(dc, &e, b);
        e = {r.left, r.top, r.left + t, r.bottom}; FillRect(dc, &e, b);
        e = {r.right - t, r.top, r.right, r.bottom}; FillRect(dc, &e, b);
        DeleteObject(b);
    }

    // Composes the scene into dcBack, limited to clip.
    void Compose(RECT clip) {
        int sv = SaveDC(dcBack);
        IntersectClipRect(dcBack, clip.left, clip.top, clip.right, clip.bottom);
        BitBlt(dcBack, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, dcDim, clip.left, clip.top, SRCCOPY);
        RECT r;
        if (CurrentRect(r)) {
            RECT bright;
            if (IntersectRect(&bright, &r, &clip))
                BitBlt(dcBack, bright.left, bright.top, bright.right - bright.left, bright.bottom - bright.top, dcOrig,
                       bright.left, bright.top, SRCCOPY);
            RECT outer = r;
            InflateRect(&outer, 1, 1);
            if (dragging) {
                Frame(dcBack, outer, RGB(0, 0, 0), 1);
                Frame(dcBack, r, RGB(255, 255, 255), 1);
            } else {
                Frame(dcBack, r, forRecording ? RGB(235, 60, 60) : RGB(70, 160, 255), 2);
            }
            wchar_t buf[48];
            swprintf_s(buf, L"%d × %d", r.right - r.left, r.bottom - r.top);
            Text(dcBack, buf, LabelRect(r));
        }
        // Magnifier
        RECT m = MagRect();
        int half = kMagCells / 2;
        SetStretchBltMode(dcBack, COLORONCOLOR);
        StretchBlt(dcBack, m.left, m.top, kMagSize, kMagSize, dcOrig, cur.x - half, cur.y - half, kMagCells, kMagCells, SRCCOPY);
        RECT c{m.left + half * kMagCell, m.top + half * kMagCell, m.left + (half + 1) * kMagCell, m.top + (half + 1) * kMagCell};
        RECT co = c;
        InflateRect(&co, 1, 1);
        Frame(dcBack, co, RGB(0, 0, 0), 1);
        Frame(dcBack, c, RGB(255, 255, 255), 1);
        RECT mf{m.left - 1, m.top - 1, m.right + 1, m.top + kMagSize + 1};
        Frame(dcBack, mf, RGB(255, 255, 255), 1);
        wchar_t pos[48];
        COLORREF px = GetPixel(dcOrig, cur.x, cur.y);
        swprintf_s(pos, L"%d, %d  #%02X%02X%02X", cur.x + vs.left, cur.y + vs.top, GetRValue(px), GetGValue(px), GetBValue(px));
        Text(dcBack, pos, {m.left - 1, m.top + kMagSize + 1, m.right + 1, m.bottom});
        RestoreDC(dcBack, sv);
    }

    void Present(HDC dc, const std::vector<RECT>& rects) {
        for (RECT r : rects) {
            RECT all{0, 0, W, H};
            if (!IntersectRect(&r, &r, &all)) continue;
            Compose(r);
            BitBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, dcBack, r.left, r.top, SRCCOPY);
        }
    }

    void Update() {
        std::vector<RECT> now = DirtyRects();
        std::vector<RECT> all = lastDirty;
        all.insert(all.end(), now.begin(), now.end());
        // Merge the two selection rects (they share an anchor) to avoid painting twice.
        if (!lastDirty.empty() && lastDirty.size() == now.size() && now.size() == 3) {
            all.clear();
            RECT u;
            UnionRect(&u, &lastDirty[0], &now[0]);
            all.push_back(u);
            all.push_back(lastDirty[1]); all.push_back(now[1]);
            all.push_back(lastDirty[2]); all.push_back(now[2]);
        }
        lastDirty = now;
        HDC dc = GetDC(hwnd);
        Present(dc, all);
        ReleaseDC(hwnd, dc);
    }

    void UpdateHover() {
        hasHover = false;
        for (const RECT& r : wins)
            if (PtInRect(&r, cur)) { hover = r; hasHover = true; break; }
    }

    void Finish(bool accept, RECT r) {
        ok = accept && r.right - r.left >= 2 && r.bottom - r.top >= 2;
        result = r;
        OffsetRect(&result, vs.left, vs.top);
        done = true;
    }
};

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* o = (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!o && m != WM_NCCREATE) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (o) o->Present(dc, {ps.rcPaint});
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_MOUSEMOVE: {
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        if (p.x == o->cur.x && p.y == o->cur.y) return 0;
        o->cur = p;
        if (o->dragging) {
            o->sel = {std::min(o->anchor.x, p.x), std::min(o->anchor.y, p.y), std::max(o->anchor.x, p.x) + 1,
                      std::max(o->anchor.y, p.y) + 1};
        } else {
            o->UpdateHover();
        }
        o->Update();
        return 0;
    }
    case WM_LBUTTONDOWN:
        o->anchor = o->cur = {(short)LOWORD(l), (short)HIWORD(l)};
        o->dragging = true;
        o->sel = {o->anchor.x, o->anchor.y, o->anchor.x + 1, o->anchor.y + 1};
        SetCapture(h);
        return 0;
    case WM_LBUTTONUP: {
        if (!o->dragging) return 0;
        ReleaseCapture();
        o->dragging = false;
        bool click = std::abs(o->cur.x - o->anchor.x) < 4 && std::abs(o->cur.y - o->anchor.y) < 4;
        if (click && o->hasHover) o->Finish(true, o->hover);
        else o->Finish(!click, o->sel);
        return 0;
    }
    case WM_RBUTTONDOWN:
        if (o->dragging) {
            ReleaseCapture();
            o->dragging = false;
            o->UpdateHover();
            o->Update();
        } else {
            o->Finish(false, {});
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) o->Finish(false, {});
        else if (w == VK_RETURN && !o->dragging) {
            RECT r{0, 0, o->W, o->H};
            o->Finish(true, o->hasHover ? o->hover : r);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

const wchar_t* RegisterOverlayClass() {
    static const wchar_t* name = L"ZipshotOverlay";
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = name;
        RegisterClassW(&wc);
        reg = true;
    }
    return name;
}

}  // namespace

bool SelectRegion(const Image& screen, RECT vs, RECT& out, bool forRecording) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    Overlay o;
    o.forRecording = forRecording;
    if (!o.Init(screen, vs)) return false;
    GetCursorPos(&o.cur);
    o.cur.x -= vs.left;
    o.cur.y -= vs.top;
    o.UpdateHover();
    o.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, RegisterOverlayClass(), L"Zipshot", WS_POPUP, vs.left, vs.top,
                             o.W, o.H, nullptr, nullptr, g_inst, &o);
    if (!o.hwnd) return false;
    o.lastDirty = o.DirtyRects();
    ShowWindow(o.hwnd, SW_SHOW);
    SetForegroundWindow(o.hwnd);
    SetFocus(o.hwnd);
    UpdateWindow(o.hwnd);
    QueryPerformanceCounter(&t1);
    Log(L"overlay: shown in %.1f ms", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    MSG msg;
    while (!o.done) {
        int r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(o.hwnd);
    out = o.result;
    return o.ok;
}

bool DumpOverlayTest(const Image& screen, RECT vs, POINT cursor, RECT sel, const std::wstring& outPng) {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    Overlay o;
    if (!o.Init(screen, vs)) return false;
    QueryPerformanceCounter(&t1);
    Log(L"overlay test: init %.1f ms", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    o.cur = cursor;
    if (sel.right > sel.left) {
        o.dragging = true;
        o.sel = sel;
    } else {
        o.UpdateHover();
    }
    o.Compose({0, 0, o.W, o.H});
    GdiFlush();
    Image img;
    img.w = o.W;
    img.h = o.H;
    img.px.resize((size_t)o.W * o.H);
    BITMAP bm;
    GetObject(o.bmBack, sizeof(bm), &bm);
    memcpy(img.px.data(), bm.bmBits, img.px.size() * 4);
    return EncodeImage(img, outPng, false);
}
