#include "common.h"
#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

// Popup colour picker in the spirit of Houdini's / Figma's: saturation-value square, hue and alpha
// strips, old/new preview, HEX and alpha fields, screen eyedropper, presets and recent colours.
// Click outside or Enter commits, Esc cancels.

using Microsoft::WRL::ComPtr;

namespace {

ID2D1Factory* Factory() {
    static ComPtr<ID2D1Factory> f;
    if (!f) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf());
    return f.Get();
}
IDWriteFactory* DWFactory() {
    static ComPtr<IDWriteFactory> f;
    if (!f) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)f.GetAddressOf());
    return f.Get();
}

D2D1_COLOR_F C(int r, int g, int b, float a = 1) { return {r / 255.f, g / 255.f, b / 255.f, a}; }
const D2D1_COLOR_F kPanel = C(30, 31, 34), kField = C(43, 44, 48), kFieldHover = C(52, 53, 58), kEdge = C(64, 65, 72),
                   kText = C(231, 233, 234), kDim = C(150, 152, 158), kAccent = C(0, 138, 255);

const uint32_t kPresets[20] = {0xFFF23C3C, 0xFFFF7A1A, 0xFFFFC531, 0xFF00FF5D, 0xFF2DD4BF, 0xFF4DA3FF, 0xFF3B5BFF,
                               0xFFA78BFA, 0xFFFF5AC8, 0xFFFFFFFF, 0xFF000000, 0xFF1E1E1E, 0xFF3A3A3A, 0xFF6B6B6B,
                               0xFF9E9E9E, 0xFFD4D4D4, 0xFF8B4513, 0xFF7FD1FF, 0xFFFFE6A7, 0x00000000};

struct Hsv { float h, s, v; };

Hsv ToHsv(float r, float g, float b) {
    float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn, h = 0;
    if (d > 0) {
        if (mx == r) h = 60 * std::fmod((g - b) / d, 6.f);
        else if (mx == g) h = 60 * ((b - r) / d + 2);
        else h = 60 * ((r - g) / d + 4);
    }
    if (h < 0) h += 360;
    return {h, mx > 0 ? d / mx : 0, mx};
}

void ToRgb(Hsv c, float& r, float& g, float& b) {
    float k = c.v * c.s, hh = c.h / 60, x = k * (1 - std::fabs(std::fmod(hh, 2.f) - 1)), m = c.v - k;
    float rr = 0, gg = 0, bb = 0;
    if (hh < 1) rr = k, gg = x;
    else if (hh < 2) rr = x, gg = k;
    else if (hh < 3) gg = k, bb = x;
    else if (hh < 4) gg = x, bb = k;
    else if (hh < 5) rr = x, bb = k;
    else rr = k, bb = x;
    r = rr + m, g = gg + m, b = bb + m;
}

uint32_t Pack(const float c[4]) {
    auto q = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.f, 1.f) * 255); };
    return q(c[3]) << 24 | q(c[0]) << 16 | q(c[1]) << 8 | q(c[2]);
}
void Unpack(uint32_t v, float c[4]) {
    c[0] = (v >> 16 & 255) / 255.f, c[1] = (v >> 8 & 255) / 255.f, c[2] = (v & 255) / 255.f, c[3] = (v >> 24) / 255.f;
}

std::wstring Ini() { return ExeDir() + L"\\zipshot.ini"; }

std::vector<uint32_t> LoadRecent() {
    wchar_t buf[256];
    GetPrivateProfileStringW(L"colors", L"Recent", L"", buf, 256, Ini().c_str());
    std::vector<uint32_t> v;
    for (wchar_t* p = buf; *p;) {
        wchar_t* end;
        uint32_t c = wcstoul(p, &end, 16);
        if (end == p) break;
        v.push_back(c);
        p = *end == L',' ? end + 1 : end;
    }
    return v;
}

void SaveRecent(uint32_t c) {
    auto v = LoadRecent();
    v.erase(std::remove(v.begin(), v.end(), c), v.end());
    v.insert(v.begin(), c);
    if (v.size() > 10) v.resize(10);
    std::wstring s;
    for (uint32_t x : v) {
        wchar_t b[12];
        swprintf_s(b, L"%08X,", x);
        s += b;
    }
    WritePrivateProfileStringW(L"colors", L"Recent", s.c_str(), Ini().c_str());
}

enum Drag { D_None, D_SV, D_Hue, D_Alpha };

struct Picker {
    HWND hwnd = nullptr;
    float ui = 1;
    float orig[4]{}, cur[4]{};
    Hsv hsv{};
    bool done = false, ok = false;
    Drag drag = D_None;
    bool editingHex = false, editingAlpha = false, eyedropper = false;
    std::wstring editText;
    Image screen;  // for the eyedropper
    RECT vs{};
    std::vector<uint32_t> recent;
    POINT mouse{-1, -1};
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    ComPtr<IDWriteTextFormat> font, smallFont;

    // Layout, in pixels.
    float W() const { return std::round(300 * ui); }
    float P() const { return std::round(12 * ui); }
    D2D1_RECT_F SvRect() const { return {P(), P(), W() - P(), P() + std::round(176 * ui)}; }
    D2D1_RECT_F HueRect() const { float y = SvRect().bottom + std::round(12 * ui); return {P(), y, W() - P(), y + std::round(12 * ui)}; }
    D2D1_RECT_F AlphaRect() const { float y = HueRect().bottom + std::round(10 * ui); return {P(), y, W() - P(), y + std::round(12 * ui)}; }
    float RowY() const { return AlphaRect().bottom + std::round(12 * ui); }
    float RowH() const { return std::round(30 * ui); }
    D2D1_RECT_F PreviewRect() const { return {P(), RowY(), P() + std::round(44 * ui), RowY() + RowH()}; }
    D2D1_RECT_F EyeRect() const { return {W() - P() - RowH(), RowY(), W() - P(), RowY() + RowH()}; }
    D2D1_RECT_F AlphaBox() const { float r = EyeRect().left - std::round(6 * ui); return {r - std::round(56 * ui), RowY(), r, RowY() + RowH()}; }
    D2D1_RECT_F HexBox() const { return {PreviewRect().right + std::round(6 * ui), RowY(), AlphaBox().left - std::round(6 * ui), RowY() + RowH()}; }
    float Cell() const { return (W() - 2 * P() - 9 * std::round(6 * ui)) / 10; }
    D2D1_RECT_F Swatch(int i) const {  // 0..19 presets, 20..29 recent
        float gap = std::round(6 * ui), c = Cell();
        float y0 = RowY() + RowH() + std::round(14 * ui);
        int row = i / 10, col = i % 10;
        float y = y0 + row * (c + gap) + (row >= 2 ? std::round(20 * ui) : 0);
        float x = P() + col * (c + gap);
        return {x, y, x + c, y + c};
    }
    float H() const { return Swatch(20).bottom + P(); }

    void SetFromRgb() { hsv = ToHsv(cur[0], cur[1], cur[2]); }
    void SetFromHsv() { ToRgb(hsv, cur[0], cur[1], cur[2]); }

    void Brush(D2D1_COLOR_F c) { br->SetColor(c); }

    void Checker(D2D1_RECT_F r, float cell) {
        rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
        Brush(C(200, 200, 200));
        rt->FillRectangle(r, br.Get());
        Brush(C(255, 255, 255));
        for (float y = r.top; y < r.bottom; y += cell)
            for (float x = r.left + (((int)((y - r.top) / cell)) % 2) * cell; x < r.right; x += 2 * cell)
                rt->FillRectangle({x, y, x + cell, y + cell}, br.Get());
        rt->PopAxisAlignedClip();
    }

    ComPtr<ID2D1LinearGradientBrush> Gradient(D2D1_POINT_2F a, D2D1_POINT_2F b, const D2D1_GRADIENT_STOP* stops, UINT n) {
        ComPtr<ID2D1GradientStopCollection> sc;
        rt->CreateGradientStopCollection(stops, n, &sc);
        ComPtr<ID2D1LinearGradientBrush> lg;
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), sc.Get(), &lg);
        return lg;
    }

    void Knob(D2D1_POINT_2F p, float r, D2D1_COLOR_F fill) {
        Brush(C(0, 0, 0, .35f));
        rt->FillEllipse(D2D1::Ellipse({p.x, p.y + ui}, r + 1.5f * ui, r + 1.5f * ui), br.Get());
        Brush(C(255, 255, 255));
        rt->FillEllipse(D2D1::Ellipse(p, r + 1.5f * ui, r + 1.5f * ui), br.Get());
        Brush(fill);
        rt->FillEllipse(D2D1::Ellipse(p, r - ui, r - ui), br.Get());
    }

    void Field(D2D1_RECT_F r, const std::wstring& text, bool editing, bool hover) {
        Brush(hover || editing ? kFieldHover : kField);
        rt->FillRoundedRectangle({r, 6 * ui, 6 * ui}, br.Get());
        if (editing) {
            Brush(kAccent);
            rt->DrawRoundedRectangle({{r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f}, 6 * ui, 6 * ui}, br.Get(), 1.5f * ui);
        }
        Brush(kText);
        std::wstring t = text + (editing ? L"|" : L"");
        rt->DrawTextW(t.c_str(), (UINT32)t.size(), font.Get(), {r.left + 8 * ui, r.top, r.right - 6 * ui, r.bottom}, br.Get());
    }

    bool Hover(D2D1_RECT_F r) const { return mouse.x >= r.left && mouse.x < r.right && mouse.y >= r.top && mouse.y < r.bottom; }

    void Render() {
        if (!rt) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties();
            p.dpiX = p.dpiY = 96;
            Factory()->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(cr.right, cr.bottom)), &rt);
            if (!rt) return;
            rt->CreateSolidColorBrush(kText, &br);
        }
        rt->BeginDraw();
        rt->Clear(kPanel);
        D2D1_COLOR_F solid{cur[0], cur[1], cur[2], 1};
        // Saturation/value square: hue, white to the left, black to the bottom.
        D2D1_RECT_F sv = SvRect();
        float hr, hg, hb;
        ToRgb({hsv.h, 1, 1}, hr, hg, hb);
        // One anti-aliased rounded mask over plain fills: stacking three rounded fills left a light fringe
        // on the corners where their soft edges overlapped.
        ComPtr<ID2D1RoundedRectangleGeometry> svg;
        Factory()->CreateRoundedRectangleGeometry({sv, 8 * ui, 8 * ui}, &svg);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), svg.Get()), nullptr);
        Brush({hr, hg, hb, 1});
        rt->FillRectangle(sv, br.Get());
        D2D1_GRADIENT_STOP white[2] = {{0, C(255, 255, 255, 1)}, {1, C(255, 255, 255, 0)}};
        rt->FillRectangle(sv, Gradient({sv.left, 0}, {sv.right, 0}, white, 2).Get());
        D2D1_GRADIENT_STOP black[2] = {{0, C(0, 0, 0, 0)}, {1, C(0, 0, 0, 1)}};
        rt->FillRectangle(sv, Gradient({0, sv.top}, {0, sv.bottom}, black, 2).Get());
        rt->PopLayer();
        Knob({sv.left + hsv.s * (sv.right - sv.left), sv.top + (1 - hsv.v) * (sv.bottom - sv.top)}, 6 * ui, solid);
        // Hue strip.
        D2D1_RECT_F hu = HueRect();
        D2D1_GRADIENT_STOP rainbow[7];
        for (int i = 0; i < 7; ++i) {
            float r, g, b;
            ToRgb({std::min(359.9f, i * 60.f), 1, 1}, r, g, b);
            rainbow[i] = {i / 6.f, {r, g, b, 1}};
        }
        D2D1_ROUNDED_RECT hr2{hu, (hu.bottom - hu.top) / 2, (hu.bottom - hu.top) / 2};
        rt->FillRoundedRectangle(hr2, Gradient({hu.left, 0}, {hu.right, 0}, rainbow, 7).Get());
        Knob({hu.left + hsv.h / 360 * (hu.right - hu.left), (hu.top + hu.bottom) / 2}, 6 * ui, {hr, hg, hb, 1});
        // Alpha strip over a checkerboard.
        D2D1_RECT_F al = AlphaRect();
        float rad = (al.bottom - al.top) / 2;
        ComPtr<ID2D1RoundedRectangleGeometry> alg;
        Factory()->CreateRoundedRectangleGeometry({al, rad, rad}, &alg);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), alg.Get()), nullptr);
        Checker(al, std::round(6 * ui));
        D2D1_GRADIENT_STOP fade[2] = {{0, {cur[0], cur[1], cur[2], 0}}, {1, solid}};
        rt->FillRectangle(al, Gradient({al.left, 0}, {al.right, 0}, fade, 2).Get());
        rt->PopLayer();
        Knob({al.left + cur[3] * (al.right - al.left), (al.top + al.bottom) / 2}, 6 * ui, solid);
        // Old | new preview.
        D2D1_RECT_F pv = PreviewRect();
        ComPtr<ID2D1RoundedRectangleGeometry> pvg;
        Factory()->CreateRoundedRectangleGeometry({pv, 6 * ui, 6 * ui}, &pvg);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), pvg.Get()), nullptr);
        Checker(pv, std::round(5 * ui));
        float mid = (pv.left + pv.right) / 2;
        Brush({orig[0], orig[1], orig[2], orig[3]});
        rt->FillRectangle({pv.left, pv.top, mid, pv.bottom}, br.Get());
        Brush({cur[0], cur[1], cur[2], cur[3]});
        rt->FillRectangle({mid, pv.top, pv.right, pv.bottom}, br.Get());
        rt->PopLayer();
        // HEX and alpha fields.
        wchar_t hex[16], alpha[16];
        swprintf_s(hex, L"#%06X", Pack(cur) & 0xFFFFFF);
        swprintf_s(alpha, L"%d%%", (int)std::lround(cur[3] * 100));
        Field(HexBox(), editingHex ? editText : hex, editingHex, Hover(HexBox()));
        Field(AlphaBox(), editingAlpha ? editText : alpha, editingAlpha, Hover(AlphaBox()));
        // Eyedropper button.
        D2D1_RECT_F ey = EyeRect();
        Brush(eyedropper ? kAccent : Hover(ey) ? kFieldHover : kField);
        rt->FillRoundedRectangle({ey, 6 * ui, 6 * ui}, br.Get());
        {
            float cx = (ey.left + ey.right) / 2, cy = (ey.top + ey.bottom) / 2, s = 6 * ui;
            Brush(kText);
            rt->DrawLine({cx - s, cy + s}, {cx + s * .35f, cy - s * .35f}, br.Get(), 1.8f * ui);
            rt->FillEllipse(D2D1::Ellipse({cx + s * .55f, cy - s * .55f}, 2.6f * ui, 2.6f * ui), br.Get());
            rt->FillEllipse(D2D1::Ellipse({cx - s, cy + s}, 1.4f * ui, 1.4f * ui), br.Get());
        }
        // Swatches.
        Brush(kDim);
        D2D1_RECT_F lab{P(), Swatch(20).top - std::round(19 * ui), W() - P(), Swatch(20).top - std::round(2 * ui)};
        const wchar_t* recentLabel = Tr(L"Недавние", L"Recent");
        rt->DrawTextW(recentLabel, (UINT32)wcslen(recentLabel), smallFont.Get(), lab, br.Get());
        uint32_t now = Pack(cur);
        for (int i = 0; i < 30; ++i) {
            uint32_t c;
            if (i < 20) c = kPresets[i];
            else if (i - 20 < (int)recent.size()) c = recent[i - 20];
            else {
                D2D1_RECT_F r = Swatch(i);
                Brush(kField);
                rt->DrawRoundedRectangle({{r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f}, 5 * ui, 5 * ui}, br.Get(), ui);
                continue;
            }
            D2D1_RECT_F r = Swatch(i);
            ComPtr<ID2D1RoundedRectangleGeometry> g;
            Factory()->CreateRoundedRectangleGeometry({r, 5 * ui, 5 * ui}, &g);
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.Get()), nullptr);
            float f[4];
            Unpack(c, f);
            if (f[3] < 1) Checker(r, std::round(4 * ui));
            Brush({f[0], f[1], f[2], f[3]});
            rt->FillRectangle(r, br.Get());
            if (c == 0) {  // "no colour"
                Brush(C(242, 60, 60));
                rt->DrawLine({r.left, r.bottom}, {r.right, r.top}, br.Get(), 1.6f * ui);
            }
            rt->PopLayer();
            bool sel = c == now, hov = Hover(r);
            if (sel || hov) {
                Brush(sel ? kAccent : C(255, 255, 255, .7f));
                rt->DrawRoundedRectangle({{r.left - 1.5f * ui, r.top - 1.5f * ui, r.right + 1.5f * ui, r.bottom + 1.5f * ui}, 6 * ui, 6 * ui},
                                         br.Get(), 1.5f * ui);
            } else {
                Brush(C(255, 255, 255, .12f));
                rt->DrawRoundedRectangle({{r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f}, 5 * ui, 5 * ui}, br.Get(), ui);
            }
        }
        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
            rt.Reset();
            br.Reset();
        }
    }

    void Invalidate() { InvalidateRect(hwnd, nullptr, FALSE); }

    void DragTo(int x, int y) {
        if (drag == D_SV) {
            D2D1_RECT_F r = SvRect();
            hsv.s = std::clamp((x - r.left) / (r.right - r.left), 0.f, 1.f);
            hsv.v = 1 - std::clamp((y - r.top) / (r.bottom - r.top), 0.f, 1.f);
            SetFromHsv();
        } else if (drag == D_Hue) {
            D2D1_RECT_F r = HueRect();
            hsv.h = std::clamp((x - r.left) / (r.right - r.left), 0.f, .9999f) * 360;
            SetFromHsv();
        } else if (drag == D_Alpha) {
            D2D1_RECT_F r = AlphaRect();
            cur[3] = std::clamp((x - r.left) / (r.right - r.left), 0.f, 1.f);
        }
        Invalidate();
    }

    void CommitField() {
        if (editingHex) {
            std::wstring t = editText;
            if (!t.empty() && t[0] == L'#') t.erase(0, 1);
            if (t.size() == 3) t = {t[0], t[0], t[1], t[1], t[2], t[2]};
            if (t.size() == 6 || t.size() == 8) {
                wchar_t* end;
                uint32_t v = wcstoul(t.c_str(), &end, 16);
                if (!*end) {
                    float a = cur[3];
                    Unpack(t.size() == 6 ? (v | 0xFF000000u) : (v << 8 | v >> 24), cur);  // RRGGBBAA like Figma
                    if (t.size() == 6) cur[3] = a;
                    SetFromRgb();
                }
            }
        } else if (editingAlpha) {
            cur[3] = std::clamp(_wtoi(editText.c_str()) / 100.f, 0.f, 1.f);
        }
        editingHex = editingAlpha = false;
        Invalidate();
    }

    void Finish(bool accept) {
        if (done) return;
        if (accept) CommitField();
        ok = accept;
        done = true;
    }

    void SampleScreen() {
        POINT p;
        GetCursorPos(&p);
        int x = p.x - vs.left, y = p.y - vs.top;
        if (screen.empty() || x < 0 || y < 0 || x >= screen.w || y >= screen.h) return;
        uint32_t c = screen.px[(size_t)y * screen.w + x];
        float a = cur[3];
        Unpack(c | 0xFF000000u, cur);
        cur[3] = a;
        SetFromRgb();
        Invalidate();
    }

    void MouseDown(int x, int y) {
        if (eyedropper) {  // click anywhere picks
            SampleScreen();
            eyedropper = false;
            screen = {};
            ReleaseCapture();
            Invalidate();
            return;
        }
        if (editingHex || editingAlpha) CommitField();
        D2D1_POINT_2F p{(float)x, (float)y};
        auto in = [&](D2D1_RECT_F r, float pad = 0) {
            return p.x >= r.left - pad && p.x < r.right + pad && p.y >= r.top - pad && p.y < r.bottom + pad;
        };
        float grab = 6 * ui;
        if (in(SvRect())) drag = D_SV;
        else if (in(HueRect(), grab)) drag = D_Hue;
        else if (in(AlphaRect(), grab)) drag = D_Alpha;
        if (drag != D_None) {
            SetCapture(hwnd);
            DragTo(x, y);
            return;
        }
        if (in(HexBox())) {
            editingHex = true;
            wchar_t hex[16];
            swprintf_s(hex, L"%06X", Pack(cur) & 0xFFFFFF);
            editText = hex;
        } else if (in(AlphaBox())) {
            editingAlpha = true;
            editText = std::to_wstring((int)std::lround(cur[3] * 100));
        } else if (in(EyeRect())) {
            vs = VirtualScreen();
            screen = CaptureRect(vs, false);
            eyedropper = true;
            SetCapture(hwnd);
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        } else {
            for (int i = 0; i < 30; ++i) {
                if (!in(Swatch(i))) continue;
                if (i < 20) Unpack(kPresets[i], cur);
                else if (i - 20 < (int)recent.size()) Unpack(recent[i - 20], cur);
                else break;
                SetFromRgb();
                break;
            }
        }
        Invalidate();
    }

    void OnChar(wchar_t ch) {
        if (!editingHex && !editingAlpha) return;
        if (ch == 8) {
            if (!editText.empty()) editText.pop_back();
        } else if (ch == L'\r') {
            CommitField();
            return;
        } else if (editingHex ? iswxdigit(ch) || ch == L'#' : iswdigit(ch)) {
            if (editText.size() < (editingHex ? 9u : 3u)) editText += (wchar_t)towupper(ch);
        }
        Invalidate();
    }
};

LRESULT CALLBACK PickerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* p = (Picker*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)l)->lpCreateParams);
        return DefWindowProcW(h, m, w, l);
    }
    if (!p) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        p->Render();
        ValidateRect(h, nullptr);
        return 0;
    case WM_SETCURSOR:
        if (p->eyedropper) {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }
        if (LOWORD(l) == HTCLIENT) {
            bool text = p->Hover(p->HexBox()) || p->Hover(p->AlphaBox());
            SetCursor(LoadCursorW(nullptr, text ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        p->MouseDown((short)LOWORD(l), (short)HIWORD(l));
        return 0;
    case WM_LBUTTONDBLCLK:
        p->MouseDown((short)LOWORD(l), (short)HIWORD(l));
        if (!p->editingHex && !p->editingAlpha && p->drag == D_None) {
            for (int i = 0; i < 30; ++i)
                if (p->Hover(p->Swatch(i))) p->Finish(true);  // double-click a swatch: pick and close
        }
        return 0;
    case WM_MOUSEMOVE:
        p->mouse = {(short)LOWORD(l), (short)HIWORD(l)};
        if (p->eyedropper) p->SampleScreen();
        else if (p->drag != D_None) p->DragTo(p->mouse.x, p->mouse.y);
        else {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            p->Invalidate();
        }
        return 0;
    case WM_MOUSELEAVE:
        p->mouse = {-1, -1};
        p->Invalidate();
        return 0;
    case WM_LBUTTONUP:
        if (p->drag != D_None) {
            p->drag = D_None;
            ReleaseCapture();
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            if (p->eyedropper) {
                p->eyedropper = false;
                p->screen = {};
                ReleaseCapture();
                p->Invalidate();
            } else if (p->editingHex || p->editingAlpha) {
                p->editingHex = p->editingAlpha = false;
                p->Invalidate();
            } else {
                p->Finish(false);
            }
        } else if (w == VK_RETURN && !p->editingHex && !p->editingAlpha) {
            p->Finish(true);
        }
        return 0;
    case WM_CHAR:
        p->OnChar((wchar_t)w);
        return 0;
    case WM_ACTIVATE:
        // Clicking anywhere else commits, like a Figma/Houdini popover.
        if (LOWORD(w) == WA_INACTIVE && !p->eyedropper) p->Finish(true);
        return 0;
    case WM_CLOSE:
        p->Finish(false);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool ShowColorPicker(HWND owner, POINT at, float rgba[4]) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.style = CS_DROPSHADOW | CS_DBLCLKS;
        wc.lpfnWndProc = PickerProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"ZipshotColorPicker";
        RegisterClassW(&wc);
        reg = true;
    }
    if (!rgba) return false;
    Picker p;
    std::copy(rgba, rgba + 4, p.orig);
    std::copy(rgba, rgba + 4, p.cur);
    p.SetFromRgb();
    p.recent = LoadRecent();
    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (auto fn = (GetDpiForMonitorFn)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor")) {
        UINT dx, dy;
        if (SUCCEEDED(fn(mon, 0, &dx, &dy))) dpi = dx;
    }
    p.ui = dpi / 96.0f;
    DWFactory()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, 13 * p.ui, L"", &p.font);
    p.font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWFactory()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, 11.5f * p.ui, L"", &p.smallFont);
    int w = (int)p.W(), h = (int)p.H();
    int x = std::clamp<int>(at.x, mi.rcWork.left, mi.rcWork.right - w);
    int y = at.y + (int)(4 * p.ui);
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    p.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"ZipshotColorPicker", L"", WS_POPUP, x, y, w, h, owner, nullptr,
                             g_inst, &p);
    if (!p.hwnd) return false;
    DWORD corner = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(p.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    COLORREF edge = RGB(64, 65, 72);
    DwmSetWindowAttribute(p.hwnd, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof(edge));
    ShowWindow(p.hwnd, SW_SHOW);
    SetForegroundWindow(p.hwnd);
    MSG msg;
    bool quit = false;
    MSG replay{};
    bool haveReplay = false;
    while (!p.done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            quit = true;
            break;
        }
        // A click outside commits the colour first and is then replayed, so the shape that click starts
        // already uses the new colour (it used to start with the old one and pick up the new colour a shape late).
        bool button = msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN || msg.message == WM_MBUTTONDOWN ||
                      msg.message == WM_NCLBUTTONDOWN;
        if (button && msg.hwnd != p.hwnd && !p.eyedropper) {
            p.Finish(true);
            replay = msg;
            haveReplay = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    SetWindowLongPtrW(p.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(p.hwnd);
    if (owner) SetForegroundWindow(owner);
    if (quit) PostQuitMessage(0);
    if (haveReplay) PostMessageW(replay.hwnd, replay.message, replay.wParam, replay.lParam);  // runs after the caller stored the colour
    if (!p.ok) return false;
    std::copy(p.cur, p.cur + 4, rgba);
    SaveRecent(Pack(p.cur));
    return true;
}

// Off-screen render of the picker (never activated) for visual checks.
bool ColorPickerTest(const std::wstring& out) {
    ShowColorPicker(nullptr, {-20000, -20000}, nullptr);  // registers the class only (rgba == nullptr)
    Picker p;
    float c[4] = {0, 1, 93 / 255.f, 1};
    std::copy(c, c + 4, p.orig);
    p.cur[0] = 0.3f, p.cur[1] = 0.64f, p.cur[2] = 1, p.cur[3] = 0.8f;
    p.SetFromRgb();
    p.recent = {0xFF00FF5D, 0xFFF23C3C, 0xFFFFC531, 0x80000000};
    p.ui = 1.5f;
    DWFactory()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, 13 * p.ui, L"", &p.font);
    p.font->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWFactory()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, 11.5f * p.ui, L"", &p.smallFont);
    int w = (int)p.W(), h = (int)p.H();
    p.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"ZipshotColorPicker", L"", WS_POPUP, -20000, -20000, w, h,
                             nullptr, nullptr, g_inst, &p);
    if (!p.hwnd) return false;
    p.mouse = {(int)p.Swatch(3).left + 3, (int)p.Swatch(3).top + 3};
    ShowWindow(p.hwnd, SW_SHOWNOACTIVATE);
    p.Render();
    void* bits;
    HBITMAP bmp = CreateDib(w, h, &bits);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bmp);
    PrintWindow(p.hwnd, dc, PW_RENDERFULLCONTENT);
    GdiFlush();
    Image shot;
    shot.w = w;
    shot.h = h;
    shot.px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)w * h);
    for (auto& px : shot.px) px |= 0xFF000000u;
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bmp);
    SetWindowLongPtrW(p.hwnd, GWLP_USERDATA, 0);
    DestroyWindow(p.hwnd);
    return EncodeImage(shot, out, false);
}
