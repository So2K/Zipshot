#include "shapes_ext.h"
#include "clipboard_dib.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <random>

using Microsoft::WRL::ComPtr;

namespace shapes_ext {
namespace {

IWICImagingFactory* Wic() {  // per thread: the sticker picker decodes thumbnails on a worker thread
    thread_local ComPtr<IWICImagingFactory> f;
    if (!f) CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f.Get();
}

ID2D1Factory* Factory() {
    static ComPtr<ID2D1Factory> f;
    if (!f) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf());
    return f.Get();
}

bool FromSource(IWICBitmapSource* src, Image& out) {
    ComPtr<IWICFormatConverter> conv;
    auto* f = Wic();
    if (!src || !f || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT w, h;
    conv->GetSize(&w, &h);
    if (!w || !h) return false;
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)(out.px.size() * 4), (BYTE*)out.px.data()))) return false;
    out.alpha = false;
    for (uint32_t p : out.px)
        if ((p >> 24) != 255) { out.alpha = true; break; }
    return true;
}

bool DecodeMemory(const void* data, size_t size, Image& out) {
    ComPtr<IWICStream> s;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (!Wic() || FAILED(Wic()->CreateStream(&s)) || FAILED(s->InitializeFromMemory((BYTE*)data, (DWORD)size)) ||
        FAILED(Wic()->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) || FAILED(dec->GetFrame(0, &frame)))
        return false;
    return FromSource(frame.Get(), out);
}

uint32_t Premul(uint32_t p) {
    uint32_t a = p >> 24;
    if (a == 255) return p;
    if (a == 0) return 0;
    return a << 24 | ((p >> 16 & 255) * a / 255) << 16 | ((p >> 8 & 255) * a / 255) << 8 | (p & 255) * a / 255;
}

uint32_t Straight(uint32_t p) {
    uint32_t a = p >> 24;
    if (a == 0 || a == 255) return p;
    return a << 24 | std::min(255u, (p >> 16 & 255) * 255 / a) << 16 | std::min(255u, (p >> 8 & 255) * 255 / a) << 8 |
           std::min(255u, (p & 255) * 255 / a);
}

bool IsImageFile(const wchar_t* p) {
    const wchar_t* e = PathFindExtensionW(p);
    for (const wchar_t* x : {L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".gif", L".bmp", L".tif", L".tiff", L".webp"})
        if (_wcsicmp(e, x) == 0) return true;
    return false;
}

Image Crop(const Image& src, int x, int y, int w, int h) {
    Image out;
    out.w = w;
    out.h = h;
    out.alpha = src.alpha;
    out.px.resize((size_t)w * h);
    for (int j = 0; j < h; ++j) std::copy_n(&src.px[(size_t)(y + j) * src.w + x], w, &out.px[(size_t)j * w]);
    return out;
}

// Fills `pts` (a closed polygon) with the part's own pixels over `bg`, like ShareX's TextureBrush FillPolygon.
Image FillPolygon(const Image& part, const std::vector<POINT>& pts, D2D1_COLOR_F bg) {
    Image out;
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1RenderTarget> t;
    if (pts.size() < 3 || FAILED(Wic()->CreateBitmap(part.w, part.h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)))
        return part;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(Factory()->CreateWicBitmapRenderTarget(bmp.Get(), p, &t))) return part;
    std::vector<uint32_t> pm(part.px.size());
    for (size_t i = 0; i < pm.size(); ++i) pm[i] = Premul(part.alpha ? part.px[i] : part.px[i] | 0xFF000000u);
    ComPtr<ID2D1Bitmap> src;
    ComPtr<ID2D1BitmapBrush> brush;
    ComPtr<ID2D1PathGeometry> geo;
    ComPtr<ID2D1GeometrySink> sink;
    t->BeginDraw();
    t->Clear(bg);
    t->CreateBitmap(D2D1::SizeU(part.w, part.h), pm.data(), part.w * 4,
                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &src);
    t->CreateBitmapBrush(src.Get(), &brush);
    Factory()->CreatePathGeometry(&geo);
    geo->Open(&sink);
    sink->BeginFigure({(float)pts[0].x, (float)pts[0].y}, D2D1_FIGURE_BEGIN_FILLED);
    for (size_t i = 1; i < pts.size(); ++i) sink->AddLine({(float)pts[i].x, (float)pts[i].y});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    if (bg.a > 0) {  // opaque background: the polygon covers the part, the rest is bg
        t->FillGeometry(geo.Get(), brush.Get());
    } else {
        t->FillGeometry(geo.Get(), brush.Get());
    }
    t->EndDraw();
    out.w = part.w;
    out.h = part.h;
    out.px.resize(part.px.size());
    bmp->CopyPixels(nullptr, part.w * 4, (UINT)out.px.size() * 4, (BYTE*)out.px.data());
    out.alpha = false;
    for (auto& q : out.px) {
        q = Straight(q);
        if ((q >> 24) != 255) out.alpha = true;
    }
    return out;
}

enum Side { Top = 1, Bottom = 2, Left = 4, Right = 8 };

int RandomBelow(int n) {
    static std::mt19937 rng{GetTickCount()};
    return n <= 0 ? 0 : (int)(rng() % (unsigned)n);
}

// ImageHelpers.TornEdges (curvedEdges false).
Image TornEdges(const Image& bmp, int depth, int range, int sides, bool random, D2D1_COLOR_F bg) {
    if (depth < 1 || range < 1 || !sides) return bmp;
    std::vector<POINT> pts;
    int hc = bmp.w / range, vc = bmp.h / range;
    if (hc < 2 && vc < 2) return bmp;
    auto has = [&](int s) { return (sides & s) != 0; };
    if (has(Top) && hc > 1) {
        int sx = has(Left) && vc > 1 ? depth : 0, ex = has(Right) && vc > 1 ? bmp.w - depth : bmp.w;
        for (int x = sx; x < ex; x += range) pts.push_back({x, random ? RandomBelow(depth) : ((x / range) & 1) * depth});
    } else {
        pts.push_back({0, 0});
        pts.push_back({bmp.w, 0});
    }
    if (has(Right) && vc > 1) {
        int sy = has(Top) && hc > 1 ? depth : 0, ey = has(Bottom) && hc > 1 ? bmp.h - depth : bmp.h;
        for (int y = sy; y < ey; y += range) pts.push_back({bmp.w - depth + (random ? RandomBelow(depth) : ((y / range) & 1) * depth), y});
    } else {
        pts.push_back({bmp.w, 0});
        pts.push_back({bmp.w, bmp.h});
    }
    if (has(Bottom) && hc > 1) {
        int sx = has(Right) && vc > 1 ? bmp.w - depth : bmp.w, ex = has(Left) && vc > 1 ? depth : 0;
        for (int x = sx; x >= ex; x = (x / range - 1) * range)
            pts.push_back({x, bmp.h - depth + (random ? RandomBelow(depth) : ((x / range) & 1) * depth)});
    } else {
        pts.push_back({bmp.w, bmp.h});
        pts.push_back({0, bmp.h});
    }
    if (has(Left) && vc > 1) {
        int sy = has(Bottom) && hc > 1 ? bmp.h - depth : bmp.h, ey = has(Top) && hc > 1 ? depth : 0;
        for (int y = sy; y >= ey; y = (y / range - 1) * range) pts.push_back({random ? RandomBelow(depth) : ((y / range) & 1) * depth, y});
    } else {
        pts.push_back({0, bmp.h});
        pts.push_back({0, 0});
    }
    std::vector<POINT> uniq;  // points.Distinct()
    for (POINT p : pts)
        if (std::none_of(uniq.begin(), uniq.end(), [&](POINT q) { return q.x == p.x && q.y == p.y; })) uniq.push_back(p);
    return FillPolygon(bmp, uniq, bg);
}

// ImageHelpers.WavyEdges.
Image WavyEdges(const Image& bmp, int depth, int range, int sides, D2D1_COLOR_F bg) {
    if (depth < 1 || range < 1 || !sides) return bmp;
    auto has = [&](int s) { return (sides & s) != 0; };
    std::vector<POINT> pts;
    int hwc = std::max(2, (bmp.w / range + 1) / 2 * 2) - 1, vwc = std::max(2, (bmp.h / range + 1) / 2 * 2) - 1;
    int hwr = bmp.w / hwc, vwr = bmp.h / vwc;
    int step = std::min(std::max(1, range / depth), 10);
    auto wave = [](int t, int mx, int d) { return (int)((1 - std::cos(t * 3.14159265358979 / mx)) * d / 2); };
    if (has(Top)) {
        int sx = has(Left) ? depth : 0, ex = has(Right) ? bmp.w - depth : bmp.w;
        for (int x = sx; x < ex; x += step) pts.push_back({x, wave(x, hwr, depth)});
        pts.push_back({ex, wave(ex, hwr, depth)});
    } else {
        pts.push_back({0, 0});
    }
    if (has(Right)) {
        int sy = has(Top) ? depth : 0, ey = has(Bottom) ? bmp.h - depth : bmp.h;
        for (int y = sy; y < ey; y += step) pts.push_back({bmp.w - depth + wave(y, vwr, depth), y});
        pts.push_back({bmp.w - depth + wave(ey, vwr, depth), ey});
    } else {
        pts.push_back({bmp.w, pts.back().y});
    }
    if (has(Bottom)) {
        int sx = has(Right) ? bmp.w - depth : bmp.w, ex = has(Left) ? depth : 0;
        for (int x = sx; x >= ex; x -= step) pts.push_back({x, bmp.h - depth + wave(x, hwr, depth)});
        pts.push_back({ex, bmp.h - depth + wave(ex, hwr, depth)});
    } else {
        pts.push_back({pts.back().x, bmp.h});
    }
    if (has(Left)) {
        int sy = has(Bottom) ? bmp.h - depth : bmp.h, ey = has(Top) ? depth : 0;
        for (int y = sy; y >= ey; y -= step) pts.push_back({wave(y, vwr, depth), y});
        pts.push_back({wave(ey, vwr, depth), ey});
    } else {
        pts.push_back({0, pts.back().y});
    }
    if (!has(Top)) pts[0] = {pts.back().x, 0};
    return FillPolygon(bmp, pts, bg);
}

Image CutOutEffect(const Image& part, int edge, int effect, int size, D2D1_COLOR_F bg) {
    switch (effect) {
    case 1: return TornEdges(part, size, size, edge, false, bg);
    case 2: return TornEdges(part, size, size * 2, edge, true, bg);
    case 3: return WavyEdges(part, size, size * 5, edge, bg);
    default: return part;
    }
}

}  // namespace

// ---------------------------------------------------------------- images

bool LoadImagePremul(const std::wstring& path, Image& out) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (!Wic() || FAILED(Wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)))
        return false;
    return FromSource(frame.Get(), out);
}

Image ResizeLimit(const Image& src, int size) {
    if (src.w <= size && src.h <= size) return src;
    int w = size, h = size;
    double rx = (double)size / src.w, ry = (double)size / src.h;
    if (rx < ry) h = (int)std::round(src.h * rx);
    else if (rx > ry) w = (int)std::round(src.w * ry);
    w = std::max(1, w);
    h = std::max(1, h);
    ComPtr<IWICBitmap> b;
    ComPtr<IWICBitmapScaler> sc;
    Image out;
    if (FAILED(Wic()->CreateBitmapFromMemory(src.w, src.h, GUID_WICPixelFormat32bppPBGRA, src.w * 4, (UINT)src.px.size() * 4,
                                             (BYTE*)src.px.data(), &b)) ||
        FAILED(Wic()->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(b.Get(), w, h, WICBitmapInterpolationModeFant)) ||
        !FromSource(sc.Get(), out))
        return src;
    return out;
}

Image PremulCopy(const Image& s) {
    Image out = s;
    for (auto& p : out.px) p = s.alpha ? Premul(p) : (p | 0xFF000000u);
    return out;
}

Image CropPremul(const Image& s, RECT r) {
    r.left = std::max<LONG>(r.left, 0);
    r.top = std::max<LONG>(r.top, 0);
    r.right = std::min<LONG>(r.right, s.w);
    r.bottom = std::min<LONG>(r.bottom, s.h);
    if (r.right <= r.left || r.bottom <= r.top) return {};
    return PremulCopy(Crop(s, r.left, r.top, r.right - r.left, r.bottom - r.top));
}

// ---------------------------------------------------------------- clipboard

bool ClipboardImage(HWND owner, Image& out) {
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    static UINT png = RegisterClipboardFormatW(L"PNG");
    if (HANDLE h = IsClipboardFormatAvailable(png) ? GetClipboardData(png) : nullptr) {
        if (void* p = GlobalLock(h)) {
            ok = DecodeMemory(p, GlobalSize(h), out);
            GlobalUnlock(h);
        }
    }
    for (UINT fmt : {(UINT)CF_DIBV5, (UINT)CF_DIB}) {
        if (ok) break;
        HANDLE h = IsClipboardFormatAvailable(fmt) ? GetClipboardData(fmt) : nullptr;
        if (!h) continue;
        auto* bi = (const BITMAPINFOHEADER*)GlobalLock(h);
        if (!bi) continue;
        size_t size = GlobalSize(h);
        // Wrap the DIB in a BMP file header and let WIC decode it.
        WORD bitCount = 0;
        std::vector<BYTE> file;
        bool wrapped = detail::WrapClipboardDib(bi, size, file, bitCount);
        GlobalUnlock(h);
        if (!wrapped) continue;
        ok = DecodeMemory(file.data(), file.size(), out);
        if (ok && bitCount == 32) {
            bool anyAlpha = std::any_of(out.px.begin(), out.px.end(), [](uint32_t p) { return (p >> 24) != 0; });
            if (!anyAlpha || fmt == CF_DIB) {  // plain DIBs carry no meaningful alpha
                for (auto& p : out.px) p |= 0xFF000000u;
                out.alpha = false;
            }
        }
    }
    if (!ok) {
        if (HANDLE h = IsClipboardFormatAvailable(CF_HDROP) ? GetClipboardData(CF_HDROP) : nullptr) {
            HDROP drop = (HDROP)h;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n && !ok; ++i) {
                wchar_t f[MAX_PATH];
                if (DragQueryFileW(drop, i, f, MAX_PATH) && IsImageFile(f)) ok = LoadImagePremul(f, out);
            }
        }
    }
    CloseClipboard();
    return ok;
}

bool ClipboardText(HWND owner, std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    text.clear();
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* t = (const wchar_t*)GlobalLock(h)) {
            size_t count = GlobalSize(h) / sizeof(wchar_t);
            for (size_t i = 0; i < count && t[i]; ++i)
                if (t[i] != L'\r') text += t[i];
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    size_t a = text.find_first_not_of(L" \t\n"), b = text.find_last_not_of(L" \t\n");  // ShareX trims
    text = a == std::wstring::npos ? L"" : text.substr(a, b - a + 1);
    return !text.empty();
}

// ---------------------------------------------------------------- cursors

namespace {
struct CursorType { const wchar_t* name; LPCWSTR id; };
const CursorType kCursors[] = {
    {L"AppStarting", IDC_APPSTARTING}, {L"Arrow", IDC_ARROW}, {L"Cross", IDC_CROSS}, {L"Default", IDC_ARROW},
    {L"Hand", IDC_HAND}, {L"Help", IDC_HELP}, {L"IBeam", IDC_IBEAM}, {L"No", IDC_NO}, {L"SizeAll", IDC_SIZEALL},
    {L"SizeNESW", IDC_SIZENESW}, {L"SizeNS", IDC_SIZENS}, {L"SizeNWSE", IDC_SIZENWSE}, {L"SizeWE", IDC_SIZEWE},
    {L"UpArrow", IDC_UPARROW}, {L"WaitCursor", IDC_WAIT},
};
}  // namespace

int CursorTypeCount() { return (int)std::size(kCursors); }
const wchar_t* CursorTypeName(int i) { return kCursors[std::clamp(i, 0, CursorTypeCount() - 1)].name; }

Image CursorImage(int i) {
    Image out;
    HCURSOR c = LoadCursorW(nullptr, kCursors[std::clamp(i, 0, CursorTypeCount() - 1)].id);
    ICONINFO ii{};
    if (!c || !GetIconInfo(c, &ii)) return out;
    BITMAP bm{};
    int w = 32, h = 32;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) w = bm.bmWidth, h = bm.bmHeight;
    else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm)) w = bm.bmWidth, h = bm.bmHeight / 2;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    // Draw over black and over white: the difference gives alpha, the black pass the premultiplied colour.
    void *kb, *wb;
    HBITMAP kbmp = CreateDib(w, h, &kb), wbmp = CreateDib(w, h, &wb);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, kbmp);
    PatBlt(dc, 0, 0, w, h, BLACKNESS);
    DrawIconEx(dc, 0, 0, c, w, h, 0, nullptr, DI_NORMAL);
    SelectObject(dc, wbmp);
    PatBlt(dc, 0, 0, w, h, WHITENESS);
    DrawIconEx(dc, 0, 0, c, w, h, 0, nullptr, DI_NORMAL);
    GdiFlush();
    SelectObject(dc, old);
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h);
    out.alpha = true;
    auto* K = (uint32_t*)kb;
    auto* W = (uint32_t*)wb;
    for (size_t k = 0; k < out.px.size(); ++k) {
        int d = 0;
        for (int sh : {0, 8, 16}) d = std::max(d, (int)(W[k] >> sh & 255) - (int)(K[k] >> sh & 255));
        uint32_t a = (uint32_t)std::clamp(255 - d, 0, 255);
        uint32_t r = std::min(a, K[k] >> 16 & 255), g = std::min(a, K[k] >> 8 & 255), b = std::min(a, K[k] & 255);
        out.px[k] = a << 24 | r << 16 | g << 8 | b;
    }
    DeleteDC(dc);
    DeleteObject(kbmp);
    DeleteObject(wbmp);
    return out;
}

// ---------------------------------------------------------------- steps

std::wstring StepLabel(int n, int type) {
    if (type == 1 || type == 2) {  // Helpers.NumberToLetters
        std::wstring r;
        for (int num = n; --num >= 0; num /= 26) r.insert(r.begin(), (wchar_t)((type == 1 ? L'A' : L'a') + num % 26));
        return r;
    }
    if (type == 3 || type == 4) {  // Helpers.NumberToRomanNumeral
        static const std::pair<int, const wchar_t*> k[] = {{1000, L"M"}, {900, L"CM"}, {500, L"D"}, {400, L"CD"}, {100, L"C"},
                                                           {90, L"XC"},  {50, L"L"},   {40, L"XL"}, {10, L"X"},   {9, L"IX"},
                                                           {5, L"V"},    {4, L"IV"},   {1, L"I"}};
        std::wstring r;
        int num = n;
        for (auto& [v, s] : k)
            while (num >= v) r += s, num -= v;
        if (type == 4)
            for (auto& ch : r) ch = (wchar_t)towlower(ch);
        return r;
    }
    return std::to_wstring(n);
}

// ---------------------------------------------------------------- cut out

Image CutOutMiddle(const Image& bmp, bool horizontal, int start, int size, int effect, int effectSize, D2D1_COLOR_F bg) {
    if (bmp.empty() || size <= 0) return bmp;
    Image first, second;
    if (start > 0) {
        first = horizontal ? Crop(bmp, 0, 0, std::min(start, bmp.w), bmp.h) : Crop(bmp, 0, 0, bmp.w, std::min(start, bmp.h));
        first = CutOutEffect(first, horizontal ? Right : Bottom, effect, effectSize, bg);
    }
    int dim = horizontal ? bmp.w : bmp.h;
    if (start + size < dim) {
        int end = std::max(start + size, 0);
        second = horizontal ? Crop(bmp, end, 0, bmp.w - end, bmp.h) : Crop(bmp, 0, end, bmp.w, bmp.h - end);
        second = CutOutEffect(second, horizontal ? Left : Top, effect, effectSize, bg);
    }
    if (first.empty()) return second.empty() ? bmp : second;
    if (second.empty()) return first;
    // ImageHelpers.CombineImages: side by side (or stacked), top/left aligned.
    Image out;
    out.alpha = first.alpha || second.alpha;
    out.w = horizontal ? first.w + second.w : std::max(first.w, second.w);
    out.h = horizontal ? std::max(first.h, second.h) : first.h + second.h;
    out.px.assign((size_t)out.w * out.h, 0);
    auto blit = [&](const Image& im, int ox, int oy) {
        for (int y = 0; y < im.h; ++y) std::copy_n(&im.px[(size_t)y * im.w], im.w, &out.px[(size_t)(oy + y) * out.w + ox]);
    };
    blit(first, 0, 0);
    blit(second, horizontal ? first.w : 0, horizontal ? 0 : first.h);
    return out;
}

// ---------------------------------------------------------------- curves

std::vector<D2D1_POINT_2F> CurveBeziers(const std::vector<D2D1_POINT_2F>& p) {
    std::vector<D2D1_POINT_2F> out;
    size_t n = p.size();
    if (n < 2) return p;
    const float t = 0.5f * 0.3f;  // GDI+: tension * TENSION_CONST
    out.push_back(p[0]);
    out.push_back({p[0].x + t * (p[1].x - p[0].x), p[0].y + t * (p[1].y - p[0].y)});
    for (size_t i = 1; i + 1 < n; ++i) {
        float dx = p[i + 1].x - p[i - 1].x, dy = p[i + 1].y - p[i - 1].y;
        out.push_back({p[i].x - t * dx, p[i].y - t * dy});
        out.push_back(p[i]);
        out.push_back({p[i].x + t * dx, p[i].y + t * dy});
    }
    out.push_back({p[n - 1].x + t * (p[n - 2].x - p[n - 1].x), p[n - 1].y + t * (p[n - 2].y - p[n - 1].y)});
    out.push_back(p[n - 1]);
    return out;
}

// ---------------------------------------------------------------- fonts

std::vector<std::wstring> FontChoices(const std::wstring& current) {
    static std::vector<std::wstring> installed;
    if (installed.empty()) {
        ComPtr<IDWriteFactory> f;
        ComPtr<IDWriteFontCollection> fc;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)f.GetAddressOf());
        if (f) f->GetSystemFontCollection(&fc);
        for (const wchar_t* n : {L"Arial", L"Calibri", L"Cambria", L"Comic Sans MS", L"Consolas", L"Courier New", L"Georgia", L"Impact",
                                 L"Segoe UI", L"Tahoma", L"Times New Roman", L"Trebuchet MS", L"Verdana"}) {
            UINT32 idx;
            BOOL exists = FALSE;
            if (!fc || (SUCCEEDED(fc->FindFamilyName(n, &idx, &exists)) && exists)) installed.push_back(n);
        }
    }
    std::vector<std::wstring> v = installed;
    if (!current.empty() && std::find(v.begin(), v.end(), current) == v.end()) v.insert(v.begin(), current);
    return v;
}

}  // namespace shapes_ext
