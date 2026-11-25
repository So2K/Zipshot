#include "combine.h"
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

// ShareX "Image combiner" (ImageHelpers.CombineImages + ImageCombinerForm) and the shared combine-and-save pipeline.

using Microsoft::WRL::ComPtr;

std::vector<RECT> CombineLayout(const std::vector<SIZE>& sizes, bool horizontal, int alignment, int space, int wrapAfter,
                                SIZE& canvas) {
    std::vector<RECT> out(sizes.size());
    canvas = {0, 0};
    int n = (int)sizes.size(), per = wrapAfter > 0 ? wrapAfter : std::max(1, n);
    int x = 0, y = 0;
    for (int start = 0; start < n; start += per) {
        int end = std::min(n, start + per), maxSize = 0;
        for (int i = start; i < end; ++i) maxSize = std::max(maxSize, (int)(horizontal ? sizes[i].cy : sizes[i].cx));
        for (int i = start; i < end; ++i) {
            int w = sizes[i].cx, h = sizes[i].cy, cross = horizontal ? h : w;
            int off = alignment == 1 ? maxSize / 2 - cross / 2 : alignment == 2 ? maxSize - cross : 0;
            if (horizontal) {
                out[i] = {x, y + off, x + w, y + off + h};
                x += w + space;
            } else {
                out[i] = {x + off, y, x + off + w, y + h};
                y += h + space;
            }
            canvas.cx = std::max(canvas.cx, out[i].right);
            canvas.cy = std::max(canvas.cy, out[i].bottom);
        }
        if (horizontal) {
            x = 0;
            y += maxSize + space;
        } else {
            y = 0;
            x += maxSize + space;
        }
    }
    return out;
}

Image CombineImages(const std::vector<Image>& images, bool horizontal, int alignment, int space, int wrapAfter, bool autoFill) {
    std::vector<SIZE> sizes;
    for (const Image& im : images) sizes.push_back(im.empty() ? SIZE{0, 0} : SIZE{im.w, im.h});
    SIZE c;
    std::vector<RECT> rects = CombineLayout(sizes, horizontal, alignment, std::max(0, space), wrapAfter, c);
    Image out;
    if (c.cx <= 0 || c.cy <= 0) return out;
    if ((long long)c.cx * c.cy > 200'000'000) {  // ~800 MB; WIC and the clipboard give up long before
        Log(L"combine: canvas %dx%d too large", (int)c.cx, (int)c.cy);
        return out;
    }
    out.w = c.cx;
    out.h = c.cy;
    uint32_t bg = 0;
    if (autoFill && !images.empty() && !images[0].empty()) {
        bg = images[0].px[(size_t)(images[0].h - 1) * images[0].w + images[0].w - 1];
        if (!images[0].alpha) bg |= 0xFF000000u;
    }
    out.px.assign((size_t)out.w * out.h, bg);
    for (size_t i = 0; i < images.size(); ++i) {
        const Image& im = images[i];
        if (im.empty()) continue;
        for (int y = 0; y < im.h; ++y) {
            auto* dst = &out.px[(size_t)(rects[i].top + y) * out.w + rects[i].left];
            const auto* src = &im.px[(size_t)y * im.w];
            if (im.alpha) memcpy(dst, src, (size_t)im.w * 4);
            else for (int x = 0; x < im.w; ++x) dst[x] = src[x] | 0xFF000000u;
        }
    }
    // Captures can have undefined alpha bytes; image files can contain genuine transparency.
    // Preserve both sources even when the layout has no gaps or automatic fill is enabled.
    out.alpha = std::any_of(out.px.begin(), out.px.end(), [](uint32_t p) { return (p >> 24) != 255; });
    return out;
}

Image ScaleImage(const Image& src, int w, int h) {
    Image out;
    if (src.empty() || w <= 0 || h <= 0) return out;
    out.w = w;
    out.h = h;
    out.alpha = src.alpha;
    out.px.resize((size_t)w * h);
    std::vector<int> x0(w), x1(w);
    for (int x = 0; x < w; ++x) {
        x0[x] = (int)((long long)x * src.w / w);
        x1[x] = std::max(x0[x] + 1, (int)((long long)(x + 1) * src.w / w));
    }
    std::vector<uint32_t> acc((size_t)w * 4);
    for (int y = 0; y < h; ++y) {
        int y0 = (int)((long long)y * src.h / h), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * src.h / h));
        std::fill(acc.begin(), acc.end(), 0);
        for (int sy = y0; sy < y1; ++sy) {
            const uint32_t* row = &src.px[(size_t)sy * src.w];
            for (int x = 0; x < w; ++x) {
                uint32_t* a = &acc[(size_t)x * 4];
                for (int sx = x0[x]; sx < x1[x]; ++sx) {
                    uint32_t p = row[sx];
                    a[0] += p & 255;
                    a[1] += p >> 8 & 255;
                    a[2] += p >> 16 & 255;
                    a[3] += p >> 24;
                }
            }
        }
        uint32_t* dst = &out.px[(size_t)y * w];
        for (int x = 0; x < w; ++x) {
            uint32_t n = (uint32_t)((x1[x] - x0[x]) * (y1 - y0)), half = n / 2;
            const uint32_t* a = &acc[(size_t)x * 4];
            dst[x] = (a[0] + half) / n | (a[1] + half) / n << 8 | (a[2] + half) / n << 16 | (a[3] + half) / n << 24;
        }
    }
    return out;
}

// ---------- options ----------

static std::wstring IniFile() { return ExeDir() + L"\\zipshot.ini"; }

static std::wstring IniGet(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[64];
    GetPrivateProfileStringW(L"combiner", key, def, buf, (DWORD)std::size(buf), IniFile().c_str());
    return buf;
}

CombineOptions LoadCombineOptions() {
    CombineOptions o;
    o.horizontal = !_wcsicmp(IniGet(L"Orientation", L"Vertical").c_str(), L"Horizontal");
    std::wstring a = IniGet(L"Alignment", L"LeftOrTop");
    o.alignment = !_wcsicmp(a.c_str(), L"Center") ? 1 : !_wcsicmp(a.c_str(), L"RightOrBottom") ? 2 : 0;
    o.space = std::clamp(_wtoi(IniGet(L"Space", L"0").c_str()), 0, 500);
    o.wrapAfter = std::clamp(_wtoi(IniGet(L"WrapAfter", L"0").c_str()), 0, 100);
    std::wstring f = IniGet(L"AutoFillBackground", L"1");
    o.autoFill = f != L"0" && _wcsicmp(f.c_str(), L"false") != 0;
    return o;
}

void SaveCombineOptions(const CombineOptions& o) {
    std::wstring ini = IniFile();
    const wchar_t* align[] = {L"LeftOrTop", L"Center", L"RightOrBottom"};
    WritePrivateProfileStringW(L"combiner", L"Orientation", o.horizontal ? L"Horizontal" : L"Vertical", ini.c_str());
    WritePrivateProfileStringW(L"combiner", L"Alignment", align[std::clamp(o.alignment, 0, 2)], ini.c_str());
    WritePrivateProfileStringW(L"combiner", L"Space", std::to_wstring(o.space).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"combiner", L"WrapAfter", std::to_wstring(o.wrapAfter).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"combiner", L"AutoFillBackground", o.autoFill ? L"1" : L"0", ini.c_str());
}

// ---------- combine + save ----------

static std::atomic_uint g_combineJobs{0};
bool CombineBusy() { return g_combineJobs.load() != 0; }

void CombineFilesAsync(std::vector<std::wstring> paths, CombineOptions opt, std::function<void(const std::wstring&)> onSaved) {
    ++g_combineJobs;
    try {
    std::thread([paths = std::move(paths), opt, onSaved = std::move(onSaved)] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto sp = std::make_shared<Image>();
        {
            std::vector<Image> imgs;
            for (const std::wstring& p : paths) {
                Image im;
                if (LoadImageFile(p, im) && !im.empty()) imgs.push_back(std::move(im));
                else Log(L"combine: cannot load %s", p.c_str());
            }
            if (imgs.size() >= 2) *sp = CombineImages(imgs, opt.horizontal, opt.alignment, opt.space, opt.wrapAfter, opt.autoFill);
        }
        std::wstring path;
        bool ok = false;
        if (!sp->empty()) {
            path = NewOutputPath(L"png");
            ok = SaveImageAuto(*sp, path);
        }
        if (SUCCEEDED(hr)) CoUninitialize();
        RunOnMain([sp, path, ok, onSaved] {
            --g_combineJobs;
            if (!ok) {
                PlayUiSound(S_Error);
                MessageBoxW(g_main, Tr(L"Не удалось объединить изображения.", L"Could not combine the images."), L"Zipshot",
                            MB_ICONERROR);
                return;
            }
            Log(L"combined %s", path.c_str());
            if (GetSettingBool(L"AfterCopy", true)) CopyImageToClipboard(*sp);
            PlayUiSound(S_TaskDone);
            MainWindowNotifyCapture(path);
            if (onSaved) onSaved(path);
            if (GetSettingBool(L"AfterToast", true)) ShowToast(sp, path, Tr(L"Объединено", L"Combined"));
        });
    }).detach();
    } catch (...) {
        --g_combineJobs;
        PlayUiSound(S_Error);
    }
}

// ---------- dialog ----------

namespace {

const COLORREF kBg = RGB(39, 39, 39), kField = RGB(46, 46, 46), kFieldHover = RGB(58, 58, 58), kEdge = RGB(70, 70, 70),
               kText = RGB(231, 233, 234), kDim = RGB(150, 152, 154), kAccent = RGB(0, 138, 255),
               kAccentHover = RGB(40, 158, 255), kPreviewBg = RGB(30, 30, 30);

enum Hit { H_None, H_Or0, H_Or1, H_Al0, H_Al1, H_Al2, H_Space, H_SpaceMinus, H_SpacePlus, H_Wrap, H_WrapMinus, H_WrapPlus,
           H_Fill, H_Reverse, H_Ok, H_Cancel };
constexpr int kW = 720, kH = 440, kPadL = 16, kColW = 240;
constexpr int kMaxSpace = 500, kMaxWrap = 100;
constexpr UINT WM_LOADED = WM_APP + 1;

struct Loaded {  // written by the loader thread, read once on WM_LOADED
    std::mutex mu;
    bool done = false;
    std::atomic<bool> cancel{false};
    std::vector<Image> thumbs;
    std::vector<SIZE> full;
    double k = 1;
};

struct Dlg {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    std::vector<std::wstring> paths;
    CombineOptions opt;
    std::shared_ptr<Loaded> src;
    bool loaded = false;
    std::vector<Image> thumbs;
    std::vector<SIZE> full;
    double k = 1;
    HBITMAP prev = nullptr;
    int pw = 0, ph = 0;
    bool prevAlpha = false;
    SIZE result{};
    int hover = H_None, pressed = H_None, focus = H_None;
    bool fresh = false;  // the next typed digit replaces the field value
    bool tracking = false, done = false, ok = false;
    bool reversed = false;
};

int S(const Dlg& d, int v) { return MulDiv(v, (int)d.dpi, 96); }

void Fill(HDC dc, RECT r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

void Frame(HDC dc, RECT r, COLORREF c, int t = 1) {
    Fill(dc, {r.left, r.top, r.right, r.top + t}, c);
    Fill(dc, {r.left, r.bottom - t, r.right, r.bottom}, c);
    Fill(dc, {r.left, r.top, r.left + t, r.bottom}, c);
    Fill(dc, {r.right - t, r.top, r.right, r.bottom}, c);
}

// Logical layout rows of the left column (y of each row's label; controls sit 20 px below).
constexpr int kYOrient = 14, kYAlign = 74, kYSpace = 134, kYWrap = 194, kYFill = 256, kYReverse = 284;

RECT L(const Dlg& d, int x, int y, int w, int h) { return {S(d, x), S(d, y), S(d, x + w), S(d, y + h)}; }

RECT HitRect(const Dlg& d, int id) {
    int segW2 = kColW / 2, segW3 = kColW / 3;
    switch (id) {
    case H_Or0: return L(d, kPadL, kYOrient + 20, segW2, 28);
    case H_Or1: return L(d, kPadL + segW2, kYOrient + 20, kColW - segW2, 28);
    case H_Al0: return L(d, kPadL, kYAlign + 20, segW3, 28);
    case H_Al1: return L(d, kPadL + segW3, kYAlign + 20, segW3, 28);
    case H_Al2: return L(d, kPadL + 2 * segW3, kYAlign + 20, kColW - 2 * segW3, 28);
    case H_Space: return L(d, kPadL, kYSpace + 20, kColW - 64, 28);
    case H_SpaceMinus: return L(d, kPadL + kColW - 60, kYSpace + 20, 28, 28);
    case H_SpacePlus: return L(d, kPadL + kColW - 28, kYSpace + 20, 28, 28);
    case H_Wrap: return L(d, kPadL, kYWrap + 20, kColW - 64, 28);
    case H_WrapMinus: return L(d, kPadL + kColW - 60, kYWrap + 20, 28, 28);
    case H_WrapPlus: return L(d, kPadL + kColW - 28, kYWrap + 20, 28, 28);
    case H_Fill: return L(d, kPadL, kYFill, kColW, 22);
    case H_Reverse: return L(d, kPadL, kYReverse, kColW, 22);
    case H_Ok: return L(d, kPadL, kH - 16 - 30, kColW / 2 - 5, 30);
    case H_Cancel: return L(d, kPadL + kColW / 2 + 5, kH - 16 - 30, kColW - kColW / 2 - 5, 30);
    default: return {};
    }
}

RECT PreviewRect(const Dlg& d) { return L(d, kPadL + kColW + 16, 14, kW - (kPadL + kColW + 16) - 16, kH - 28); }

int HitTest(const Dlg& d, POINT p) {
    for (int id = H_Or0; id <= H_Cancel; ++id) {
        RECT r = HitRect(d, id);
        if (PtInRect(&r, p)) return id;
    }
    return H_None;
}

void CreateFonts(Dlg& d) {
    if (d.font) DeleteObject(d.font);
    if (d.bold) DeleteObject(d.bold);
    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfHeight = -MulDiv(9, (int)d.dpi, 72);
    d.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    d.bold = CreateFontIndirectW(&lf);
}

void Rebuild(Dlg& d) {
    if (d.prev) DeleteObject(d.prev);
    d.prev = nullptr;
    d.pw = d.ph = 0;
    CombineLayout(d.full, d.opt.horizontal, d.opt.alignment, d.opt.space, d.opt.wrapAfter, d.result);
    if (!d.loaded || d.thumbs.empty()) return;
    int space = (int)std::lround(d.opt.space * d.k);
    if (d.opt.space > 0) space = std::max(1, space);
    Image c = CombineImages(d.thumbs, d.opt.horizontal, d.opt.alignment, space, d.opt.wrapAfter, d.opt.autoFill);
    if (c.empty()) return;
    RECT box = PreviewRect(d);
    int bw = box.right - box.left - S(d, 24), bh = box.bottom - box.top - S(d, 24);
    double f = std::min({1.0, (double)bw / c.w, (double)bh / c.h});
    int w = std::max(1, (int)(c.w * f + 0.5)), h = std::max(1, (int)(c.h * f + 0.5));
    Image s = w == c.w && h == c.h ? std::move(c) : ScaleImage(c, w, h);
    void* bits = nullptr;
    d.prev = CreateDib(w, h, &bits);
    if (!d.prev) return;
    memcpy(bits, s.px.data(), s.px.size() * 4);
    if (!s.alpha)
        for (uint32_t* p = (uint32_t*)bits, *e = p + s.px.size(); p < e; ++p) *p |= 0xFF000000;
    d.pw = w;
    d.ph = h;
    d.prevAlpha = s.alpha;
}

void Redraw(Dlg& d) { InvalidateRect(d.hwnd, nullptr, FALSE); }

void Changed(Dlg& d) {
    Rebuild(d);
    Redraw(d);
}

void Text(HDC dc, const std::wstring& s, RECT r, COLORREF c, UINT fmt) {
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void PaintSegment(Dlg& d, HDC dc, int id, bool on, const wchar_t* label) {
    RECT r = HitRect(d, id);
    Fill(dc, r, on ? kAccent : d.hover == id ? kFieldHover : kField);
    Frame(dc, r, on ? kAccent : kEdge);
    SelectObject(dc, on ? d.bold : d.font);
    Text(dc, label, r, on ? RGB(255, 255, 255) : kText, DT_CENTER);
}

void PaintNumber(Dlg& d, HDC dc, int field, int value, const wchar_t* zeroText) {
    RECT r = HitRect(d, field);
    Fill(dc, r, d.hover == field && d.focus != field ? kFieldHover : kField);
    Frame(dc, r, d.focus == field ? kAccent : kEdge);
    SelectObject(dc, d.font);
    std::wstring s = std::to_wstring(value);
    RECT t{r.left + S(d, 10), r.top, r.right - S(d, 8), r.bottom};
    Text(dc, s, t, kText, DT_LEFT);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    if (d.focus == field) Fill(dc, {t.left + sz.cx + S(d, 1), r.top + S(d, 7), t.left + sz.cx + S(d, 2), r.bottom - S(d, 7)}, kText);
    if (value == 0 && zeroText) {
        RECT z{t.left + sz.cx + S(d, 10), r.top, t.right, r.bottom};
        Text(dc, zeroText, z, kDim, DT_LEFT);
    }
    for (int k = 0; k < 2; ++k) {
        int id = field + 1 + k;
        RECT b = HitRect(d, id);
        Fill(dc, b, d.pressed == id ? kAccent : d.hover == id ? kFieldHover : kField);
        Frame(dc, b, kEdge);
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2, a = S(d, 5), th = std::max(1, S(d, 1));
        Fill(dc, {cx - a, cy - th / 2, cx + a + 1, cy - th / 2 + th}, kText);
        if (k == 1) Fill(dc, {cx - th / 2, cy - a, cx - th / 2 + th, cy + a + 1}, kText);
    }
}

void PaintCheck(Dlg& d, HDC dc, int id, bool on, const wchar_t* label) {
    RECT r = HitRect(d, id);
    int bs = S(d, 16);
    RECT b{r.left, (r.top + r.bottom - bs) / 2, r.left + bs, (r.top + r.bottom - bs) / 2 + bs};
    Fill(dc, b, on ? kAccent : d.hover == id ? kFieldHover : kField);
    Frame(dc, b, on ? kAccent : kEdge);
    if (on) {
        HPEN pen = CreatePen(PS_SOLID, std::max(2, S(d, 2)), RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, pen);
        POINT pts[3] = {{b.left + bs * 4 / 16, b.top + bs * 8 / 16}, {b.left + bs * 7 / 16, b.top + bs * 11 / 16},
                        {b.left + bs * 12 / 16, b.top + bs * 5 / 16}};
        Polyline(dc, pts, 3);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    SelectObject(dc, d.font);
    RECT t{b.right + S(d, 8), r.top, r.right, r.bottom};
    Text(dc, label, t, d.hover == id ? RGB(255, 255, 255) : kText, DT_LEFT);
}

void PaintButton(Dlg& d, HDC dc, int id, bool primary, const wchar_t* label, bool enabled) {
    RECT r = HitRect(d, id);
    bool hot = enabled && (d.hover == id || d.pressed == id);
    COLORREF bg = primary ? (enabled ? (hot ? kAccentHover : kAccent) : RGB(40, 75, 110)) : (hot ? kFieldHover : kField);
    Fill(dc, r, bg);
    Frame(dc, r, primary ? bg : kEdge);
    SelectObject(dc, primary ? d.bold : d.font);
    Text(dc, label, r, primary ? (enabled ? RGB(255, 255, 255) : RGB(150, 170, 190)) : kText, DT_CENTER);
}

void Checker(HDC dc, RECT r, int cell) {
    for (int y = r.top, row = 0; y < r.bottom; y += cell, ++row)
        for (int x = r.left, col = 0; x < r.right; x += cell, ++col)
            Fill(dc, {x, y, std::min<LONG>(x + cell, r.right), std::min<LONG>(y + cell, r.bottom)},
                 (row + col) & 1 ? RGB(60, 60, 60) : RGB(80, 80, 80));
}

void Paint(Dlg& d) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(d.hwnd, &ps);
    RECT rc;
    GetClientRect(d.hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) { EndPaint(d.hwnd, &ps); return; }
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, d.font);
    SetBkMode(dc, TRANSPARENT);
    Fill(dc, rc, kBg);

    auto label = [&](int y, const wchar_t* s) {
        SelectObject(dc, d.font);
        Text(dc, s, L(d, kPadL, y, kColW, 18), kDim, DT_LEFT);
    };
    const CombineOptions& o = d.opt;
    label(kYOrient, Tr(L"Ориентация", L"Orientation"));
    PaintSegment(d, dc, H_Or0, !o.horizontal, Tr(L"Вертикально", L"Vertical"));
    PaintSegment(d, dc, H_Or1, o.horizontal, Tr(L"Горизонтально", L"Horizontal"));
    label(kYAlign, Tr(L"Выравнивание", L"Alignment"));
    PaintSegment(d, dc, H_Al0, o.alignment == 0, o.horizontal ? Tr(L"Сверху", L"Top") : Tr(L"Слева", L"Left"));
    PaintSegment(d, dc, H_Al1, o.alignment == 1, Tr(L"По центру", L"Center"));
    PaintSegment(d, dc, H_Al2, o.alignment == 2, o.horizontal ? Tr(L"Снизу", L"Bottom") : Tr(L"Справа", L"Right"));
    label(kYSpace, Tr(L"Отступ, px", L"Space (px)"));
    PaintNumber(d, dc, H_Space, o.space, nullptr);
    label(kYWrap, o.horizontal ? Tr(L"Перенос строки после", L"Wrap after (images per row)")
                               : Tr(L"Новый столбец после", L"Wrap after (images per column)"));
    PaintNumber(d, dc, H_Wrap, o.wrapAfter, Tr(L"выкл.", L"off"));
    PaintCheck(d, dc, H_Fill, o.autoFill, Tr(L"Автозаливка фона", L"Auto fill background"));
    PaintCheck(d, dc, H_Reverse, d.reversed, Tr(L"Обратный порядок", L"Reverse order"));

    SelectObject(dc, d.font);
    std::wstring info = std::wstring(Tr(L"Изображений: ", L"Images: ")) + std::to_wstring(d.loaded ? d.thumbs.size() : d.paths.size());
    Text(dc, info, L(d, kPadL, kH - 16 - 30 - 52, kColW, 20), kDim, DT_LEFT);
    if (d.loaded && d.result.cx > 0) {
        std::wstring sz = std::to_wstring(d.result.cx) + L" × " + std::to_wstring(d.result.cy) + L" px";
        Text(dc, std::wstring(Tr(L"Результат: ", L"Result: ")) + sz, L(d, kPadL, kH - 16 - 30 - 32, kColW, 20), kText, DT_LEFT);
    }
    PaintButton(d, dc, H_Ok, true, Tr(L"Объединить", L"Combine"), d.loaded && d.thumbs.size() >= 2);
    PaintButton(d, dc, H_Cancel, false, Tr(L"Отмена", L"Cancel"), true);

    RECT pr = PreviewRect(d);
    Fill(dc, pr, kPreviewBg);
    Frame(dc, pr, RGB(31, 31, 31));
    if (!d.loaded) {
        Text(dc, Tr(L"Загрузка…", L"Loading…"), pr, kDim, DT_CENTER);
    } else if (d.prev) {
        int x = (pr.left + pr.right - d.pw) / 2, y = (pr.top + pr.bottom - d.ph) / 2;
        HDC mem = CreateCompatibleDC(wdc);
        HGDIOBJ old = SelectObject(mem, d.prev);
        if (d.prevAlpha) {
            Checker(dc, {x, y, x + d.pw, y + d.ph}, S(d, 8));
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            AlphaBlend(dc, x, y, d.pw, d.ph, mem, 0, 0, d.pw, d.ph, bf);
        } else {
            BitBlt(dc, x, y, d.pw, d.ph, mem, 0, 0, SRCCOPY);
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        Frame(dc, {x - 1, y - 1, x + d.pw + 1, y + d.ph + 1}, RGB(70, 70, 70));
    } else {
        Text(dc, Tr(L"Нужно хотя бы два изображения", L"At least two images are needed"), pr, kDim, DT_CENTER);
    }

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(d.hwnd, &ps);
}

int* FieldValue(Dlg& d, int field) { return field == H_Space ? &d.opt.space : field == H_Wrap ? &d.opt.wrapAfter : nullptr; }
int FieldMax(int field) { return field == H_Space ? kMaxSpace : kMaxWrap; }

void Step(Dlg& d, int field, int delta) {
    int* v = FieldValue(d, field);
    if (!v) return;
    int nv = std::clamp(*v + delta, 0, FieldMax(field));
    if (nv != *v) {
        *v = nv;
        Changed(d);
    }
}

void Finish(Dlg& d, bool ok) {
    if (ok && !(d.loaded && d.thumbs.size() >= 2)) return;
    d.ok = ok;
    d.done = true;
    PostMessageW(d.hwnd, WM_NULL, 0, 0);
}

void Click(Dlg& d, int id) {
    CombineOptions& o = d.opt;
    switch (id) {
    case H_Or0:
    case H_Or1: o.horizontal = id == H_Or1; break;
    case H_Al0:
    case H_Al1:
    case H_Al2: o.alignment = id - H_Al0; break;
    case H_Space:
    case H_Wrap:
        d.focus = id;
        d.fresh = true;
        Redraw(d);
        return;
    case H_SpaceMinus: Step(d, H_Space, -1); return;
    case H_SpacePlus: Step(d, H_Space, +1); return;
    case H_WrapMinus: Step(d, H_Wrap, -1); return;
    case H_WrapPlus: Step(d, H_Wrap, +1); return;
    case H_Fill: o.autoFill = !o.autoFill; break;
    case H_Reverse:
        d.reversed = !d.reversed;
        std::reverse(d.paths.begin(), d.paths.end());
        std::reverse(d.thumbs.begin(), d.thumbs.end());
        std::reverse(d.full.begin(), d.full.end());
        break;
    default: return;
    }
    Changed(d);
}

void OnLoaded(Dlg& d) {
    std::lock_guard<std::mutex> lk(d.src->mu);
    if (!d.src->done || d.loaded) return;
    // Drop files that could not be decoded.
    std::vector<std::wstring> paths;
    for (size_t i = 0; i < d.paths.size() && i < d.src->thumbs.size(); ++i)
        if (!d.src->thumbs[i].empty()) {
            paths.push_back(d.paths[i]);
            d.thumbs.push_back(std::move(d.src->thumbs[i]));
            d.full.push_back(d.src->full[i]);
        }
    d.paths = std::move(paths);
    d.k = d.src->k;
    d.src->thumbs.clear();
    d.loaded = true;
    Changed(d);
}

SIZE ImageFileSize(IWICImagingFactory* f, const std::wstring& path) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (f && SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)))
        frame->GetSize(&w, &h);
    return {(LONG)w, (LONG)h};
}

// Loads downscaled copies with one common factor so the preview keeps the real proportions.
void LoaderProc(std::shared_ptr<Loaded> s, std::vector<std::wstring> paths, HWND notify) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IWICImagingFactory> f;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        double maxDim = 1, area = 1;
        for (const std::wstring& p : paths) {
            SIZE z = ImageFileSize(f.Get(), p);
            maxDim = std::max({maxDim, (double)z.cx, (double)z.cy});
            area += (double)z.cx * z.cy;
        }
        double k = std::min({1.0, 1000.0 / maxDim, std::sqrt(4e6 / area)});
        std::vector<Image> thumbs(paths.size());
        std::vector<SIZE> full(paths.size());
        for (size_t i = 0; i < paths.size() && !s->cancel; ++i) {
            Image im;
            if (!LoadImageFile(paths[i], im) || im.empty()) continue;
            full[i] = {im.w, im.h};
            int w = std::max(1, (int)std::lround(im.w * k)), h = std::max(1, (int)std::lround(im.h * k));
            thumbs[i] = w == im.w && h == im.h ? std::move(im) : ScaleImage(im, w, h);
        }
        std::lock_guard<std::mutex> lk(s->mu);
        s->thumbs = std::move(thumbs);
        s->full = std::move(full);
        s->k = k;
        s->done = true;
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    PostMessageW(notify, WM_LOADED, 0, 0);
}

SIZE WindowSize(UINT dpi) {
    RECT r{0, 0, MulDiv(kW, (int)dpi, 96), MulDiv(kH, (int)dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, dpi);
    return {r.right - r.left, r.bottom - r.top};
}

LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Dlg* d = (Dlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        d = (Dlg*)((CREATESTRUCTW*)l)->lpCreateParams;
        d->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
    }
    if (!d) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(*d); return 0;
    case WM_LOADED: OnLoaded(*d); return 0;
    case WM_MOUSEMOVE: {
        if (!d->tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            d->tracking = true;
        }
        int hit = HitTest(*d, {(short)LOWORD(l), (short)HIWORD(l)});
        if (hit != d->hover) {
            d->hover = hit;
            Redraw(*d);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        d->tracking = false;
        if (d->hover != H_None) {
            d->hover = H_None;
            Redraw(*d);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int hit = HitTest(*d, {(short)LOWORD(l), (short)HIWORD(l)});
        if (hit != H_Space && hit != H_Wrap && d->focus != H_None) {
            d->focus = H_None;
            Redraw(*d);
        }
        if (hit == H_Ok || hit == H_Cancel) {
            d->pressed = hit;
            SetCapture(h);
            Redraw(*d);
        } else {
            d->pressed = hit;
            Click(*d, hit);
            if (hit == H_SpaceMinus || hit == H_SpacePlus || hit == H_WrapMinus || hit == H_WrapPlus) {
                SetCapture(h);
                Redraw(*d);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int p = d->pressed;
        d->pressed = H_None;
        if (GetCapture() == h) ReleaseCapture();
        int hit = HitTest(*d, {(short)LOWORD(l), (short)HIWORD(l)});
        if ((p == H_Ok || p == H_Cancel) && hit == p) Finish(*d, p == H_Ok);
        Redraw(*d);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        ScreenToClient(h, &p);
        int hit = HitTest(*d, p);
        int field = hit == H_Space || hit == H_SpaceMinus || hit == H_SpacePlus ? H_Space
                    : hit == H_Wrap || hit == H_WrapMinus || hit == H_WrapPlus  ? H_Wrap
                                                                                 : H_None;
        if (field != H_None) Step(*d, field, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1);
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE: Finish(*d, false); return 0;
        case VK_RETURN: Finish(*d, true); return 0;
        case VK_TAB:
            d->focus = d->focus == H_Space ? H_Wrap : d->focus == H_Wrap ? H_None : H_Space;
            if (GetKeyState(VK_SHIFT) & 0x8000) d->focus = d->focus == H_Space ? H_None : d->focus == H_None ? H_Wrap : H_Space;
            d->fresh = true;
            Redraw(*d);
            return 0;
        case VK_UP:
        case VK_DOWN:
            if (d->focus != H_None) Step(*d, d->focus, w == VK_UP ? 1 : -1);
            return 0;
        case VK_BACK:
        case VK_DELETE:
            if (int* v = FieldValue(*d, d->focus)) {
                *v = w == VK_BACK && !d->fresh ? *v / 10 : 0;
                d->fresh = false;
                Changed(*d);
            }
            return 0;
        }
        break;
    case WM_CHAR:
        if (w >= L'0' && w <= L'9') {
            if (int* v = FieldValue(*d, d->focus)) {
                int nv = d->fresh ? (int)(w - L'0') : *v * 10 + (int)(w - L'0');
                d->fresh = false;
                *v = std::min(nv, FieldMax(d->focus));
                Changed(*d);
            }
        }
        return 0;
    case WM_DPICHANGED: {
        d->dpi = HIWORD(w);
        CreateFonts(*d);
        const RECT* r = (const RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Changed(*d);
        return 0;
    }
    case WM_CLOSE: Finish(*d, false); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool ShowCombineDialog(HWND owner, std::vector<std::wstring>& paths, CombineOptions& opt) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = g_inst;
        wc.hIcon = g_icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ZipshotCombineDialog";
        registered = RegisterClassExW(&wc) != 0;
    }
    Dlg d;
    d.paths = paths;
    d.opt = opt;
    d.dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    CreateFonts(d);
    SIZE s = WindowSize(d.dpi);
    RECT orc{};
    if (owner) GetWindowRect(owner, &orc);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&orc, MONITOR_DEFAULTTONEAREST), &mi);
    int x = (int)(orc.left + orc.right - s.cx) / 2, y = (int)(orc.top + orc.bottom - s.cy) / 2, sw = (int)s.cx, sh = (int)s.cy;
    x = std::clamp(x, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - sw));
    y = std::clamp(y, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - sh));
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ZipshotCombineDialog", Tr(L"Объединение изображений", L"Image combiner"),
                             WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, s.cx, s.cy, owner, nullptr, g_inst, &d);
    if (!h) {
        DeleteObject(d.font);
        DeleteObject(d.bold);
        return false;
    }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE
    Rebuild(d);
    d.src = std::make_shared<Loaded>();
    std::thread(LoaderProc, d.src, d.paths, h).detach();

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    MSG m;
    while (!d.done && IsWindow(h)) {
        int r = GetMessageW(&m, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage((int)m.wParam);
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    d.src->cancel = true;
    if (owner) EnableWindow(owner, TRUE);
    if (IsWindow(h)) {
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
    if (d.prev) DeleteObject(d.prev);
    DeleteObject(d.font);
    DeleteObject(d.bold);
    if (!d.ok) return false;
    opt = d.opt;
    SaveCombineOptions(opt);
    paths = d.paths;
    return true;
}
