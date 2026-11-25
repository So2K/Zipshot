#include "common.h"
#include "shapes_ext.h"
#include "capture_ext.h"
#include "imagedialogs.h"
#include "imageops.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <numeric>

// Image editor laid out like ShareX's: a centred icon toolbar on top with the same buttons,
// tool order, shortcuts and annotation defaults. Rendering is Direct2D; effects (blur, pixelate,
// highlight) are computed once per parameter on the CPU and then just clipped into place.

using Microsoft::WRL::ComPtr;
namespace sx = shapes_ext;

namespace {

// ---------- tools (ShareX order and shortcuts) ----------

enum Tool {
    T_RegionRect, T_RegionEllipse, T_RegionFreehand, T_Select, T_Rect, T_Ellipse, T_Freehand, T_FreehandArrow, T_Line, T_Arrow, T_TextOutline, T_TextBackground,
    T_Step, T_Magnify, T_SmartEraser, T_Blur, T_Pixelate, T_Highlight, T_Crop,
    // Added later; appended so the tool numbers saved in zipshot.ini keep their meaning. kToolOrder has ShareX's order.
    T_SpeechBalloon, T_Image, T_ImageScreen, T_Sticker, T_Cursor, T_CutOut, T_Count
};
struct ToolInfo { const wchar_t* ru; const wchar_t* en; wchar_t key; int icon; };
const ToolInfo kTools[T_Count] = {
    {L"Прямоугольная область", L"Rectangle region", 0, 126},
    {L"Эллиптическая область", L"Ellipse region", 0, 127},
    {L"Произвольная область", L"Freehand region", 0, 128},
    {L"Выделение и перемещение (M)", L"Select and move (M)", 'M', 105},
    {L"Прямоугольник (R)", L"Rectangle (R)", 'R', 106},
    {L"Эллипс (E)", L"Ellipse (E)", 'E', 107},
    {L"Кисть (F)", L"Freehand (F)", 'F', 108},
    {L"Кисть со стрелкой", L"Freehand arrow", 0, 109},
    {L"Линия (L)", L"Line (L)", 'L', 110},
    {L"Стрелка (A)", L"Arrow (A)", 'A', 111},
    {L"Текст с обводкой (O)", L"Text (Outline) (O)", 'O', 112},
    {L"Текст на фоне (T)", L"Text (Background) (T)", 'T', 113},
    {L"Шаг (I)", L"Step (I)", 'I', 114},
    {L"Лупа", L"Magnify", 0, 115},
    {L"Умный ластик", L"Smart eraser", 0, 116},
    {L"Размытие (B)", L"Blur (B)", 'B', 117},
    {L"Пикселизация (P)", L"Pixelate (P)", 'P', 118},
    {L"Выделение маркером (H)", L"Highlight (H)", 'H', 119},
    {L"Обрезка (C)", L"Crop image (C)", 'C', 120},
    {L"Сноска (S)", L"Speech balloon (S)", 'S', 140},
    {L"Картинка из файла", L"Image (File)", 0, 141},
    {L"Картинка с экрана", L"Image (Screen)", 0, 142},
    {L"Стикер", L"Sticker", 0, 143},
    {L"Курсор", L"Cursor", 0, 144},
    {L"Разрыв (X)", L"Cut out (X)", 'X', 145},
};
// ShareX ShapeType order (toolbar).
constexpr Tool kToolOrder[] = {T_RegionRect, T_RegionEllipse, T_RegionFreehand, T_Select, T_Rect, T_Ellipse, T_Freehand,
                               T_FreehandArrow, T_Line, T_Arrow, T_TextOutline, T_TextBackground, T_SpeechBalloon, T_Step,
                               T_Magnify, T_Image, T_ImageScreen, T_Sticker, T_Cursor, T_SmartEraser, T_Blur, T_Pixelate,
                               T_Highlight, T_Crop, T_CutOut};
static_assert(std::size(kToolOrder) == T_Count);
constexpr int kNumpadTools[] = {-1, T_Rect, T_Ellipse, T_Freehand, T_Line, T_Arrow, T_TextOutline, T_Step, T_Blur, T_Pixelate};

D2D1_COLOR_F Rgb(int r, int g, int b, int a = 255) { return {r / 255.f, g / 255.f, b / 255.f, a / 255.f}; }
const D2D1_COLOR_F kRed = Rgb(242, 60, 60), kWhite = Rgb(255, 255, 255), kClear = Rgb(0, 0, 0, 0), kYellow = Rgb(255, 255, 0);
const D2D1_COLOR_F kShadow = Rgb(0, 0, 0, 125);

bool IsRegion(Tool t) { return t == T_RegionRect || t == T_RegionEllipse || t == T_RegionFreehand; }

// ShareX TextDrawingOptions (font, style, alignment; colour and size live in Opts/Shape as before).
struct TextStyle {
    std::wstring font = L"Arial";
    int bold = 0, italic = 0, underline = 0;
    int alignH = 1, alignV = 1;  // StringAlignment: 0 Near, 1 Center, 2 Far
    int enterNewLine = 0;        // EnterKeyNewLine: Enter adds a line, Ctrl+Enter finishes
};
bool g_lightNodes = false;  // [region] UseLightResizeNodes, see RegionLightNodes()

// Annotation options. Defaults are the user's own ShareX values (ApplicationConfig.json); changes persist
// in zipshot.ini [annotation], like ShareX remembers them. Font sizes are points, as in ShareX.
struct Opts {
    D2D1_COLOR_F border = Rgb(0, 255, 93), fill = kClear;
    float borderSize = 3;
    D2D1_COLOR_F outlineColor = Rgb(0, 0, 0), outlineText = kWhite;
    float outlineFont = 8, outlineSize = 5;
    D2D1_COLOR_F textColor = kWhite, textFill = kRed;
    float textFont = 18;
    D2D1_COLOR_F stepText = kWhite, stepFill = kRed;
    float stepFont = 18;
    float magnify = 200, blur = 35, pixel = 15;
    D2D1_COLOR_F highlight = kYellow;
    // Region capture surface (RegionCaptureOptions).
    int lastRegionTool = T_RegionRect, lastAnnotationTool = T_Freehand, lastEditorTool = T_Select;
    bool showInfo = true, showMagnifier = true, squareMagnifier = false, dimming = true, detectWindows = true, detectControls = false;
    int magnifierCount = 33, magnifierSize = 10, dimStrength = 10;
    // ShareX AnnotationOptions added with the shape/tool options.
    int regionRadius = 0, borderStyle = 0, cornerRadius = 3, shadow = 1, centerPoints = 1, arrowDir = 0;
    D2D1_COLOR_F shadowColor = Rgb(0, 0, 0, 125);
    TextStyle outlineStyle, textStyle;
    int stepType = 0, startStep = 1, stickerSize = 64, cursorType = 3, cutType = 0, cutSize = 10;
    D2D1_COLOR_F cutBg = kClear;
    std::wstring lastImage, lastSticker;
    bool quickCrop = true, crosshair = false, animations = true, fixedSize = false, showFps = false, menuLocked = false,
         rememberMenu = false, switchToDrawing = false, switchToSelect = false, lightNodes = false;
    int fixedW = 250, fixedH = 250;
    float menuDX = 0, menuDY = 0;  // toolbar offset from its default spot (RememberMenuState)
    std::wstring actionX1 = L"CaptureFullscreen", actionX2 = L"CaptureActiveMonitor";

    static std::wstring Ini() { return ExeDir() + L"\\zipshot.ini"; }
    static std::wstring Hex(D2D1_COLOR_F c) {
        wchar_t b[16];
        swprintf_s(b, L"%02X%02X%02X%02X", (int)(c.a * 255 + .5f), (int)(c.r * 255 + .5f), (int)(c.g * 255 + .5f), (int)(c.b * 255 + .5f));
        return b;
    }
    template <class F>
    void Visit(F&& f) {
        f(L"BorderColor", &border, nullptr, nullptr); f(L"FillColor", &fill, nullptr, nullptr);
        f(L"BorderSize", nullptr, &borderSize, nullptr);
        f(L"TextOutlineBorderColor", &outlineColor, nullptr, nullptr); f(L"TextOutlineColor", &outlineText, nullptr, nullptr);
        f(L"TextOutlineSize", nullptr, &outlineFont, nullptr); f(L"TextOutlineBorderSize", nullptr, &outlineSize, nullptr);
        f(L"TextColor", &textColor, nullptr, nullptr); f(L"TextFillColor", &textFill, nullptr, nullptr);
        f(L"TextSize", nullptr, &textFont, nullptr);
        f(L"StepColor", &stepText, nullptr, nullptr); f(L"StepFillColor", &stepFill, nullptr, nullptr);
        f(L"StepFontSize", nullptr, &stepFont, nullptr);
        f(L"MagnifyStrength", nullptr, &magnify, nullptr); f(L"BlurRadius", nullptr, &blur, nullptr);
        f(L"PixelateSize", nullptr, &pixel, nullptr); f(L"HighlightColor", &highlight, nullptr, nullptr);
        f(L"LastRegionTool", nullptr, nullptr, &lastRegionTool); f(L"LastAnnotationTool", nullptr, nullptr, &lastAnnotationTool);
        f(L"LastEditorTool", nullptr, nullptr, &lastEditorTool);
        f(L"RegionCornerRadius", nullptr, nullptr, &regionRadius); f(L"BorderStyle", nullptr, nullptr, &borderStyle);
        f(L"DrawingCornerRadius", nullptr, nullptr, &cornerRadius); f(L"Shadow", nullptr, nullptr, &shadow);
        f(L"ShadowColor", &shadowColor, nullptr, nullptr); f(L"LineCenterPointCount", nullptr, nullptr, &centerPoints);
        f(L"ArrowHeadDirection", nullptr, nullptr, &arrowDir);
        auto style = [&](const wchar_t* pre, TextStyle& t) {
            std::wstring p = pre;
            f((p + L"Bold").c_str(), nullptr, nullptr, &t.bold); f((p + L"Italic").c_str(), nullptr, nullptr, &t.italic);
            f((p + L"Underline").c_str(), nullptr, nullptr, &t.underline);
            f((p + L"AlignmentHorizontal").c_str(), nullptr, nullptr, &t.alignH);
            f((p + L"AlignmentVertical").c_str(), nullptr, nullptr, &t.alignV);
            f((p + L"EnterKeyNewLine").c_str(), nullptr, nullptr, &t.enterNewLine);
        };
        style(L"TextOutline", outlineStyle); style(L"Text", textStyle);
        f(L"StepType", nullptr, nullptr, &stepType); f(L"StartingStepValue", nullptr, nullptr, &startStep);
        f(L"StickerSize", nullptr, nullptr, &stickerSize); f(L"CursorType", nullptr, nullptr, &cursorType);
        f(L"CutOutEffectType", nullptr, nullptr, &cutType); f(L"CutOutEffectSize", nullptr, nullptr, &cutSize);
        f(L"CutOutBackgroundColor", &cutBg, nullptr, nullptr);
    }
    template <class F>
    void VisitStrings(F&& f) {
        f(L"TextOutlineFont", outlineStyle.font); f(L"TextFont", textStyle.font);
        f(L"LastImageFilePath", lastImage); f(L"LastStickerPath", lastSticker);
    }
    void Load() {
        std::wstring ini = Ini();
        Visit([&](const wchar_t* k, D2D1_COLOR_F* c, float* f, int* i) {
            wchar_t buf[64];
            GetPrivateProfileStringW(L"annotation", k, L"", buf, 64, ini.c_str());
            if (!*buf) return;
            if (c) {
                unsigned v = wcstoul(buf, nullptr, 16);
                *c = Rgb(v >> 16 & 255, v >> 8 & 255, v & 255, v >> 24 & 255);
            } else if (f) {
                *f = (float)_wtof(buf);
            } else {
                *i = _wtoi(buf);
            }
        });
        VisitStrings([&](const wchar_t* k, std::wstring& v) {
            wchar_t buf[MAX_PATH];
            GetPrivateProfileStringW(L"annotation", k, L"", buf, MAX_PATH, ini.c_str());
            if (*buf) v = buf;
        });
        centerPoints = std::clamp(centerPoints, 0, 5);  // LineDrawingShape.MaximumCenterPointCount
        auto flag = [&](const wchar_t* k, bool& v) { v = GetPrivateProfileIntW(L"region", k, v, ini.c_str()) != 0; };
        flag(L"ShowInfo", showInfo); flag(L"ShowMagnifier", showMagnifier); flag(L"UseSquareMagnifier", squareMagnifier);
        flag(L"UseDimming", dimming); flag(L"DetectWindows", detectWindows); flag(L"DetectControls", detectControls);
        magnifierCount = GetPrivateProfileIntW(L"region", L"MagnifierPixelCount", magnifierCount, ini.c_str()) | 1;
        magnifierSize = GetPrivateProfileIntW(L"region", L"MagnifierPixelSize", magnifierSize, ini.c_str());
        dimStrength = GetPrivateProfileIntW(L"region", L"BackgroundDimStrength", dimStrength, ini.c_str());
        LoadRegionExtras(ini);
    }
    // ShareX RegionCaptureOptions beyond the magnifier/info basics ([region] keys, same names as ShareX).
    void LoadRegionExtras(const std::wstring& ini) {
        auto flag = [&](const wchar_t* k, bool& v) { v = GetPrivateProfileIntW(L"region", k, v, ini.c_str()) != 0; };
        flag(L"QuickCrop", quickCrop); flag(L"ShowCrosshair", crosshair); flag(L"EnableAnimations", animations);
        flag(L"IsFixedSize", fixedSize); flag(L"ShowFPS", showFps); flag(L"MenuLocked", menuLocked);
        flag(L"RememberMenuState", rememberMenu); flag(L"SwitchToDrawingToolAfterSelection", switchToDrawing);
        flag(L"SwitchToSelectionToolAfterDrawing", switchToSelect); flag(L"UseLightResizeNodes", lightNodes);
        wchar_t buf[128];
        GetPrivateProfileStringW(L"region", L"FixedSize", L"", buf, 128, ini.c_str());
        int a, b;
        if (swscanf_s(buf, L"%d , %d", &a, &b) == 2) fixedW = std::clamp(a, 10, 10000), fixedH = std::clamp(b, 10, 10000);
        GetPrivateProfileStringW(L"region", L"MenuOffset", L"", buf, 128, ini.c_str());
        if (swscanf_s(buf, L"%d , %d", &a, &b) == 2) menuDX = (float)a, menuDY = (float)b;
        GetPrivateProfileStringW(L"region", L"RegionCaptureActionX1Click", actionX1.c_str(), buf, 128, ini.c_str());
        actionX1 = buf;
        GetPrivateProfileStringW(L"region", L"RegionCaptureActionX2Click", actionX2.c_str(), buf, 128, ini.c_str());
        actionX2 = buf;
        g_lightNodes = lightNodes;
    }
    void SaveRegionExtras(const std::wstring& ini) {
        auto flag = [&](const wchar_t* k, bool v) { WritePrivateProfileStringW(L"region", k, v ? L"1" : L"0", ini.c_str()); };
        flag(L"QuickCrop", quickCrop); flag(L"ShowCrosshair", crosshair); flag(L"EnableAnimations", animations);
        flag(L"IsFixedSize", fixedSize); flag(L"ShowFPS", showFps); flag(L"MenuLocked", menuLocked);
        flag(L"RememberMenuState", rememberMenu); flag(L"SwitchToDrawingToolAfterSelection", switchToDrawing);
        flag(L"SwitchToSelectionToolAfterDrawing", switchToSelect); flag(L"UseLightResizeNodes", lightNodes);
        WritePrivateProfileStringW(L"region", L"FixedSize", (std::to_wstring(fixedW) + L"," + std::to_wstring(fixedH)).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"region", L"MenuOffset",
                                   (std::to_wstring((int)menuDX) + L"," + std::to_wstring((int)menuDY)).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"region", L"RegionCaptureActionX1Click", actionX1.c_str(), ini.c_str());
        WritePrivateProfileStringW(L"region", L"RegionCaptureActionX2Click", actionX2.c_str(), ini.c_str());
        g_lightNodes = lightNodes;
    }
    void Save() {
        std::wstring ini = Ini();
        Visit([&](const wchar_t* k, D2D1_COLOR_F* c, float* f, int* i) {
            std::wstring v = c ? Hex(*c) : f ? std::to_wstring((int)*f) : std::to_wstring(*i);
            WritePrivateProfileStringW(L"annotation", k, v.c_str(), ini.c_str());
        });
        VisitStrings([&](const wchar_t* k, std::wstring& v) {
            if (!v.empty()) WritePrivateProfileStringW(L"annotation", k, v.c_str(), ini.c_str());
        });
        auto flag = [&](const wchar_t* k, bool v) { WritePrivateProfileStringW(L"region", k, v ? L"1" : L"0", ini.c_str()); };
        flag(L"ShowInfo", showInfo); flag(L"ShowMagnifier", showMagnifier); flag(L"UseSquareMagnifier", squareMagnifier);
        flag(L"UseDimming", dimming); flag(L"DetectWindows", detectWindows); flag(L"DetectControls", detectControls);
        WritePrivateProfileStringW(L"region", L"MagnifierPixelCount", std::to_wstring(magnifierCount).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"region", L"MagnifierPixelSize", std::to_wstring(magnifierSize).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"region", L"BackgroundDimStrength", std::to_wstring(dimStrength).c_str(), ini.c_str());
        SaveRegionExtras(ini);
    }
};

ID2D1Factory* D2D() {
    static ComPtr<ID2D1Factory> f;
    if (!f) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf());
    return f.Get();
}
IDWriteFactory* DW() {
    static ComPtr<IDWriteFactory> f;
    if (!f) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)f.GetAddressOf());
    return f.Get();
}
ID2D1StrokeStyle* RoundStroke() {
    static ComPtr<ID2D1StrokeStyle> s;
    if (!s)
        D2D()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                             D2D1_LINE_JOIN_ROUND),
                                 nullptr, 0, &s);
    return s.Get();
}
ID2D1StrokeStyle* DashStroke() {
    static ComPtr<ID2D1StrokeStyle> s;
    if (!s)
        D2D()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                             D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DASH),
                                 nullptr, 0, &s);
    return s.Get();
}

// ShareX BorderStyle (GDI+ DashStyle: dashes in pen widths, flat dash caps). `round` = round line ends and joins.
ID2D1StrokeStyle* BorderStroke(int style, bool round) {
    static ComPtr<ID2D1StrokeStyle> cache[5][2];
    style = std::clamp(style, 0, 4);
    if (style == 0) return round ? RoundStroke() : nullptr;
    auto& s = cache[style][round];
    if (!s) {
        static const float dash[] = {3, 1}, dot[] = {1, 1}, dashDot[] = {3, 1, 1, 1}, dashDotDot[] = {3, 1, 1, 1, 1, 1};
        const float* d[] = {nullptr, dash, dot, dashDot, dashDotDot};
        const UINT n[] = {0, 2, 2, 4, 6};
        D2D1_CAP_STYLE cap = round ? D2D1_CAP_STYLE_ROUND : D2D1_CAP_STYLE_FLAT;
        D2D()->CreateStrokeStyle(D2D1::StrokeStyleProperties(cap, cap, D2D1_CAP_STYLE_FLAT,
                                                             round ? D2D1_LINE_JOIN_ROUND : D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM),
                                 d[style], n[style], &s);
    }
    return s.Get();
}

ComPtr<IDWriteTextFormat> Font(const wchar_t* family, float size, bool bold, bool italic = false) {
    ComPtr<IDWriteTextFormat> f;
    DW()->CreateTextFormat(family, nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                           italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ru-ru", &f);
    return f;
}

// Draws text as geometry so it can get a real outline (ShareX "text outline" tool).
class OutlineRenderer : public IDWriteTextRenderer {
public:
    ID2D1RenderTarget* rt = nullptr;
    ID2D1Brush* fill = nullptr;
    ID2D1Brush* stroke = nullptr;
    float strokeW = 0;
    STDMETHOD(QueryInterface)(REFIID riid, void** p) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextRenderer) || riid == __uuidof(IDWritePixelSnapping)) {
            *p = this;
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(IsPixelSnappingDisabled)(void*, BOOL* d) override { *d = TRUE; return S_OK; }
    STDMETHOD(GetCurrentTransform)(void*, DWRITE_MATRIX* m) override {
        rt->GetTransform((D2D1_MATRIX_3X2_F*)m);
        return S_OK;
    }
    STDMETHOD(GetPixelsPerDip)(void*, FLOAT* p) override { *p = 1; return S_OK; }
    STDMETHOD(DrawGlyphRun)(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run,
                            const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        ComPtr<ID2D1PathGeometry> g;
        D2D()->CreatePathGeometry(&g);
        ComPtr<ID2D1GeometrySink> s;
        g->Open(&s);
        run->fontFace->GetGlyphRunOutline(run->fontEmSize, run->glyphIndices, run->glyphAdvances, run->glyphOffsets,
                                          run->glyphCount, run->isSideways, run->bidiLevel % 2, s.Get());
        s->Close();
        ComPtr<ID2D1TransformedGeometry> tg;
        D2D()->CreateTransformedGeometry(g.Get(), D2D1::Matrix3x2F::Translation(x, y), &tg);
        if (stroke && strokeW > 0) rt->DrawGeometry(tg.Get(), stroke, strokeW, RoundStroke());
        if (fill) rt->FillGeometry(tg.Get(), fill);
        return S_OK;
    }
    STDMETHOD(DrawUnderline)(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* u, IUnknown*) override {
        D2D1_RECT_F r{x, y + u->offset, x + u->width, y + u->offset + u->thickness};
        if (stroke && strokeW > 0) rt->DrawRectangle(r, stroke, strokeW, RoundStroke());
        if (fill) rt->FillRectangle(r, fill);
        return S_OK;
    }
    STDMETHOD(DrawStrikethrough)(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    STDMETHOD(DrawInlineObject)(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }
};

// ---------- shapes ----------

struct Shape {
    Tool tool;
    D2D1_COLOR_F color{}, color2{};  // border/text, fill/background
    float size = 0, size2 = 0;       // width or font size, outline width or effect strength
    std::vector<D2D1_POINT_2F> pts;
    std::wstring text;
    int step = 0;
    ComPtr<ID2D1PathGeometry> geo;  // freehand cache
    // ShareX per-shape options, copied from Opts when the shape is created.
    bool shadow = true;
    D2D1_COLOR_F shadowColor = Rgb(0, 0, 0, 125);
    int borderStyle = 0, arrowDir = 0, stepType = 0;
    float radius = 0;                 // rectangle / text background corner radius
    bool centerActive = false;        // line/arrow: a centre node was dragged, so the line is a curve
    TextStyle ts;                     // text tools
    bool autoSize = true;             // text box follows the text until resized with a node
    D2D1_POINT_2F tail{};             // speech balloon / step tail point
    bool tailActive = false;          // step: tail shown once dragged (speech balloon: always)
    D2D1_SIZE_F initSize{};           // ShareX InitialSize, for Shift-proportional node resizing
    std::shared_ptr<Image> bmp;       // image shapes (premultiplied); an animation's first frame
    std::shared_ptr<const sx::Anim> anim;  // animated sticker/image (GIF): frames at the shape's size
    std::wstring src;                 // the file an image/sticker shape came from (Add to my stickers)
};

enum Fx { FX_Pixel = 1, FX_Blur, FX_Highlight };

struct Gfx {
    ComPtr<ID2D1Bitmap> img;
    ComPtr<ID2D1SolidColorBrush> br, br2;
    std::map<uint64_t, ComPtr<ID2D1Bitmap>> fx;
    std::map<int, ComPtr<ID2D1Bitmap>> icons;
    // Image shapes' GPU copies. Weak keys never collide with a later image (the control block stays alive).
    std::map<std::weak_ptr<Image>, ComPtr<ID2D1Bitmap>, std::owner_less<std::weak_ptr<Image>>> shapeImgs;
    ComPtr<ID2D1BitmapBrush> checker;  // under transparent documents
};

D2D1_RECT_F Norm(D2D1_POINT_2F a, D2D1_POINT_2F b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y)};
}
bool IsTwoPoint(Tool t) {
    return t == T_RegionRect || t == T_RegionEllipse || t == T_Rect || t == T_Ellipse || t == T_Line || t == T_Arrow || t == T_Magnify || t == T_SmartEraser ||
           t == T_Blur || t == T_Pixelate || t == T_Highlight || t == T_Crop || t == T_ImageScreen;
}
bool IsFreehand(Tool t) { return t == T_Freehand || t == T_FreehandArrow; }
bool IsText(Tool t) { return t == T_TextOutline || t == T_TextBackground || t == T_SpeechBalloon; }
bool IsLine(Tool t) { return t == T_Line || t == T_Arrow; }
bool IsImageShape(Tool t) { return t == T_Image || t == T_ImageScreen || t == T_Sticker || t == T_Cursor; }
// Shapes ShareX resizes with the 8 box nodes (BaseShape.OnNodePositionUpdate).
bool HasBoxNodes(Tool t) {
    return t == T_Rect || t == T_Ellipse || IsText(t) || t == T_Magnify || t == T_SmartEraser || t == T_Blur || t == T_Pixelate ||
           t == T_Highlight || t == T_Image || t == T_ImageScreen;
}

void BoxBlurPass(std::vector<uint32_t>& src, std::vector<uint32_t>& dst, int w, int h, int r, bool horiz) {
    // Preserve the original arithmetic for small images and unusually large radii.
    // Larger images benefit from exact division lookup and sequential row access.
    if (w > 0 && h > 0 && r >= 0 && r <= 256 && (uint64_t)w * h >= 4096 &&
        src.size() >= (size_t)w * h && dst.size() >= (size_t)w * h) {
        static thread_local int cachedRadius = -1;
        static thread_local std::vector<uint8_t> quotient;
        const int div = 2 * r + 1;
        if (cachedRadius != r) {
            quotient.resize((size_t)255 * div + 1);
            for (size_t i = 0; i < quotient.size(); ++i) quotient[i] = (uint8_t)(i / div);
            cachedRadius = r;
        }
        const uint8_t* average = quotient.data();  // resolve the per-thread table once, outside pixel loops
        if (horiz) {
            for (int y = 0; y < h; ++y) {
                const uint32_t* row = src.data() + (size_t)y * w;
                uint32_t* out = dst.data() + (size_t)y * w;
                int sr = 0, sg = 0, sb = 0;
                for (int x = -r; x <= r; ++x) {
                    const uint32_t p = row[std::clamp(x, 0, w - 1)];
                    sr += p >> 16 & 255; sg += p >> 8 & 255; sb += p & 255;
                }
                for (int x = 0; x < w; ++x) {
                    out[x] = 0xFF000000u | (uint32_t)average[sr] << 16 | (uint32_t)average[sg] << 8 | average[sb];
                    const uint32_t a = row[std::min(x + r + 1, w - 1)], b = row[std::max(x - r, 0)];
                    sr += (int)(a >> 16 & 255) - (int)(b >> 16 & 255);
                    sg += (int)(a >> 8 & 255) - (int)(b >> 8 & 255);
                    sb += (int)(a & 255) - (int)(b & 255);
                }
            }
        } else {
            // Each column keeps the same rolling sum as before, but the loop walks
            // adjacent pixels instead of striding through the full image per column.
            std::vector<int> sr(w), sg(w), sb(w);
            for (int y = -r; y <= r; ++y) {
                const uint32_t* row = src.data() + (size_t)std::clamp(y, 0, h - 1) * w;
                for (int x = 0; x < w; ++x) {
                    const uint32_t p = row[x];
                    sr[x] += p >> 16 & 255; sg[x] += p >> 8 & 255; sb[x] += p & 255;
                }
            }
            for (int y = 0; y < h; ++y) {
                uint32_t* out = dst.data() + (size_t)y * w;
                const uint32_t* add = src.data() + (size_t)std::min(y + r + 1, h - 1) * w;
                const uint32_t* remove = src.data() + (size_t)std::max(y - r, 0) * w;
                for (int x = 0; x < w; ++x) {
                    out[x] = 0xFF000000u | (uint32_t)average[sr[x]] << 16 | (uint32_t)average[sg[x]] << 8 | average[sb[x]];
                    const uint32_t a = add[x], b = remove[x];
                    sr[x] += (int)(a >> 16 & 255) - (int)(b >> 16 & 255);
                    sg[x] += (int)(a >> 8 & 255) - (int)(b >> 8 & 255);
                    sb[x] += (int)(a & 255) - (int)(b & 255);
                }
            }
        }
        return;
    }
    int n = horiz ? w : h, lines = horiz ? h : w;
    size_t step = horiz ? 1 : w;
    for (int l = 0; l < lines; ++l) {
        size_t base = horiz ? (size_t)l * w : l;
        int sr = 0, sg = 0, sb = 0;
        auto at = [&](int i) { return src[base + step * std::clamp(i, 0, n - 1)]; };
        for (int i = -r; i <= r; ++i) {
            uint32_t p = at(i);
            sr += p >> 16 & 255; sg += p >> 8 & 255; sb += p & 255;
        }
        int div = 2 * r + 1;
        for (int i = 0; i < n; ++i) {
            dst[base + step * i] = 0xFF000000u | (uint32_t)(sr / div) << 16 | (uint32_t)(sg / div) << 8 | (uint32_t)(sb / div);
            uint32_t a = at(i + r + 1), b = at(i - r);
            sr += (int)(a >> 16 & 255) - (int)(b >> 16 & 255);
            sg += (int)(a >> 8 & 255) - (int)(b >> 8 & 255);
            sb += (int)(a & 255) - (int)(b & 255);
        }
    }
}

// Pressed-in icons from ShareX (RCDATA PNGs), decoded once to premultiplied BGRA.
struct IconData { int w = 0, h = 0; std::vector<uint32_t> px; };
// The PNGs are 64px renders of the SVG set in icons/; scale them once with Fant to the exact pixel size.
const IconData& Icon(int id, int px) {
    static std::map<int, IconData> cache;
    int key = id * 1000 + px;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    IconData& d = cache[key];
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return d;
    void* data = LockResource(LoadResource(g_inst, r));
    DWORD size = SizeofResource(g_inst, r);
    ComPtr<IWICImagingFactory> f;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    ComPtr<IWICStream> s;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<IWICBitmapScaler> scaler;
    if (!f || FAILED(f->CreateStream(&s)) || FAILED(s->InitializeFromMemory((BYTE*)data, size)) ||
        FAILED(f->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), px, px, WICBitmapInterpolationModeFant)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return d;
    UINT w, h;
    conv->GetSize(&w, &h);
    d.w = w;
    d.h = h;
    d.px.resize((size_t)w * h);
    conv->CopyPixels(nullptr, w * 4, (UINT)d.px.size() * 4, (BYTE*)d.px.data());
    return d;
}

// ---------- toolbar ----------

enum BtnKind { B_Apply, B_Save, B_SaveAs, B_Copy, B_Close, B_Tool, B_Border, B_Fill, B_Highlight, B_Options, B_Edit, B_Sep,
               B_Grip, B_Capture, B_RegionOptions, B_Image };
struct Button {
    BtnKind kind;
    int idx;
    D2D1_RECT_F r;
};

// Theme colours from ShareX's dark theme.
const D2D1_COLOR_F kBarBg = Rgb(39, 39, 39), kBarBorder = Rgb(28, 28, 28), kHover = Rgb(62, 62, 66),
                   kChecked = Rgb(46, 66, 92), kAccent = Rgb(0, 138, 255), kCanvas = Rgb(33, 33, 33), kText = Rgb(231, 233, 234);

struct RegionResult {
    bool done = false, ok = false;
    Image image;
    RECT rect{};
};

struct Editor {
    HWND hwnd = nullptr;
    // Region capture mode (ShareX RegionCaptureForm): full-screen surface over the frozen desktop.
    bool region = false;
    RegionResult* result = nullptr;
    RECT vs{};                      // virtual screen; client (0,0) = vs.left/top
    RECT mon{};                     // active monitor in client coords; the toolbar is centred on it
    std::vector<WindowRect> wins;   // screen coords, z-order
    bool hasHover = false;
    RECT hover{};                   // client coords
    bool creatingRegion = false;
    Shape regionShape{T_RegionRect};
    // Multi region mode (QuickCrop off): finished regions stay editable until Enter / double click.
    std::vector<Shape> regions;
    int selRegion = -1;
    int regionDrag = 0;             // RD_* bits while moving/resizing a finished region
    bool fixedClick = false;        // fixed size mode: button down, capture the hover rect on release
    DWORD fpsTick = 0;
    int fpsFrames = 0, fps = 0;
    bool useWarp = false;
    int rendererMode = 0;  // 0 auto (GPU -> WARP on GPU stall), 1 force GPU, 2 force CPU/WARP
    int slowGpuFrames = 0;
    ULONGLONG lastPaintMs = 0;
    ComPtr<IDWriteTextFormat> fpsFont;  // ShareX infoFontBig
    POINT cursor{-10000, -10000};   // client coords
    float gripDX = 0, gripDY = 0;   // toolbar dragged by its grip
    bool gripping = false;
    POINT gripStart{};
    DWORD startTick = GetTickCount();
    ImagePtr img;
    std::wstring path;
    ComPtr<ID2D1HwndRenderTarget> rt;
    Gfx g;
    Opts o;
    std::vector<Shape> shapes;
    // Undo history: the shapes plus the bitmap they sit on. Entries share the ImagePtr, so only the
    // Image menu operations, which really replace the bitmap, add bitmaps to the history.
    struct HistoryEntry {
        std::vector<Shape> shapes;
        ImagePtr image;
    };
    std::vector<HistoryEntry> undo, redo;
    std::map<uint64_t, Image> fxCache;
    Tool tool = T_Rect;
    int selected = -1;
    int editingText = -1;
    bool drawing = false, moving = false, cropping = false, panning = false;
    D2D1_POINT_2F dragStart{}, cropA{}, cropB{};
    POINT panStart{};
    float panOX = 0, panOY = 0;
    float scale = 1, offX = 0, offY = 0;
    bool userZoom = false;
    bool dirty = false;
    float ui = 1;
    float tbH = 32;
    std::vector<Button> buttons;
    int hoverBtn = -1;
    bool showTip = false;
    ComPtr<IDWriteTextFormat> tipFont;
    ComPtr<IDWriteTextFormat> infoFont;  // ShareX region info: Verdana 9pt

    // ---------- document ----------
    D2D1_RECT_F Crop() const {
        D2D1_RECT_F r{0, 0, (float)img->w, (float)img->h};
        for (const Shape& s : shapes)
            if (s.tool == T_Crop) r = Norm(s.pts[0], s.pts[1]);
        return r;
    }

    const Image& FxImage(int kind, uint32_t param) {
        uint64_t key = (uint64_t)kind << 32 | param;
        auto it = fxCache.find(key);
        if (it != fxCache.end()) return it->second;
        Image& out = fxCache[key];
        if (kind == FX_Pixel) {
            int b = std::max<int>(2, param);
            int pw = (img->w + b - 1) / b, ph = (img->h + b - 1) / b;
            out.w = pw;
            out.h = ph;
            out.px.resize((size_t)pw * ph);
            for (int by = 0; by < ph; ++by)
                for (int bx = 0; bx < pw; ++bx) {
                    uint32_t r = 0, gg = 0, bb = 0, n = 0;
                    for (int y = by * b; y < std::min(img->h, (by + 1) * b); ++y)
                        for (int x = bx * b; x < std::min(img->w, (bx + 1) * b); ++x) {
                            uint32_t p = img->px[(size_t)y * img->w + x];
                            r += p >> 16 & 255; gg += p >> 8 & 255; bb += p & 255; ++n;
                        }
                    out.px[(size_t)by * pw + bx] = 0xFF000000u | (r / n) << 16 | (gg / n) << 8 | (bb / n);
                }
        } else if (kind == FX_Blur) {
            out = *img;
            std::vector<uint32_t> tmp(out.px.size());
            int r = std::max<int>(1, param / 3);
            for (int i = 0; i < 3; ++i) {
                BoxBlurPass(out.px, tmp, out.w, out.h, r, true);
                BoxBlurPass(tmp, out.px, out.w, out.h, r, false);
            }
        } else {  // highlight: multiply by the colour, like ShareX
            out = *img;
            uint32_t hr = param >> 16 & 255, hg = param >> 8 & 255, hb = param & 255;
            for (auto& p : out.px)
                p = 0xFF000000u | ((p >> 16 & 255) * hr / 255) << 16 | ((p >> 8 & 255) * hg / 255) << 8 | ((p & 255) * hb / 255);
        }
        return out;
    }

    ID2D1Bitmap* FxBitmap(ID2D1RenderTarget* t, Gfx& gx, int kind, uint32_t param) {
        uint64_t key = (uint64_t)kind << 32 | param;
        auto it = gx.fx.find(key);
        if (it != gx.fx.end()) return it->second.Get();
        const Image& im = FxImage(kind, param);
        ComPtr<ID2D1Bitmap> b;
        t->CreateBitmap(D2D1::SizeU(im.w, im.h), im.px.data(), im.w * 4,
                        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &b);
        gx.fx[key] = b;
        return b.Get();
    }

    static uint32_t Packed(D2D1_COLOR_F c) {
        return (uint32_t)(c.r * 255 + .5f) << 16 | (uint32_t)(c.g * 255 + .5f) << 8 | (uint32_t)(c.b * 255 + .5f);
    }

    // Transparent documents (Image menu: transparent new image/canvas, shadow, rounded corners) upload premultiplied.
    static void UploadBitmap(ID2D1RenderTarget* t, Gfx& gx, const Image& im) {
        gx.img.Reset();
        if (!im.alpha) {
            t->CreateBitmap(D2D1::SizeU(im.w, im.h), im.px.data(), im.w * 4,
                            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &gx.img);
            return;
        }
        std::vector<uint32_t> pm(im.px.size());
        for (size_t i = 0; i < pm.size(); ++i) {
            uint32_t p = im.px[i], a = p >> 24;
            pm[i] = a == 255 ? p : a << 24 | ((p >> 16 & 255) * a / 255) << 16 | ((p >> 8 & 255) * a / 255) << 8 | (p & 255) * a / 255;
        }
        t->CreateBitmap(D2D1::SizeU(im.w, im.h), pm.data(), im.w * 4,
                        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &gx.img);
    }

    static Gfx MakeGfx(ID2D1RenderTarget* t, const Image& im) {
        Gfx out;
        UploadBitmap(t, out, im);
        t->CreateSolidColorBrush(kRed, &out.br);
        t->CreateSolidColorBrush(kRed, &out.br2);
        return out;
    }

    static void BuildGeo(Shape& s) {
        s.geo.Reset();
        if (s.pts.size() < 2) return;
        D2D()->CreatePathGeometry(&s.geo);
        ComPtr<ID2D1GeometrySink> sink;
        s.geo->Open(&sink);
        sink->BeginFigure(s.pts[0], D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLines(s.pts.data() + 1, (UINT32)s.pts.size() - 1);
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
    }

    static void ArrowHead(ID2D1RenderTarget* t, ID2D1Brush* br, D2D1_POINT_2F from, D2D1_POINT_2F tip, float width,
                          D2D1_POINT_2F* lineEnd, float chord = -1) {
        float dx = tip.x - from.x, dy = tip.y - from.y, len = std::sqrt(dx * dx + dy * dy);
        if (lineEnd) *lineEnd = tip;
        if (len < 1) return;
        float ux = dx / len, uy = dy / len;
        // A short arrow gets a smaller head, so the shaft never pokes out behind it while it is being dragged.
        // `chord`: the length to size by when `from` is only a direction (a curve's control point).
        float head = std::min(width * 3 + 10, (chord > 0 ? chord : len) * 0.6f), hw = head * 0.55f;
        D2D1_POINT_2F base{tip.x - ux * head, tip.y - uy * head};
        if (lineEnd) *lineEnd = {base.x + ux * 2, base.y + uy * 2};
        ComPtr<ID2D1PathGeometry> tri;
        D2D()->CreatePathGeometry(&tri);
        ComPtr<ID2D1GeometrySink> sink;
        tri->Open(&sink);
        sink->BeginFigure(tip, D2D1_FIGURE_BEGIN_FILLED);
        D2D1_POINT_2F p2[2] = {{base.x - uy * hw, base.y + ux * hw}, {base.x + uy * hw, base.y - ux * hw}};
        sink->AddLines(p2, 2);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        t->FillGeometry(tri.Get(), br);
    }

    // Text laid out in a w x h box; `aligned` applies ShareX's StringFormat alignment (and wraps like DrawString),
    // otherwise the text is measured on one line per paragraph.
    static ComPtr<IDWriteTextLayout> Layout(const Shape& s, float w = 100000, float h = 100000, bool aligned = false) {
        // Sizes are points, like ShareX's GDI+ fonts on a 96-DPI canvas.
        bool step = s.tool == T_Step;
        auto f = step ? Font(L"Arial", s.size * 4 / 3, true) : Font(s.ts.font.c_str(), s.size * 4 / 3, s.ts.bold != 0, s.ts.italic != 0);
        std::wstring txt = step ? sx::StepLabel(s.step, s.stepType) : (s.text.empty() ? L" " : s.text);
        if (aligned) {
            const DWRITE_TEXT_ALIGNMENT ha[] = {DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_TRAILING};
            const DWRITE_PARAGRAPH_ALIGNMENT va[] = {DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                                                     DWRITE_PARAGRAPH_ALIGNMENT_FAR};
            f->SetTextAlignment(ha[std::clamp(s.ts.alignH, 0, 2)]);
            f->SetParagraphAlignment(va[std::clamp(s.ts.alignV, 0, 2)]);
        } else {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        ComPtr<IDWriteTextLayout> lay;
        DW()->CreateTextLayout(txt.c_str(), (UINT32)txt.size(), f.Get(), std::max(w, 1.f), std::max(h, 1.f), &lay);
        if (lay && !step && s.ts.underline) lay->SetUnderline(TRUE, {0, (UINT32)txt.size()});
        return lay;
    }

    static D2D1_SIZE_F TextSize(const Shape& s) {
        DWRITE_TEXT_METRICS m{};
        Layout(s)->GetMetrics(&m);
        return {m.widthIncludingTrailingWhitespace, m.height};
    }

    static float TextPad(const Shape& s) { return s.tool == T_TextOutline ? 0 : s.size * 0.35f; }

    // Where the text goes inside a text shape's box.
    static D2D1_RECT_F TextInner(const Shape& s) {
        D2D1_RECT_F b = Norm(s.pts[0], s.pts.size() > 1 ? s.pts[1] : s.pts[0]);
        float p = TextPad(s);
        return {b.left + p, b.top + p / 2, std::max(b.left + p, b.right - p), std::max(b.top + p / 2, b.bottom - p / 2)};
    }

    static ComPtr<IDWriteTextLayout> TextLayoutIn(const Shape& s, D2D1_RECT_F inner) {
        return Layout(s, inner.right - inner.left + 0.5f, inner.bottom - inner.top + 0.5f, true);
    }

    // While a text shape is auto-sized its box follows the text from the top-left corner (pts[0]).
    static void UpdateTextBox(Shape& s) {
        if (!IsText(s.tool)) return;
        if (s.pts.size() < 2) s.pts.resize(2, s.pts[0]);
        if (!s.autoSize) return;
        D2D1_SIZE_F z = TextSize(s);
        float p = TextPad(s);
        s.pts[1] = {s.pts[0].x + std::ceil(z.width) + 2 * p, s.pts[0].y + std::ceil(z.height) + p};
    }

    // Moves every point of a shape, keeping caches in step.
    static void MoveShape(Shape& s, float dx, float dy) {
        for (auto& q : s.pts) q.x += dx, q.y += dy;
        s.tail.x += dx;
        s.tail.y += dy;
        if (IsFreehand(s.tool)) BuildGeo(s);
    }

    // ShareX LineDrawingShape.AutoPositionCenterPoints: centre points evenly spaced until one is dragged.
    static void AutoCenterPoints(Shape& s) {
        if (!IsLine(s.tool) || s.centerActive || s.pts.size() < 3) return;
        D2D1_POINT_2F a = s.pts.front(), b = s.pts.back();
        int n = (int)s.pts.size() - 2;
        for (int i = 1; i <= n; ++i) s.pts[i] = {a.x + (b.x - a.x) * i / (n + 1.f), a.y + (b.y - a.y) * i / (n + 1.f)};
    }

    // Speech balloon / step tail: a triangle from the centre of `r` to `tip` (ShareX CreateTailPath).
    static ComPtr<ID2D1PathGeometry> TailGeometry(D2D1_RECT_F r, D2D1_POINT_2F tip, float widthMul) {
        ComPtr<ID2D1PathGeometry> g;
        D2D1_POINT_2F c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        float w = r.right - r.left, h = r.bottom - r.top;
        float tw = std::min({widthMul * (w + h) / 2, w, h}), half = tw / 2;
        float dx = tip.x - c.x, dy = tip.y - c.y, len = std::sqrt(dx * dx + dy * dy);
        if (len < 1) return g;
        float nx = -dy / len * half, ny = dx / len * half;
        D2D()->CreatePathGeometry(&g);
        ComPtr<ID2D1GeometrySink> sink;
        g->Open(&sink);
        sink->BeginFigure({c.x + nx, c.y + ny}, D2D1_FIGURE_BEGIN_FILLED);
        D2D1_POINT_2F rest[2] = {tip, {c.x - nx, c.y - ny}};
        sink->AddLines(rest, 2);
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        return g;
    }

    static bool InRect(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x <= r.right && p.y >= r.top && p.y <= r.bottom; }

    D2D1_RECT_F Bounds(const Shape& s) const {
        auto withTail = [&](D2D1_RECT_F r) {
            return D2D1_RECT_F{std::min(r.left, s.tail.x), std::min(r.top, s.tail.y), std::max(r.right, s.tail.x), std::max(r.bottom, s.tail.y)};
        };
        switch (s.tool) {
        case T_TextOutline:
        case T_TextBackground:
        case T_Image:
        case T_ImageScreen:
        case T_Sticker:
        case T_Cursor:
            return Norm(s.pts[0], s.pts.size() > 1 ? s.pts[1] : s.pts[0]);
        case T_SpeechBalloon:
            return withTail(Norm(s.pts[0], s.pts.size() > 1 ? s.pts[1] : s.pts[0]));
        case T_Step: {
            float r = StepRadius(s);
            D2D1_RECT_F b{s.pts[0].x - r, s.pts[0].y - r, s.pts[0].x + r, s.pts[0].y + r};
            return s.tailActive ? withTail(b) : b;
        }
        default: {
            D2D1_RECT_F r{1e9f, 1e9f, -1e9f, -1e9f};
            for (auto p : s.pts) r = {std::min(r.left, p.x), std::min(r.top, p.y), std::max(r.right, p.x), std::max(r.bottom, p.y)};
            float pad = s.size / 2 + (s.tool == T_Arrow || s.tool == T_FreehandArrow ? s.size * 2 + 6 : 0);
            return {r.left - pad, r.top - pad, r.right + pad, r.bottom + pad};
        }
        }
    }

    static float StepRadius(const Shape& s) {
        D2D1_SIZE_F z = TextSize(s);
        return std::max(z.width, z.height) / 2 + s.size * 0.35f;
    }

    // Polyline, or GDI+ DrawCurve through the points when `curve` (line/arrow centre points).
    static ComPtr<ID2D1PathGeometry> PathOf(const std::vector<D2D1_POINT_2F>& b, bool beziers) {
        ComPtr<ID2D1PathGeometry> g;
        D2D()->CreatePathGeometry(&g);
        ComPtr<ID2D1GeometrySink> sink;
        g->Open(&sink);
        sink->BeginFigure(b[0], D2D1_FIGURE_BEGIN_HOLLOW);
        if (beziers) {
            for (size_t i = 1; i + 2 < b.size(); i += 3) sink->AddBezier({b[i], b[i + 1], b[i + 2]});
        } else if (b.size() > 1) {
            sink->AddLines(b.data() + 1, (UINT32)b.size() - 1);
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        return g;
    }

    // Line and arrow (ShareX LineDrawingShape / ArrowDrawingShape): straight until a centre node is dragged.
    static void DrawLineShape(ID2D1RenderTarget* t, ID2D1Brush* br, const Shape& s) {
        float w = std::max(s.size, 1.f);
        ID2D1StrokeStyle* ss = BorderStroke(s.borderStyle, true);
        bool curve = s.centerActive && s.pts.size() > 2;
        std::vector<D2D1_POINT_2F> b = curve ? sx::CurveBeziers(s.pts) : std::vector<D2D1_POINT_2F>{s.pts.front(), s.pts.back()};
        if (s.tool == T_Arrow) {
            D2D1_POINT_2F a = s.pts.front(), z = s.pts.back();
            float chord = std::hypot(z.x - a.x, z.y - a.y);
            int dir = s.arrowDir;
            if (dir == 2 && chord <= 6 * w * 2) dir = 0;  // ShareX: both heads only when long enough
            size_t n = b.size();
            if (dir == 0 || dir == 2) {  // end
                D2D1_POINT_2F from = curve ? b[n - 2] : a, end;
                if (curve && std::hypot(z.x - from.x, z.y - from.y) < 1) from = s.pts[s.pts.size() - 2];
                float seg = curve ? std::hypot(z.x - s.pts[s.pts.size() - 2].x, z.y - s.pts[s.pts.size() - 2].y) : chord;
                ArrowHead(t, br, from, z, w, &end, seg);
                float dx = end.x - z.x, dy = end.y - z.y;
                b[n - 1] = end;
                if (curve) b[n - 2].x += dx, b[n - 2].y += dy;
            }
            if (dir == 1 || dir == 2) {  // start
                D2D1_POINT_2F from = curve ? b[1] : z, end;
                if (curve && std::hypot(a.x - from.x, a.y - from.y) < 1) from = s.pts[1];
                float seg = curve ? std::hypot(a.x - s.pts[1].x, a.y - s.pts[1].y) : chord;
                ArrowHead(t, br, from, a, w, &end, seg);
                float dx = end.x - a.x, dy = end.y - a.y;
                b[0] = end;
                if (curve) b[1].x += dx, b[1].y += dy;
            }
        }
        t->DrawGeometry(PathOf(b, curve).Get(), br, w, ss);
    }

    // Freehand arrow: the stroke stops at the base of each head; drawn to the tip, its round cap showed as a dot past it.
    static void DrawFreehandArrow(ID2D1RenderTarget* t, ID2D1Brush* br, const Shape& s) {
        std::vector<D2D1_POINT_2F> body = s.pts;
        float head = s.size * 3 + 10;
        auto headFrom = [&](const std::vector<D2D1_POINT_2F>& v) {  // a point a little way back, so jitter does not twist the head
            D2D1_POINT_2F tip = v.back(), from = v.front();
            float reach = head * 1.7f;  // far enough that the head keeps full size and meets the trimmed stroke
            for (size_t i = v.size(); i-- > 0;) {
                float dx = tip.x - v[i].x, dy = tip.y - v[i].y;
                if (dx * dx + dy * dy > reach * reach) { from = v[i]; break; }
            }
            return from;
        };
        auto trim = [&](std::vector<D2D1_POINT_2F>& v) {
            D2D1_POINT_2F tip = v.back();
            size_t keep = v.size();
            while (keep > 1) {
                float dx = tip.x - v[keep - 1].x, dy = tip.y - v[keep - 1].y;
                if (dx * dx + dy * dy >= head * head) break;
                --keep;
            }
            D2D1_POINT_2F last = v[keep - 1];
            v.resize(keep);
            float dx = tip.x - last.x, dy = tip.y - last.y, d = std::sqrt(dx * dx + dy * dy);
            if (d > head) v.push_back({tip.x - dx / d * (head - 2), tip.y - dy / d * (head - 2)});
        };
        bool atEnd = s.arrowDir != 1, atStart = s.arrowDir != 0;
        std::vector<D2D1_POINT_2F> rev(s.pts.rbegin(), s.pts.rend());
        D2D1_POINT_2F endFrom = headFrom(s.pts), startFrom = headFrom(rev);
        if (atEnd) trim(body);
        if (atStart && body.size() > 1) {
            std::reverse(body.begin(), body.end());
            trim(body);
            std::reverse(body.begin(), body.end());
        }
        if (body.size() >= 2) t->DrawGeometry(PathOf(body, false).Get(), br, s.size, BorderStroke(s.borderStyle, true));
        if (atEnd) ArrowHead(t, br, endFrom, s.pts.back(), s.size, nullptr);
        if (atStart) ArrowHead(t, br, startFrom, s.pts.front(), s.size, nullptr);
    }

    // Animated stickers: one clock for the document; exports set animClock to render a given moment.
    DWORD animStart = GetTickCount();
    long long animClock = -1;
    bool animTimer = false;
    long long animSig = -1;
    int animTicks = 0;  // timer ticks (tests)
    long long AnimNow() const { return animClock >= 0 ? animClock : (long long)(GetTickCount() - animStart); }
    const std::shared_ptr<Image>& FrameOf(const Shape& s) const {
        if (!s.anim || s.anim->frames.empty()) return s.bmp;
        return s.anim->frames[s.anim->FrameAt(AnimNow())];
    }
    bool HasAnimation() const {
        return std::any_of(shapes.begin(), shapes.end(), [](const Shape& s) { return s.anim && s.tool != T_Crop; });
    }
    long long AnimSignature() const {  // changes when any animated shape shows another frame
        long long sig = 0;
        for (const Shape& s : shapes)
            if (s.anim) sig = sig * 131 + s.anim->FrameAt(AnimNow()) + 1;
        return sig;
    }
    // ~30 Hz while the document has an animated sticker and the window can be seen; no timer otherwise.
    void UpdateAnimTimer() {
        if (!hwnd) return;
        bool want = HasAnimation() && IsWindowVisible(hwnd) && !IsIconic(hwnd);
        if (want == animTimer) return;
        animTimer = want;
        if (want) SetTimer(hwnd, 9, 33, nullptr);
        else KillTimer(hwnd, 9);
    }

    ID2D1Bitmap* ShapeBitmap(ID2D1RenderTarget* t, Gfx& gx, const std::shared_ptr<Image>& im) {
        if (!im || im->empty()) return nullptr;
        auto it = gx.shapeImgs.find(im);
        if (it != gx.shapeImgs.end()) return it->second.Get();
        ComPtr<ID2D1Bitmap> b;
        t->CreateBitmap(D2D1::SizeU(im->w, im->h), im->px.data(), im->w * 4,
                        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
        gx.shapeImgs[im] = b;
        return b.Get();
    }

    // Draws one shape; `shadow` is the drop-shadow pass (ShareX: offset (0, 1), the border only when there is one,
    // otherwise an opaque fill).
    void DrawShape(ID2D1RenderTarget* t, Gfx& gx, const Shape& s, bool shadow) {
        ID2D1SolidColorBrush* br = gx.br.Get();
        ID2D1SolidColorBrush* br2 = gx.br2.Get();
        br->SetColor(shadow ? s.shadowColor : s.color);
        br2->SetColor(shadow ? s.shadowColor : s.color2);
        bool borderVis = s.size > 0 && s.color.a > 0, fillVis = s.color2.a > 0;
        bool fillShadow = shadow && !borderVis && s.color2.a >= 1;  // ShareX: FillColor.A == 255
        switch (s.tool) {
        case T_Rect: {
            D2D1_ROUNDED_RECT rr{Norm(s.pts[0], s.pts[1]), s.radius, s.radius};
            if (fillVis && (!shadow || fillShadow)) t->FillRoundedRectangle(rr, br2);
            if (borderVis) t->DrawRoundedRectangle(rr, br, s.size, BorderStroke(s.borderStyle, false));
            break;
        }
        case T_Ellipse: {
            D2D1_RECT_F r = Norm(s.pts[0], s.pts[1]);
            D2D1_ELLIPSE e{{(r.left + r.right) / 2, (r.top + r.bottom) / 2}, (r.right - r.left) / 2, (r.bottom - r.top) / 2};
            if (fillVis && (!shadow || fillShadow)) t->FillEllipse(e, br2);
            if (borderVis) t->DrawEllipse(e, br, s.size, BorderStroke(s.borderStyle, false));
            break;
        }
        case T_Line:
        case T_Arrow:
            DrawLineShape(t, br, s);
            break;
        case T_Freehand:
            if (s.geo) t->DrawGeometry(s.geo.Get(), br, s.size, BorderStroke(s.borderStyle, true));
            else t->FillEllipse(D2D1::Ellipse(s.pts[0], s.size / 2, s.size / 2), br);
            break;
        case T_FreehandArrow:
            if (s.pts.size() < 2) t->FillEllipse(D2D1::Ellipse(s.pts[0], s.size / 2, s.size / 2), br);
            else DrawFreehandArrow(t, br, s);
            break;
        case T_TextOutline: {
            if (s.text.empty()) break;
            D2D1_RECT_F in = TextInner(s);
            auto lay = TextLayoutIn(s, in);
            OutlineRenderer r;
            r.rt = t;
            bool outline = s.size2 > 0 && s.color.a > 0;
            if (shadow) {  // ShareX: the outline in the shadow colour, or the filled text when there is no outline
                r.fill = outline ? nullptr : br;
                r.stroke = outline ? br : nullptr;
            } else {
                r.fill = br2;
                r.stroke = outline ? br : nullptr;
            }
            r.strokeW = s.size2;
            lay->Draw(nullptr, &r, in.left, in.top);
            break;
        }
        case T_TextBackground:
        case T_SpeechBalloon: {
            D2D1_RECT_F b = Norm(s.pts[0], s.pts[1]);
            ComPtr<ID2D1Geometry> shape;
            if (s.tool == T_SpeechBalloon) {
                ComPtr<ID2D1RectangleGeometry> rg;
                D2D()->CreateRectangleGeometry(b, &rg);
                shape = rg;
                if (!InRect(b, s.tail)) {  // ShareX TailVisible
                    if (auto tail = TailGeometry(b, s.tail, 0.3f)) {
                        ComPtr<ID2D1PathGeometry> u;
                        D2D()->CreatePathGeometry(&u);
                        ComPtr<ID2D1GeometrySink> sink;
                        u->Open(&sink);
                        rg->CombineWithGeometry(tail.Get(), D2D1_COMBINE_MODE_UNION, nullptr, sink.Get());
                        sink->Close();
                        shape = u;
                    }
                }
                if (fillVis && (!shadow || fillShadow)) t->FillGeometry(shape.Get(), br2);
            } else if (fillVis && (!shadow || fillShadow)) {
                t->FillRoundedRectangle({b, s.radius, s.radius}, br2);
            }
            if (s.text.empty()) break;
            D2D1_RECT_F in = TextInner(s);
            t->DrawTextLayout({in.left, in.top}, TextLayoutIn(s, in).Get(), br);
            break;
        }
        case T_Step: {
            float r = StepRadius(s);
            D2D1_RECT_F c{s.pts[0].x - r, s.pts[0].y - r, s.pts[0].x + r, s.pts[0].y + r};
            if (!shadow || fillShadow) {
                if (s.tailActive && !InRect(c, s.tail))
                    if (auto tail = TailGeometry(c, s.tail, 1.f)) t->FillGeometry(tail.Get(), br2);
                t->FillEllipse(D2D1::Ellipse(s.pts[0], r, r), br2);
            }
            D2D1_SIZE_F z = TextSize(s);
            t->DrawTextLayout({s.pts[0].x - z.width / 2, s.pts[0].y - z.height / 2}, Layout(s).Get(), br);
            break;
        }
        case T_Image:
        case T_ImageScreen:
        case T_Sticker:
        case T_Cursor: {
            if (shadow) break;
            // Pasted/screen images stay pixel-exact like ShareX (NearestNeighbor); stickers are artwork and get
            // smooth scaling, otherwise an enlarged emoji turns into blocks.
            if (ID2D1Bitmap* b = ShapeBitmap(t, gx, FrameOf(s)))
                t->DrawBitmap(b, Norm(s.pts[0], s.pts[1]), 1,
                              s.tool == T_Sticker ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            break;
        }
        case T_Magnify: {
            if (shadow) break;
            D2D1_RECT_F r = Norm(s.pts[0], s.pts[1]);
            float k = s.size2 / 100, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
            t->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
            D2D1_RECT_F dst{cx - cx * k, cy - cy * k, cx + (img->w - cx) * k, cy + (img->h - cy) * k};
            t->DrawBitmap(gx.img.Get(), dst, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            t->PopAxisAlignedClip();
            t->DrawRectangle(r, br, 2);
            break;
        }
        case T_SmartEraser:
            if (shadow) break;
            t->FillRectangle(Norm(s.pts[0], s.pts[1]), br2);
            break;
        case T_Blur:
        case T_Pixelate:
        case T_Highlight: {
            if (shadow) break;
            D2D1_RECT_F r = Norm(s.pts[0], s.pts[1]);
            t->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
            if (s.tool == T_Pixelate) {
                int b = std::max(2, (int)s.size2);
                const Image& pi = FxImage(FX_Pixel, b);
                t->DrawBitmap(FxBitmap(t, gx, FX_Pixel, b), {0, 0, (float)pi.w * b, (float)pi.h * b}, 1,
                              D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            } else {
                ID2D1Bitmap* b = s.tool == T_Blur ? FxBitmap(t, gx, FX_Blur, (uint32_t)s.size2)
                                                  : FxBitmap(t, gx, FX_Highlight, Packed(s.color2));
                t->DrawBitmap(b, {0, 0, (float)img->w, (float)img->h}, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            }
            t->PopAxisAlignedClip();
            break;
        }
        default:
            break;
        }
    }

    static bool HasShadow(const Shape& s) {
        Tool t = s.tool;
        return s.shadow && (t == T_Rect || t == T_Ellipse || t == T_Freehand || t == T_FreehandArrow || t == T_Line || t == T_Arrow ||
                            IsText(t) || t == T_Step);
    }

    void DrawDocument(ID2D1RenderTarget* t, Gfx& gx, bool interactive, bool drawImage = true) {
        D2D1_RECT_F c = Crop();
        D2D1_MATRIX_3X2_F base;
        t->GetTransform(&base);
        t->PushAxisAlignedClip(c, D2D1_ANTIALIAS_MODE_ALIASED);
        if (drawImage)
            t->DrawBitmap(gx.img.Get(), D2D1::RectF(0, 0, (float)img->w, (float)img->h), 1,
                          interactive && scale < 1 ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                                                   : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        for (const Shape& s : shapes) {
            if (s.tool == T_Crop || s.tool == T_CutOut) continue;
            if (HasShadow(s)) {
                t->SetTransform(D2D1::Matrix3x2F::Translation(0, 1) * base);
                DrawShape(t, gx, s, true);
                t->SetTransform(base);
            }
            DrawShape(t, gx, s, false);
        }
        t->PopAxisAlignedClip();
    }

    // Renders image + annotations. `area` defaults to the crop; `mask` (region ellipse/freehand) makes
    // everything outside it transparent, as ShareX does for non-rectangular regions.
    Image Export(const D2D1_RECT_F* area = nullptr, ID2D1Geometry* mask = nullptr) {
        Image out;
        if (!img || img->w <= 0 || img->h <= 0 ||
            (uint64_t)img->w * img->h > UINT_MAX / 4 ||
            img->px.size() < (size_t)img->w * img->h) return out;
        if (!area && !mask && shapes.empty() && !img->alpha && img->px.size() == (size_t)img->w * img->h) {
            out = *img;
            // Opaque D2D bitmap uploads ignore source alpha; its output is always255.
            for (uint32_t& pixel : out.px) pixel |= 0xFF000000u;
            return out;
        }
        D2D1_RECT_F c = area ? *area : Crop();
        if (!std::isfinite(c.left) || !std::isfinite(c.top) || !std::isfinite(c.right) || !std::isfinite(c.bottom) ||
            c.right - c.left < 1 || c.bottom - c.top < 1 ||
            (double)(c.right - c.left) > INT_MAX || (double)(c.bottom - c.top) > INT_MAX) return out;
        int w = (int)(c.right - c.left), h = (int)(c.bottom - c.top);
        if (w <= 0 || h <= 0 || (uint64_t)w * h > UINT_MAX / 4) return out;
        ComPtr<IWICImagingFactory> wic;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return out;
        ComPtr<IWICBitmap> bmp;
        if (FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return out;
        ComPtr<ID2D1RenderTarget> t;
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(D2D()->CreateWicBitmapRenderTarget(bmp.Get(), p, &t))) return out;
        Gfx gx = MakeGfx(t.Get(), *img);
        t->BeginDraw();
        bool transparent = mask || img->alpha;
        t->Clear(transparent ? kClear : kWhite);
        t->SetTransform(D2D1::Matrix3x2F::Translation(-c.left, -c.top));
        if (mask) t->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), mask), nullptr);
        DrawDocument(t.Get(), gx, false);
        if (mask) t->PopLayer();
        t->EndDraw();
        out.w = w;
        out.h = h;
        out.alpha = transparent;
        out.px.resize((size_t)w * h);
        bmp->CopyPixels(nullptr, w * 4, (UINT)(out.px.size() * 4), (BYTE*)out.px.data());
        if (!transparent) {
            for (auto& px : out.px) px |= 0xFF000000u;
        } else {
            for (auto& px : out.px) {  // premultiplied -> straight alpha
                uint32_t a = px >> 24;
                if (a == 0 || a == 255) continue;
                px = a << 24 | std::min(255u, (px >> 16 & 255) * 255 / a) << 16 | std::min(255u, (px >> 8 & 255) * 255 / a) << 8 |
                     std::min(255u, (px & 255) * 255 / a);
            }
            if (!mask) out.alpha = std::any_of(out.px.begin(), out.px.end(), [](uint32_t p) { return (p >> 24) != 255; });
        }
        return out;
    }

    // ---------- toolbar ----------
    bool ShowBorder() const {
        return tool == T_Rect || tool == T_Ellipse || IsFreehand(tool) || tool == T_Line || tool == T_Arrow ||
               IsText(tool) || tool == T_Step || tool == T_Magnify;
    }
    bool ShowFill() const { return tool == T_Rect || tool == T_Ellipse || IsText(tool) || tool == T_Step; }
    bool ShowOptions() const {
        return (!IsRegion(tool) || tool == T_RegionRect) && tool != T_Select && tool != T_Crop && tool != T_SmartEraser &&
               tool != T_Highlight && tool != T_Image && tool != T_ImageScreen;
    }
    // Tools present in this mode: region tools only on the capture surface, crop and cut out only in the editor.
    bool ToolAvailable(int t) const { return region ? t != T_Crop && t != T_CutOut : !IsRegion((Tool)t); }

    D2D1_COLOR_F* BorderRef() {
        if (tool == T_TextOutline) return &o.outlineColor;
        if (tool == T_TextBackground || tool == T_SpeechBalloon) return &o.textColor;
        if (tool == T_Step) return &o.stepText;
        return &o.border;
    }
    D2D1_COLOR_F* FillRef() {
        if (tool == T_TextOutline) return &o.outlineText;
        if (tool == T_TextBackground || tool == T_SpeechBalloon) return &o.textFill;
        if (tool == T_Step) return &o.stepFill;
        return &o.fill;
    }

    void LayoutToolbar() {
        RECT cr;
        GetClientRect(hwnd, &cr);
        float bs = std::round(24 * ui), gap = std::round(1 * ui), sep = std::round(9 * ui);
        // Same items and order as ShareX's ShapeManagerMenu for the editor / region capture.
        std::vector<std::pair<BtnKind, int>> items;
        if (region) {
            if (!o.menuLocked) items = {{B_Grip, 0}};  // ShareX "Lock menu" hides the drag grips
        } else {
            items = {{B_Apply, 0}, {B_Save, 0}, {B_SaveAs, 0}, {B_Copy, 0}, {B_Sep, 0}, {B_Close, 0}, {B_Sep, 0}};
        }
        for (Tool i : kToolOrder) {
            if (!ToolAvailable(i)) continue;
            if (region && i == T_Select) items.push_back({B_Sep, 0});
            items.push_back({B_Tool, i});
        }
        items.push_back({B_Sep, 0});
        if (ShowBorder()) items.push_back({B_Border, 0});
        if (ShowFill()) items.push_back({B_Fill, 0});
        if (tool == T_Highlight) items.push_back({B_Highlight, 0});
        if (ShowOptions()) items.push_back({B_Options, 0});
        items.push_back({B_Edit, 0});
        if (!region) items.push_back({B_Image, 0});  // ShareX: editor only
        if (region) {
            items.push_back({B_Sep, 0});
            items.push_back({B_Capture, 0});
            items.push_back({B_RegionOptions, 0});
        }
        float total = 0;
        for (auto& it : items) total += (it.first == B_Sep ? sep : it.first == B_Grip ? std::round(10 * ui) : bs) + gap;
        float pad = std::round(4 * ui);
        float x = std::max(pad, std::round((cr.right - total) / 2)), y = pad;
        if (region) {
            x = std::round(mon.left + (mon.right - mon.left - total) / 2 + gripDX);
            y = std::round(mon.top + pad + gripDY);
        }
        buttons.clear();
        for (auto& it : items) {
            float w = it.first == B_Sep ? sep : it.first == B_Grip ? std::round(10 * ui) : bs;
            buttons.push_back({it.first, it.second, {x, y, x + w, y + bs}});
            x += w + gap;
        }
        tbH = bs + 2 * pad;
    }

    std::wstring Tooltip(const Button& b) const {
        switch (b.kind) {
        case B_Apply: return Tr(L"Применить и сохранить (Enter)", L"Apply changes and save (Enter)");
        case B_Save: return Tr(L"Сохранить (Ctrl+S)", L"Save image (Ctrl+S)");
        case B_SaveAs: return Tr(L"Сохранить как... (Ctrl+Shift+S)", L"Save image as... (Ctrl+Shift+S)");
        case B_Copy: return Tr(L"Копировать в буфер (Ctrl+C)", L"Copy image to clipboard (Ctrl+C)");
        case B_Close: return Tr(L"Закрыть (Esc)", L"Close (Esc)");
        case B_Tool: return Tr(kTools[b.idx].ru, kTools[b.idx].en);
        case B_Border: return Tr(L"Цвет обводки...", L"Border color...");
        case B_Fill: return Tr(L"Цвет заливки...", L"Fill color...");
        case B_Highlight: return Tr(L"Цвет маркера...", L"Highlight color...");
        case B_Options: return Tr(L"Параметры инструмента", L"Tool options");
        case B_Edit: return Tr(L"Правка", L"Edit");
        case B_Grip: return Tr(L"Перетащить панель", L"Drag the toolbar");
        case B_Capture: return Tr(L"Захват", L"Capture");
        case B_RegionOptions: return Tr(L"Параметры", L"Options");
        case B_Image: return Tr(L"Изображение", L"Image");
        default: return L"";
        }
    }

    void DrawIcon(int id, D2D1_RECT_F r) {
        int px = (int)std::round(16 * ui);
        auto it = g.icons.find(id * 1000 + px);
        if (it == g.icons.end()) {
            const IconData& d = Icon(id, px);
            ComPtr<ID2D1Bitmap> b;
            if (d.w)
                rt->CreateBitmap(D2D1::SizeU(d.w, d.h), d.px.data(), d.w * 4,
                                 D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
            it = g.icons.emplace(id * 1000 + px, b).first;
        }
        if (!it->second) return;
        float s = (float)px, x0 = std::round((r.left + r.right - s) / 2), y0 = std::round((r.top + r.bottom - s) / 2);
        rt->DrawBitmap(it->second.Get(), {x0, y0, x0 + s, y0 + s}, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }

    void DrawSwatch(D2D1_RECT_F r, D2D1_COLOR_F c) {
        float s = std::round(16 * ui), cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        D2D1_RECT_F q{cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2};
        if (c.a < 1) {  // checkerboard under transparent colours
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) {
                    g.br->SetColor((i + j) % 2 ? Rgb(200, 200, 200) : kWhite);
                    rt->FillRectangle({q.left + i * s / 4, q.top + j * s / 4, q.left + (i + 1) * s / 4, q.top + (j + 1) * s / 4}, g.br.Get());
                }
        }
        g.br->SetColor(c);
        rt->FillRectangle(q, g.br.Get());
        g.br->SetColor(Rgb(10, 10, 10));
        rt->DrawRectangle(q, g.br.Get(), 1);
    }

    D2D1_RECT_F BarRect() const {
        float pad = std::round(4 * ui);
        return {buttons.front().r.left - pad, buttons.front().r.top - pad, buttons.back().r.right + pad, buttons.front().r.bottom + pad};
    }

    void DrawToolbar() {
        if (buttons.empty()) return;
        D2D1_RECT_F bar = BarRect();
        D2D1_ROUNDED_RECT rr{bar, 4 * ui, 4 * ui};
        g.br->SetColor(kBarBg);
        rt->FillRoundedRectangle(rr, g.br.Get());
        g.br->SetColor(kBarBorder);
        rr.rect = {bar.left + .5f, bar.top + .5f, bar.right - .5f, bar.bottom - .5f};
        rt->DrawRoundedRectangle(rr, g.br.Get(), 1);
        for (int i = 0; i < (int)buttons.size(); ++i) {
            const Button& b = buttons[i];
            if (b.kind == B_Sep) {
                float cx = std::round((b.r.left + b.r.right) / 2) + .5f;
                g.br->SetColor(Rgb(70, 70, 74));
                rt->DrawLine({cx, b.r.top + 3 * ui}, {cx, b.r.bottom - 3 * ui}, g.br.Get(), 1);
                continue;
            }
            bool checked = b.kind == B_Tool && b.idx == tool;
            if (checked || i == hoverBtn) {
                g.br->SetColor(checked ? kChecked : kHover);
                rt->FillRectangle(b.r, g.br.Get());
                g.br->SetColor(kAccent);
                if (checked) rt->DrawRectangle({b.r.left + .5f, b.r.top + .5f, b.r.right - .5f, b.r.bottom - .5f}, g.br.Get(), 1);
            }
            switch (b.kind) {
            case B_Apply: DrawIcon(100, b.r); break;
            case B_Save: DrawIcon(101, b.r); break;
            case B_SaveAs: DrawIcon(102, b.r); break;
            case B_Copy: DrawIcon(103, b.r); break;
            case B_Close: DrawIcon(104, b.r); break;
            case B_Tool: DrawIcon(kTools[b.idx].icon, b.r); break;
            case B_Border: DrawSwatch(b.r, *BorderRef()); break;
            case B_Fill: DrawSwatch(b.r, *FillRef()); break;
            case B_Highlight: DrawSwatch(b.r, o.highlight); break;
            case B_Options: DrawIcon(121, b.r); break;
            case B_Edit: DrawIcon(122, b.r); break;
            case B_Capture: DrawIcon(129, b.r); break;
            case B_RegionOptions: DrawIcon(130, b.r); break;
            case B_Image: DrawIcon(170, b.r); break;
            case B_Grip:  // ShareX draws a dotted grip
                for (int k = 0; k < 4; ++k) {
                    float cx = std::round((b.r.left + b.r.right) / 2), cy = std::round(b.r.top + (k + 1) * (b.r.bottom - b.r.top) / 5);
                    g.br->SetColor(Rgb(31, 31, 31));
                    rt->FillRectangle({cx - ui, cy - ui, cx + ui, cy + ui}, g.br.Get());
                    g.br->SetColor(Rgb(80, 80, 80));
                    rt->FillRectangle({cx - ui + 1, cy - ui + 1, cx + ui + 1, cy + ui + 1}, g.br.Get());
                }
                break;
            default: break;
            }
        }
        if (showTip && hoverBtn >= 0) {
            std::wstring tip = Tooltip(buttons[hoverBtn]);
            ComPtr<IDWriteTextLayout> lay;
            DW()->CreateTextLayout(tip.c_str(), (UINT32)tip.size(), tipFont.Get(), 2000, 100, &lay);
            DWRITE_TEXT_METRICS m;
            lay->GetMetrics(&m);
            RECT cr;
            GetClientRect(hwnd, &cr);
            float x = std::min(buttons[hoverBtn].r.left, cr.right - m.width - 12 * ui), y = BarRect().bottom + 4 * ui;
            D2D1_RECT_F box{x, y, x + m.width + 12 * ui, y + m.height + 8 * ui};
            g.br->SetColor(Rgb(28, 28, 30));
            rt->FillRectangle(box, g.br.Get());
            g.br->SetColor(Rgb(80, 80, 86));
            rt->DrawRectangle(box, g.br.Get(), 1);
            g.br->SetColor(kText);
            rt->DrawTextLayout({x + 6 * ui, y + 4 * ui}, lay.Get(), g.br.Get());
        }
    }

    // ---------- view ----------
    void LayoutAll() {
        if (!hwnd) return;
        LayoutToolbar();
        if (region) {  // 1:1 over the desktop
            scale = 1;
            offX = offY = 0;
            return;
        }
        if (userZoom) return;
        RECT cr;
        GetClientRect(hwnd, &cr);
        D2D1_RECT_F c = Crop();
        float cw = c.right - c.left, ch = c.bottom - c.top;
        float aw = cr.right - 20 * ui, ah = cr.bottom - tbH - 20 * ui;
        scale = std::min({1.0f, aw / cw, ah / ch});
        if (scale <= 0) scale = 1;
        offX = std::round((cr.right - cw * scale) / 2);
        offY = std::round(tbH + (cr.bottom - tbH - ch * scale) / 2);
    }

    D2D1_MATRIX_3X2_F ViewMatrix() const {
        D2D1_RECT_F c = Crop();
        return D2D1::Matrix3x2F::Translation(-c.left, -c.top) * D2D1::Matrix3x2F::Scale(scale, scale) *
               D2D1::Matrix3x2F::Translation(offX, offY);
    }

    D2D1_POINT_2F ToImage(int x, int y) const {
        D2D1_RECT_F c = Crop();
        return {(x - offX) / scale + c.left, (y - offY) / scale + c.top};
    }

    D2D1_RECT_F ToScreen(D2D1_RECT_F r) const {
        D2D1_RECT_F c = Crop();
        return {(r.left - c.left) * scale + offX, (r.top - c.top) * scale + offY, (r.right - c.left) * scale + offX,
                (r.bottom - c.top) * scale + offY};
    }

    void Render() {
        if (region && o.showFps) {  // ShareX FPSManager: frames counted per second
            DWORD now = GetTickCount();
            if (!fpsTick) fpsTick = now;
            ++fpsFrames;
            if (now - fpsTick >= 1000) {
                fps = (int)std::lround(fpsFrames * 1000.0 / std::max<DWORD>(1, now - fpsTick));
                fpsFrames = 0;
                fpsTick = now;
            }
        }
        if (!rt) {
            if (rendererMode == 0 && !useWarp) {
                wchar_t mode[16]{};
                GetPrivateProfileStringW(L"main", L"Renderer", L"auto", mode, 16, Opts::Ini().c_str());
                if (_wcsicmp(mode, L"cpu") == 0 || _wcsicmp(mode, L"warp") == 0) { rendererMode = 2; useWarp = true; }
                else if (_wcsicmp(mode, L"gpu") == 0) rendererMode = 1;
            }
            RECT cr;
            GetClientRect(hwnd, &cr);
            D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
                useWarp ? D2D1_RENDER_TARGET_TYPE_SOFTWARE : D2D1_RENDER_TARGET_TYPE_DEFAULT);
            p.dpiX = p.dpiY = 96;
            D2D()->CreateHwndRenderTarget(
                p, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(cr.right, cr.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY), &rt);
            if (!rt && !useWarp) {
                useWarp = true;
                p.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
                D2D()->CreateHwndRenderTarget(
                    p, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(cr.right, cr.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY), &rt);
            }
            if (!rt) return;
            g = MakeGfx(rt.Get(), *img);
        }
        rt->BeginDraw();
        rt->Clear(kCanvas);
        if (!region && img->alpha) DrawChecker();
        rt->SetTransform(ViewMatrix());
        if (region) RenderRegionBackground();
        else DrawDocument(rt.Get(), g, true);
        if (cropping) {
            D2D1_RECT_F c = Crop(), r = Norm(cropA, cropB);
            g.br->SetColor(Rgb(0, 0, 0, 140));
            rt->FillRectangle({c.left, c.top, c.right, r.top}, g.br.Get());
            rt->FillRectangle({c.left, r.bottom, c.right, c.bottom}, g.br.Get());
            rt->FillRectangle({c.left, r.top, r.left, r.bottom}, g.br.Get());
            rt->FillRectangle({r.right, r.top, c.right, r.bottom}, g.br.Get());
        }
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        if (cropping) {
            g.br->SetColor(kWhite);
            rt->DrawRectangle(ToScreen(Norm(cropA, cropB)), g.br.Get(), 1, DashStroke());
        }
        if (selected >= 0 && selected < (int)shapes.size() && !drawing && !IsFreehand(tool)) {
            const Shape& sel = shapes[selected];
            std::vector<Node> nodes = Nodes(sel);
            if (nodes.empty() || sel.tool == T_Step) {  // shapes without box nodes keep the dashed frame
                D2D1_RECT_F b = ToScreen(Bounds(sel));
                b = {std::floor(b.left) - 2.5f, std::floor(b.top) - 2.5f, std::ceil(b.right) + 2.5f, std::ceil(b.bottom) + 2.5f};
                g.br->SetColor(Rgb(0, 0, 0, 160));
                rt->DrawRectangle(b, g.br.Get(), 1);
                g.br->SetColor(kWhite);
                rt->DrawRectangle(b, g.br.Get(), 1, DashStroke());
            }
            for (const Node& n : nodes) DrawNode(ToScreen({n.p.x, n.p.y, n.p.x, n.p.y}), n.id);
        }
        if (tool == T_Cursor)  // ShareX CursorDrawingShape: the stamped cursors are framed while the tool is active
            for (const Shape& s : shapes)
                if (s.tool == T_Cursor) DashedRect(ToScreen(Bounds(s)), false);
        if (drawing && !shapes.empty() && shapes.back().tool == T_ImageScreen && !shapes.back().bmp)
            DashedRect(ToScreen(Norm(shapes.back().pts[0], shapes.back().pts[1])), true);
        if (cutting || cutPending) RenderCutOut();
        if (editingText >= 0) {  // caret
            const Shape& s = shapes[editingText];
            D2D1_RECT_F in = TextInner(s);
            auto lay = TextLayoutIn(s, in);
            DWRITE_HIT_TEST_METRICS hm;
            float cx, cy;
            lay->HitTestTextPosition((UINT32)s.text.size(), FALSE, &cx, &cy, &hm);
            if (s.text.empty()) {  // the layout holds a space; the caret goes where the first letter will
                lay->HitTestTextPosition(0, FALSE, &cx, &cy, &hm);
            }
            D2D1_RECT_F c = ToScreen({in.left + cx, in.top + cy, in.left + cx, in.top + cy + hm.height});
            g.br->SetColor(kAccent);
            rt->DrawLine({c.left + 1, c.top}, {c.left + 1, c.bottom}, g.br.Get(), 2);
        }
        if (region) RenderRegionOverlay();
        DrawToolbar();
        LARGE_INTEGER qf{}, qt0{}, qt1{};
        const bool probeGpu = !useWarp && rendererMode == 0;
        if (probeGpu) { QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qt0); }
        HRESULT endHr = rt->EndDraw();
        lastPaintMs = GetTickCount64();
        if (endHr == D2DERR_RECREATE_TARGET) {
            rt.Reset();
            g = {};
        } else if (probeGpu && qf.QuadPart > 0) {
            QueryPerformanceCounter(&qt1);
            double endMs = (qt1.QuadPart - qt0.QuadPart) * 1000.0 / qf.QuadPart;
            if (endMs > 28.0) {
                if (++slowGpuFrames >= 2) {
                    Log(L"editor: GPU EndDraw stalled (%.1f ms), switching to WARP software renderer", endMs);
                    useWarp = true;
                    slowGpuFrames = 0;
                    rt.Reset();
                    g = {};
                }
            } else {
                slowGpuFrames = 0;
            }
        }
        animSig = AnimSignature();
        UpdateAnimTimer();
    }

    // ---------- resize nodes (ShareX ResizeNode; UseLightResizeNodes off draws CircleNode.png) ----------
    // id 0..7: NodePosition TopLeft, Top, TopRight, Right, BottomRight, Bottom, BottomLeft, Left; 8: Extra (tail);
    // 100 + i: line/arrow point i.
    struct Node { D2D1_POINT_2F p; int id; };
    int dragNode = -1;
    Shape nodeOrig;
    D2D1_POINT_2F nodeStart{};

    // Screen px at any zoom: ShareX's 24 px CircleNode, or the light nodes' size shared with region nodes.
    float NodeR() const { return RegionLightNodes() ? NodeSize() / 2 : std::round(7.5f * std::sqrt(ui) * 2) / 2; }

    std::vector<Node> Nodes(const Shape& s) const {
        std::vector<Node> v;
        float lim = NodeR() * 2 / scale;  // ShareX hides nodes that would overlap their neighbours
        if (HasBoxNodes(s.tool) && s.pts.size() >= 2) {
            D2D1_RECT_F r = Norm(s.pts[0], s.pts[1]);
            float xm = (r.left + r.right) / 2, ym = (r.top + r.bottom) / 2, w = r.right - r.left, h = r.bottom - r.top;
            D2D1_POINT_2F p[8] = {{r.left, r.top},     {xm, r.top},      {r.right, r.top}, {r.right, ym},
                                  {r.right, r.bottom}, {xm, r.bottom}, {r.left, r.bottom}, {r.left, ym}};
            bool vis[8] = {true, true, true, true, true, true, true, true};
            if (h / 2 < lim) vis[7] = vis[3] = false;
            if (w / 2 < lim) vis[1] = vis[5] = false;
            if (h < lim) vis[0] = vis[1] = vis[2] = false;
            if (w < lim) vis[0] = vis[7] = vis[6] = false;
            for (int i = 0; i < 8; ++i)
                if (vis[i]) v.push_back({p[i], i});
            if (s.tool == T_SpeechBalloon) v.push_back({s.tail, 8});
        } else if (IsLine(s.tool)) {
            D2D1_POINT_2F e = s.pts.back();
            for (size_t i = 0; i < s.pts.size(); ++i) {
                if (i + 1 < s.pts.size() && std::hypot(s.pts[i].x - e.x, s.pts[i].y - e.y) < lim) continue;
                v.push_back({s.pts[i], 100 + (int)i});
            }
        } else if (s.tool == T_Step) {
            v.push_back({s.tail, 8});
        }
        return v;
    }

    bool NodeAt(int x, int y, Node& out) const {
        if (selected < 0 || selected >= (int)shapes.size() || drawing || IsFreehand(tool)) return false;
        std::vector<Node> v = Nodes(shapes[selected]);
        float r = NodeR() + 2;
        for (size_t i = v.size(); i-- > 0;) {  // the last drawn (bottom right, tail) wins
            D2D1_RECT_F p = ToScreen({v[i].p.x, v[i].p.y, v[i].p.x, v[i].p.y});
            if (std::hypot(p.left - x, p.top - y) <= r) {
                out = v[i];
                return true;
            }
        }
        return false;
    }

    void DrawNode(D2D1_RECT_F at, int id) {
        D2D1_POINT_2F c{std::round(at.left) + .5f, std::round(at.top) + .5f};
        float r = NodeR();
        if (RegionLightNodes()) {  // ShareX UseLightResizeNodes: squares, circles for line points and tails
            g.br->SetColor(kWhite);
            if (id < 8) {
                D2D1_RECT_F q{c.x - r, c.y - r, c.x + r, c.y + r};
                rt->FillRectangle(q, g.br.Get());
                g.br->SetColor(Rgb(0, 0, 0));
                rt->DrawRectangle(q, g.br.Get(), 1);
            } else {
                rt->FillEllipse(D2D1::Ellipse(c, r, r), g.br.Get());
                g.br->SetColor(Rgb(0, 0, 0));
                rt->DrawEllipse(D2D1::Ellipse(c, r, r), g.br.Get(), 1);
            }
            return;
        }
        g.br->SetColor(Rgb(0, 0, 0, 28));
        rt->FillEllipse(D2D1::Ellipse(c, r + 3, r + 3), g.br.Get());
        g.br->SetColor(Rgb(0, 0, 0, 55));
        rt->FillEllipse(D2D1::Ellipse(c, r + 1.5f, r + 1.5f), g.br.Get());
        g.br->SetColor(kWhite);
        rt->FillEllipse(D2D1::Ellipse(c, r, r), g.br.Get());
    }

    static HCURSOR NodeCursor(int id) {
        switch (id) {
        case 0: case 4: return LoadCursorW(nullptr, IDC_SIZENWSE);
        case 2: case 6: return LoadCursorW(nullptr, IDC_SIZENESW);
        case 1: case 5: return LoadCursorW(nullptr, IDC_SIZENS);
        case 3: case 7: return LoadCursorW(nullptr, IDC_SIZEWE);
        default: return LoadCursorW(nullptr, IDC_SIZEALL);
        }
    }

    void StartNodeDrag(const Node& n) {
        Snapshot();
        Shape& s = shapes[selected];
        if (HasBoxNodes(s.tool)) {  // work on a normalised box, like ShareX's Rectangle
            D2D1_RECT_F r = Norm(s.pts[0], s.pts[1]);
            s.pts[0] = {r.left, r.top};
            s.pts[1] = {r.right, r.bottom};
        }
        nodeOrig = s;
        nodeStart = n.p;
        dragNode = n.id;
    }

    // BaseShape.OnNodeUpdate (+ LineDrawingShape / SpeechBalloon / Step overrides).
    void DragNode(D2D1_POINT_2F p, bool proportional) {
        Shape& s = shapes[selected];
        if (dragNode == 8) {
            s.tail = p;
            if (s.tool == T_Step) s.tailActive = true;
            return;
        }
        if (dragNode >= 100) {
            size_t i = (size_t)(dragNode - 100);
            if (i >= s.pts.size()) return;
            s.pts[i] = p;
            if (i > 0 && i + 1 < s.pts.size()) s.centerActive = true;
            else AutoCenterPoints(s);
            return;
        }
        D2D1_RECT_F r0 = Norm(nodeOrig.pts[0], nodeOrig.pts[1]);
        D2D1_POINT_2F a{r0.left, r0.top}, b{r0.right, r0.bottom};
        float dx = p.x - nodeStart.x, dy = p.y - nodeStart.y;
        int i = dragNode;
        bool left = i == 0 || i == 7 || i == 6, right = i == 2 || i == 3 || i == 4, top = i == 0 || i == 1 || i == 2,
             bottom = i == 6 || i == 5 || i == 4;
        if (left) a.x += dx;
        if (right) b.x += dx;
        if (top) a.y += dy;
        if (bottom) b.y += dy;
        s.pts[0] = a;
        s.pts[1] = b;
        if (IsText(s.tool)) s.autoSize = false;
        if (proportional && (i == 0 || i == 2 || i == 4 || i == 6)) {  // Shift: keep InitialSize's proportions
            D2D1_SIZE_F init = s.initSize.width > 0 && s.initSize.height > 0 ? s.initSize
                                                                              : D2D1_SIZE_F{r0.right - r0.left, r0.bottom - r0.top};
            if (init.width <= 0 || init.height <= 0) return;
            D2D1_RECT_F nr = Norm(a, b);
            float ratio = std::min((nr.right - nr.left) / init.width, (nr.bottom - nr.top) / init.height);
            float nw = std::round(init.width * ratio), nh = std::round(init.height * ratio);
            D2D1_POINT_2F anchor{left ? r0.right : r0.left, top ? r0.bottom : r0.top};
            if (p.x < anchor.x) nr.left = nr.right - nw;
            nr.right = nr.left + nw;
            if (p.y < anchor.y) nr.top = nr.bottom - nh;
            nr.bottom = nr.top + nh;
            s.pts[0] = {nr.left, nr.top};
            s.pts[1] = {nr.right, nr.bottom};
        }
    }

    void EndNodeDrag() {
        dragNode = -1;
        if (selected >= 0 && selected < (int)shapes.size()) {
            Shape& s = shapes[selected];
            if (HasBoxNodes(s.tool)) {
                D2D1_RECT_F r = Norm(s.pts[0], s.pts[1]);
                s.pts[0] = {r.left, r.top};
                s.pts[1] = {r.right, r.bottom};
            }
        }
        Invalidate();
    }

    // ---------- shape list operations (ShapeManager) ----------
    void RenumberSteps() {  // ShapeManager.OrderStepShapes
        int n = o.startStep;
        for (auto& s : shapes)
            if (s.tool == T_Step) s.step = n++;
    }

    D2D1_POINT_2F CursorImagePos() const {
        POINT c;
        GetCursorPos(&c);
        ScreenToClient(hwnd, &c);
        return ToImage(c.x, c.y);
    }

    D2D1_POINT_2F ViewCenter() const {
        if (region) {
            return {(mon.left + mon.right) / 2.f, (mon.top + mon.bottom) / 2.f};
        }
        D2D1_RECT_F c = Crop();
        return {(c.left + c.right) / 2, (c.top + c.bottom) / 2};
    }

    static bool IsDocumentShape(Tool t) { return t != T_Crop && t != T_CutOut; }

    // Ctrl+D (at the mouse, centred) / Edit > Duplicate (+10, +10).
    void Duplicate(bool atMouse) {
        CommitText();
        if (selected < 0 || selected >= (int)shapes.size() || !IsDocumentShape(shapes[selected].tool)) return;
        Snapshot();
        Shape c = shapes[selected];
        float dx = 10, dy = 10;
        if (atMouse) {
            D2D1_RECT_F b = Bounds(c);
            D2D1_POINT_2F m = CursorImagePos();
            dx = m.x - (b.left + b.right) / 2;
            dy = m.y - (b.top + b.bottom) / 2;
        }
        MoveShape(c, dx, dy);
        shapes.push_back(std::move(c));
        selected = (int)shapes.size() - 1;
        RenumberSteps();
        Invalidate();
    }

    void InsertImage(Image im, D2D1_POINT_2F at, bool centre, Tool kind = T_Image, std::shared_ptr<const sx::Anim> anim = nullptr,
                     const std::wstring& src = L"") {
        if (im.empty()) return;
        Snapshot();
        Shape s = NewShape(at);
        s.tool = kind;
        float x = centre ? std::round(at.x - im.w / 2.f) : at.x, y = centre ? std::round(at.y - im.h / 2.f) : at.y;
        s.pts = {{x, y}, {x + im.w, y + im.h}};
        s.initSize = {(float)im.w, (float)im.h};
        s.bmp = std::make_shared<Image>(std::move(im));
        s.anim = std::move(anim);
        s.src = src;
        shapes.push_back(std::move(s));
        selected = (int)shapes.size() - 1;
        Invalidate();
    }

    // Ctrl+V (at the mouse) / Edit > Paste image/text (centre): ShapeManager.PasteFromClipboard.
    void Paste(bool atMouse) {
        CommitText();
        D2D1_POINT_2F at = atMouse ? CursorImagePos() : ViewCenter();
        Image im;
        std::wstring text;
        if (sx::ClipboardImage(hwnd, im)) {
            SetTool(T_Image);
            InsertImage(std::move(im), at, true);
        } else if (sx::ClipboardText(hwnd, text)) {
            SetTool(T_TextBackground);
            Snapshot();
            Shape s = NewShape(at);
            s.text = text;
            UpdateTextBox(s);
            MoveShape(s, -std::round((s.pts[1].x - s.pts[0].x) / 2), -std::round((s.pts[1].y - s.pts[0].y) / 2));  // AutoSize(center)
            shapes.push_back(std::move(s));
            selected = (int)shapes.size() - 1;
            Invalidate();
        }
    }

    // Home / End / Page Up / Page Down: bring to front, send to back, bring forward, send backward.
    enum Order { O_Front, O_Forward, O_Backward, O_Back };
    void Reorder(Order how) {
        CommitText();
        if (selected < 0 || selected >= (int)shapes.size() || !IsDocumentShape(shapes[selected].tool)) return;
        int n = (int)shapes.size(), to = selected;
        if (how == O_Front) to = n - 1;
        else if (how == O_Back) to = 0;
        else {  // step over crop / cut out records, which draw nothing
            int d = how == O_Forward ? 1 : -1;
            for (int i = selected + d; i >= 0 && i < n; i += d)
                if (IsDocumentShape(shapes[i].tool)) { to = i; break; }
        }
        if (to == selected) return;
        Snapshot();
        Shape s = std::move(shapes[selected]);
        shapes.erase(shapes.begin() + selected);
        shapes.insert(shapes.begin() + to, std::move(s));
        selected = to;
        RenumberSteps();
        Invalidate();
    }

    // ---------- image tools ----------
    bool OpenImageDialog(std::wstring& file) {
        wchar_t buf[MAX_PATH] = L"";
        std::wstring dir = o.lastImage.empty() ? L"" : o.lastImage.substr(0, o.lastImage.find_last_of(L'\\'));
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Image files (*.png, *.jpg, *.jpeg, *.jpe, *.jfif, *.gif, *.bmp, *.tif, *.tiff)\0"
                          L"*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.bmp;*.tif;*.tiff\0PNG (*.png)\0*.png\0"
                          L"JPEG (*.jpg, *.jpeg, *.jpe, *.jfif)\0*.jpg;*.jpeg;*.jpe;*.jfif\0GIF (*.gif)\0*.gif\0BMP (*.bmp)\0*.bmp\0"
                          L"TIFF (*.tif, *.tiff)\0*.tif;*.tiff\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return false;
        file = buf;
        return true;
    }

    // An animated GIF keeps all its frames; `limit` > 0 scales it down like ResizeLimit.
    static bool LoadImageOrAnim(const std::wstring& file, int limit, Image& im, std::shared_ptr<const sx::Anim>* anim) {
        sx::Anim a;
        if (anim && _wcsicmp(PathFindExtensionW(file.c_str()), L".gif") == 0 && sx::LoadAnimation(file, a)) {
            std::shared_ptr<sx::Anim> r = limit > 0 ? sx::ResizeAnim(a, limit) : std::make_shared<sx::Anim>(std::move(a));
            im = *r->frames[0];
            *anim = std::move(r);
            return true;
        }
        if (!sx::LoadImagePremul(file, im)) return false;
        if (limit > 0) im = sx::ResizeLimit(im, limit);
        return true;
    }

    bool LoadSticker(const std::wstring& file, Image& im, std::shared_ptr<const sx::Anim>* anim = nullptr) {
        if (!LoadImageOrAnim(file, std::max(8, o.stickerSize), im, anim)) return false;
        o.lastSticker = file;
        return true;
    }

    // ShareX ImageHelpers.OpenImageFileDialog / StickerForm, or the image a double-click replaces.
    bool AskImage(Tool kind, Image& im, std::shared_ptr<const sx::Anim>* anim = nullptr, std::wstring* src = nullptr) {
        std::wstring file;
        if (kind == T_Image) {
            if (!OpenImageDialog(file) || !LoadImageOrAnim(file, 0, im, anim)) return false;
            o.lastImage = file;
            o.Save();
            if (src) *src = file;
            return true;
        }
        if (kind == T_Sticker) {
            POINT at;
            GetCursorPos(&at);
            bool ok = sx::PickSticker(hwnd, at, ui, file, o.stickerSize) && LoadSticker(file, im, anim);
            o.Save();
            if (ok && src) *src = file;
            return ok;
        }
        return false;
    }

    // ShareX InsertImageFromScreen: hide, region capture of the live screen, show again.
    bool CaptureScreenImage(Image& im) {
        ShowWindow(hwnd, SW_HIDE);
        Sleep(250);
        RECT vsr = VirtualScreen();
        auto screen = std::make_shared<Image>(CaptureRect(vsr, false));
        Image out;
        RECT r;
        bool ok = RegionCapture(screen, vsr, out, r);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        if (ok) im = sx::PremulCopy(out);
        return ok && !im.empty();
    }

    // Image (File), Sticker, Cursor and, in the editor, Image (Screen): one click places the shape.
    void PlaceImageTool(D2D1_POINT_2F p) {
        Image im;
        if (tool == T_Cursor) {
            im = sx::CursorImage(o.cursorType);
            if (im.empty()) return;
            InsertImage(std::move(im), p, false, T_Cursor);  // ShareX: top-left at the click, then dragged
            moving = true;
            dragStart = p;
            SetCapture(hwnd);
            return;
        }
        std::shared_ptr<const sx::Anim> anim;
        std::wstring src;
        bool ok = tool == T_ImageScreen ? CaptureScreenImage(im) : AskImage(tool, im, &anim, &src);
        if (ok) InsertImage(std::move(im), p, true, tool, std::move(anim), src);
    }

    void ReplaceImage(int i) {  // double-click: ImageFileDrawingShape / StickerDrawingShape.OnDoubleClicked
        Image im;
        std::shared_ptr<const sx::Anim> anim;
        std::wstring src;
        if (!AskImage(shapes[i].tool, im, &anim, &src)) return;
        Snapshot();
        Shape& s = shapes[i];
        D2D1_RECT_F b = Norm(s.pts[0], s.pts[1]);
        if (s.tool == T_Sticker) {  // stays centred where it was
            float cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
            b = {std::round(cx - im.w / 2.f), std::round(cy - im.h / 2.f), 0, 0};
        }
        s.pts = {{b.left, b.top}, {b.left + im.w, b.top + im.h}};
        s.initSize = {(float)im.w, (float)im.h};
        s.bmp = std::make_shared<Image>(std::move(im));
        s.anim = std::move(anim);
        s.src = src;
        Invalidate();
    }

    // ---------- cut out (ShareX CutOutTool + ShapeManager.CutOut) ----------
    // The cut is baked into the document bitmap; the history entries (shapes + bitmap) make it undoable.
    bool cutting = false, cutPending = false;
    D2D1_POINT_2F cutA{}, cutB{};
    int cutHover = -1;

    D2D1_RECT_F CutRect() const {  // LimitRectangleToInsideCanvas
        D2D1_RECT_F r = Norm(cutA, cutB);
        float w = (float)img->w, h = (float)img->h;
        return {std::clamp(r.left, 0.f, w), std::clamp(r.top, 0.f, h), std::clamp(r.right, 0.f, w), std::clamp(r.bottom, 0.f, h)};
    }
    bool CutHorizontal() const {
        D2D1_RECT_F r = CutRect();
        return r.right - r.left >= 5 && r.right - r.left > r.bottom - r.top;
    }
    bool CutVertical() const {
        D2D1_RECT_F r = CutRect();
        return r.bottom - r.top >= 5 && r.bottom - r.top >= r.right - r.left;
    }
    bool CutValid() const {
        D2D1_RECT_F r = CutRect();
        bool hz = CutHorizontal(), vt = CutVertical();
        if (!hz && !vt) return false;
        if (hz && r.left <= 0 && r.right >= img->w) return false;
        if (vt && r.top <= 0 && r.bottom >= img->h) return false;
        return true;
    }
    D2D1_RECT_F CutBand() const {  // CutOutRectangle
        D2D1_RECT_F r = CutRect();
        if (CutHorizontal()) return {r.left, 0, r.right, (float)img->h};
        if (CutVertical()) return {0, r.top, (float)img->w, r.bottom};
        return {};
    }

    void CutButtons(D2D1_RECT_F& ok, D2D1_RECT_F& no) const {
        RECT cr;
        GetClientRect(hwnd, &cr);
        float bw = std::round(80 * ui), bh = std::round(40 * ui), off = std::round(15 * ui);
        D2D1_RECT_F r = ToScreen(CutRect()), area{0, region ? 0 : tbH, (float)cr.right, (float)cr.bottom};
        if (CutVertical()) {
            bool below = area.bottom - r.bottom >= bh + 2 * off;
            float top = below ? r.bottom + off : r.top - off - bh;
            float left = r.left + (r.right - r.left) / 2 - (2 * bw + off) / 2, right = left + 2 * bw + off;
            bool ol = left < area.left + off, orr = right >= area.right - off;
            if (ol && !orr) left = area.left + off;
            else if (orr && !ol) left = area.right - off - 2 * bw - off;
            ok = {left, top, left + bw, top + bh};
            no = {left + bw + off, top, left + 2 * bw + off, top + bh};
        } else {
            bool rightSide = area.right - r.right >= bw + 2 * off;
            float left = rightSide ? r.right + off : r.left - off - bw;
            float top = r.top + (r.bottom - r.top) / 2 - (2 * bh + off) / 2, bottom = top + 2 * bh + off;
            bool ot = top < area.top + off, ob = bottom >= area.bottom - off;
            if (ot && !ob) top = area.top + off;
            else if (ob && !ot) top = area.bottom - off - 2 * bh - off;
            ok = {left, top, left + bw, top + bh};
            no = {left, top + bh + off, left + bw, top + 2 * bh + off};
        }
    }

    void RenderCutOut() {
        D2D1_RECT_F band = CutBand();
        if (band.right > band.left && band.bottom > band.top) {
            // ShareX: a 1 px checker of translucent LightGray / Gray over the band.
            static const uint32_t px[4] = {0x80696969u, 0x80404040u, 0x80404040u, 0x80696969u};  // premultiplied
            ComPtr<ID2D1Bitmap> cb;
            ComPtr<ID2D1BitmapBrush> brush;
            rt->CreateBitmap(D2D1::SizeU(2, 2), px, 8,
                             D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &cb);
            rt->CreateBitmapBrush(cb.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                        D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                                  &brush);
            D2D1_RECT_F s = ToScreen(band), c = ToScreen(Crop());
            s = {std::round(std::max(s.left, c.left)), std::round(std::max(s.top, c.top)), std::round(std::min(s.right, c.right)),
                 std::round(std::min(s.bottom, c.bottom))};
            if (s.right > s.left && s.bottom > s.top) rt->FillRectangle(s, brush.Get());
        }
        if (!cutPending) return;
        D2D1_RECT_F ok, no;
        CutButtons(ok, no);
        auto button = [&](D2D1_RECT_F r, D2D1_COLOR_F c, const wchar_t* glyph, bool hot) {
            if (hot) c = {c.r + (1 - c.r) * .15f, c.g + (1 - c.g) * .15f, c.b + (1 - c.b) * .15f, 1};
            g.br->SetColor(c);
            rt->FillRoundedRectangle({r, 3 * ui, 3 * ui}, g.br.Get());
            auto f = Font(L"Segoe UI Symbol", 18 * ui, true);
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g.br->SetColor(kWhite);
            rt->DrawTextW(glyph, 1, f.Get(), r, g.br.Get());
        };
        button(ok, Rgb(34, 139, 34), L"✔", cutHover == 0);   // ForestGreen
        button(no, Rgb(227, 45, 45), L"✖", cutHover == 1);
    }

    // ShapeManager.CollapseAllHorizontal / CollapseAllVertical: shapes after the band move back over it,
    // shapes across it shrink, shapes inside it are deleted.
    void Collapse(bool horiz, float x, float size) {
        float x2 = x + size;
        for (size_t i = shapes.size(); i-- > 0;) {
            Shape& s = shapes[i];
            if (s.tool == T_CutOut) continue;
            D2D1_RECT_F b = HasBoxNodes(s.tool) || s.tool == T_Crop || IsImageShape(s.tool) ? Norm(s.pts[0], s.pts.size() > 1 ? s.pts[1] : s.pts[0])
                                                                                             : Bounds(s);
            if (s.tool == T_Step) {
                float r = StepRadius(s);
                b = {s.pts[0].x - r, s.pts[0].y - r, s.pts[0].x + r, s.pts[0].y + r};
            }
            float lo = horiz ? b.left : b.top, hi = horiz ? b.right : b.bottom, nlo = lo, nhi = hi;
            if (lo < x) {
                if (hi <= x) continue;
                nhi = hi < x2 ? x : hi - size;
            } else if (lo < x2) {
                if (hi <= x2) {
                    shapes.erase(shapes.begin() + i);
                    continue;
                }
                nlo = x;
                nhi = hi - size;
            } else {
                nlo = lo - size;
                nhi = hi - size;
            }
            float k = hi - lo > 0.5f ? (nhi - nlo) / (hi - lo) : 1;
            auto map = [&](float v) { return nlo + (v - lo) * k; };
            for (auto& q : s.pts) (horiz ? q.x : q.y) = map(horiz ? q.x : q.y);
            (horiz ? s.tail.x : s.tail.y) = map(horiz ? s.tail.x : s.tail.y);
            if (k != 1 && IsText(s.tool)) s.autoSize = false;
            if (IsFreehand(s.tool)) BuildGeo(s);
        }
    }

    void ApplyCutOut() {
        cutPending = false;
        if (!CutValid()) return;
        bool horiz = CutHorizontal();
        D2D1_RECT_F band = CutBand();
        int start = (int)std::round(horiz ? band.left : band.top), end = (int)std::round(horiz ? band.right : band.bottom);
        if (end <= start) return;
        CommitText();
        Snapshot();
        auto after = std::make_shared<Image>(sx::CutOutMiddle(*img, horiz, start, end - start, o.cutType, o.cutSize, o.cutBg));
        Collapse(horiz, (float)start, (float)(end - start));
        SetBitmap(after);  // Snapshot above kept the old bitmap, so undo/redo cover the cut
        selected = -1;
        RenumberSteps();
        userZoom = false;
        LayoutAll();
        Invalidate();
    }

    // ---------- region capture drawing (ShareX RegionCaptureForm) ----------
    ComPtr<ID2D1Geometry> RegionGeometry(const Shape& s) const {
        ComPtr<ID2D1Geometry> out;
        if (s.tool == T_RegionFreehand) {
            if (s.pts.size() < 3) return out;
            ComPtr<ID2D1PathGeometry> pg;
            D2D()->CreatePathGeometry(&pg);
            ComPtr<ID2D1GeometrySink> sink;
            pg->Open(&sink);
            sink->BeginFigure(s.pts[0], D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLines(s.pts.data() + 1, (UINT32)s.pts.size() - 1);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            out = pg;
        } else if (s.tool == T_RegionEllipse) {
            D2D1_RECT_F r = RegionBounds(s);
            ComPtr<ID2D1EllipseGeometry> eg;
            D2D()->CreateEllipseGeometry(D2D1::Ellipse({(r.left + r.right) / 2, (r.top + r.bottom) / 2}, (r.right - r.left) / 2,
                                                       (r.bottom - r.top) / 2),
                                         &eg);
            out = eg;
        } else if (o.regionRadius > 0) {  // ShareX RegionCornerRadius
            ComPtr<ID2D1RoundedRectangleGeometry> rg;
            float r = (float)o.regionRadius;
            D2D()->CreateRoundedRectangleGeometry({RegionBounds(s), r, r}, &rg);
            out = rg;
        } else {
            ComPtr<ID2D1RectangleGeometry> rg;
            D2D()->CreateRectangleGeometry(RegionBounds(s), &rg);
            out = rg;
        }
        return out;
    }

    // Pixel bounds; the end point is inclusive, like ShareX's rectangles.
    D2D1_RECT_F RegionBounds(const Shape& s) const {
        D2D1_RECT_F r{1e9f, 1e9f, -1e9f, -1e9f};
        for (auto p : s.pts) r = {std::min(r.left, p.x), std::min(r.top, p.y), std::max(r.right, p.x), std::max(r.bottom, p.y)};
        return {std::floor(r.left), std::floor(r.top), std::floor(r.right) + 1, std::floor(r.bottom) + 1};
    }

    bool RegionValid() const {
        if (!creatingRegion) return false;
        D2D1_RECT_F b = RegionBounds(regionShape);
        return b.right - b.left >= 5 && b.bottom - b.top >= 5;  // ShareX MinimumSize
    }

    // Finished regions (multi region mode) plus the one being drawn.
    std::vector<Shape> LiveRegions() const {
        std::vector<Shape> v = regions;
        if (RegionValid()) v.push_back(regionShape);
        return v;
    }

    // Union of regions (ShareX regionFillPath, FillMode.Winding).
    ComPtr<ID2D1Geometry> RegionsGeometry(const std::vector<Shape>& list) const {
        std::vector<ComPtr<ID2D1Geometry>> parts;
        for (const Shape& s : list)
            if (auto gm = RegionGeometry(s)) parts.push_back(gm);
        if (parts.empty()) return nullptr;
        if (parts.size() == 1) return parts[0];
        std::vector<ID2D1Geometry*> raw;
        for (auto& p : parts) raw.push_back(p.Get());
        ComPtr<ID2D1GeometryGroup> grp;
        D2D()->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), (UINT32)raw.size(), &grp);
        return grp;
    }

    void RenderRegionBackground() {
        D2D1_RECT_F all{0, 0, (float)img->w, (float)img->h};
        rt->DrawBitmap(g.img.Get(), all, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        if (o.dimming && o.dimStrength > 0) {
            g.br->SetColor(Rgb(0, 0, 0, (int)std::round(255 * o.dimStrength / 100.f)));
            auto live = LiveRegions();
            if (live.size() == 1 && live[0].tool == T_RegionRect && o.regionRadius <= 0) {
                // Fast path: 4 dim strips around the rectangle — zero offscreen PushLayer and zero 2nd full-screen blit.
                D2D1_RECT_F r = RegionBounds(live[0]);
                r.left = std::clamp(r.left, 0.f, all.right);
                r.top = std::clamp(r.top, 0.f, all.bottom);
                r.right = std::clamp(r.right, r.left, all.right);
                r.bottom = std::clamp(r.bottom, r.top, all.bottom);
                if (r.top > 0) rt->FillRectangle({0, 0, all.right, r.top}, g.br.Get());
                if (r.bottom < all.bottom) rt->FillRectangle({0, r.bottom, all.right, all.bottom}, g.br.Get());
                if (r.left > 0 && r.bottom > r.top) rt->FillRectangle({0, r.top, r.left, r.bottom}, g.br.Get());
                if (r.right < all.right && r.bottom > r.top) rt->FillRectangle({r.right, r.top, all.right, r.bottom}, g.br.Get());
            } else {
                rt->FillRectangle(all, g.br.Get());
                if (!live.empty()) {  // regions stay undimmed, clipped to tight bounding box
                    if (auto geo = RegionsGeometry(live)) {
                        D2D1_RECT_F b{all.right, all.bottom, 0, 0};
                        for (const Shape& s : live) {
                            D2D1_RECT_F rb = RegionBounds(s);
                            b.left = std::min(b.left, rb.left);
                            b.top = std::min(b.top, rb.top);
                            b.right = std::max(b.right, rb.right);
                            b.bottom = std::max(b.bottom, rb.bottom);
                        }
                        b.left = std::clamp(std::floor(b.left), 0.f, all.right);
                        b.top = std::clamp(std::floor(b.top), 0.f, all.bottom);
                        b.right = std::clamp(std::ceil(b.right), b.left, all.right);
                        b.bottom = std::clamp(std::ceil(b.bottom), b.top, all.bottom);
                        if (b.right > b.left && b.bottom > b.top) {
                            rt->PushLayer(D2D1::LayerParameters(b, geo.Get(), D2D1_ANTIALIAS_MODE_ALIASED), nullptr);
                            rt->DrawBitmap(g.img.Get(), b, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, b);
                            rt->PopLayer();
                        }
                    }
                }
            }
        }
        DrawDocument(rt.Get(), g, true, false);
    }

    // ShareX DrawInfoText: translucent theme box with a two-tone border and shadowed text.
    void InfoBox(const std::wstring& text, float x, float y, float padding, D2D1_RECT_F* outRect = nullptr, bool measureOnly = false) {
        ComPtr<IDWriteTextLayout> lay;
        DW()->CreateTextLayout(text.c_str(), (UINT32)text.size(), infoFont.Get(), 4000, 400, &lay);
        DWRITE_TEXT_METRICS m;
        lay->GetMetrics(&m);
        D2D1_RECT_F r{x, y, std::round(x + m.width + 2 * padding), std::round(y + m.height + 2 * padding)};
        if (outRect) *outRect = r;
        if (measureOnly) return;
        g.br->SetColor(Rgb(39, 39, 39, 200));
        rt->FillRectangle({r.left + 2, r.top + 2, r.right - 2, r.bottom - 2}, g.br.Get());
        g.br->SetColor(Rgb(44, 44, 44, 200));
        rt->DrawRectangle({r.left + 1.5f, r.top + 1.5f, r.right - 1.5f, r.bottom - 1.5f}, g.br.Get(), 1);
        g.br->SetColor(Rgb(31, 31, 31, 200));
        rt->DrawRectangle({r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f}, g.br.Get(), 1);
        g.br->SetColor(Rgb(0, 0, 0));
        rt->DrawTextLayout({x + padding + 1, y + padding + 1}, lay.Get(), g.br.Get());
        g.br->SetColor(kWhite);
        rt->DrawTextLayout({x + padding, y + padding}, lay.Get(), g.br.Get());
    }

    void AreaText(const RECT& a) {
        wchar_t buf[96];
        swprintf_s(buf, L"X: %d Y: %d W: %d H: %d", a.left, a.top, a.right - a.left, a.bottom - a.top);
        float offset = 6, padding = 3;
        D2D1_RECT_F r;
        InfoBox(buf, 0, 0, padding, &r, true);
        float tw = r.right - r.left - 2 * padding, th = r.bottom - r.top - 2 * padding;
        float tx, ty;
        if (a.top - offset - th - padding * 2 < 0) {
            tx = a.left + offset + padding;
            ty = a.top + offset + padding;
        } else {
            tx = a.left + padding;
            ty = a.top - offset - padding - th;
        }
        if (tx + tw + padding >= img->w) tx = img->w - tw - padding;
        InfoBox(buf, tx - padding, ty - padding, padding);
    }

    // Black line under a white 5/5 dash; the hover frame's dashes march like ShareX's.
    void DashedRect(D2D1_RECT_F r, bool animated) {
        r = {r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f};
        g.br->SetColor(Rgb(0, 0, 0));
        rt->DrawRectangle(r, g.br.Get(), 1);
        g.br->SetColor(kWhite);
        rt->DrawRectangle(r, g.br.Get(), 1, AntsStroke(animated).Get());
    }

    // ShareX borderDotPen: white 5/5 dashes; they march at 15 px/s while EnableAnimations is on.
    ComPtr<ID2D1StrokeStyle> AntsStroke(bool animated) const {
        float offset = animated && o.animations ? (GetTickCount() - startTick) / 1000.f * -15 : 0;
        ComPtr<ID2D1StrokeStyle> ss;
        float dashes[] = {5, 5};
        D2D()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                             D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, offset),
                                 dashes, 2, &ss);
        return ss;
    }

    // ShareX DrawCrosshair: black + marching white dashes from 5 px off the cursor to the surface edges.
    void DrawCrosshair() {
        if (cursor.x <= -10000) return;
        float mx = cursor.x + .5f, my = cursor.y + .5f, off = 5, W = (float)img->w - 1, H = (float)img->h - 1;
        auto ants = AntsStroke(true);
        auto line = [&](D2D1_POINT_2F a, D2D1_POINT_2F b) {
            g.br->SetColor(Rgb(0, 0, 0));
            rt->DrawLine(a, b, g.br.Get(), 1);
            g.br->SetColor(kWhite);
            rt->DrawLine(a, b, g.br.Get(), 1, ants.Get());
        };
        if (mx - off > 10) line({mx - off, my}, {0, my});
        if (W - (mx + off) > 10) line({mx + off, my}, {W, my});
        if (my - off > 10) line({mx, my - off}, {mx, 0});
        if (H - (my + off) > 10) line({mx, my + off}, {mx, H});
    }

    // ShareX DrawFPS: big white text with a black shadow, 10 px into the active monitor.
    void DrawFps() {
        if (!fpsFont) fpsFont = Font(L"Verdana", 21.33f * ui, true);
        std::wstring t = std::to_wstring(fps);
        float x = (float)mon.left + 10, y = (float)mon.top + 10;
        g.br->SetColor(Rgb(0, 0, 0));
        rt->DrawTextW(t.c_str(), (UINT32)t.size(), fpsFont.Get(), {x, y + 1, x + 400, y + 200}, g.br.Get());
        g.br->SetColor(kWhite);
        rt->DrawTextW(t.c_str(), (UINT32)t.size(), fpsFont.Get(), {x, y, x + 400, y + 200}, g.br.Get());
    }

    // Region outline: rectangles get the two-tone dashed frame, other shapes a dashed path.
    void RegionOutline(const Shape& s, bool animated) {
        D2D1_RECT_F b = RegionBounds(s);
        if (s.tool == T_RegionRect && o.regionRadius <= 0) {
            DashedRect(b, animated);
        } else if (auto geo = RegionGeometry(s)) {
            g.br->SetColor(Rgb(0, 0, 0));
            rt->DrawGeometry(geo.Get(), g.br.Get(), 1);
            g.br->SetColor(kWhite);
            rt->DrawGeometry(geo.Get(), g.br.Get(), 1, AntsStroke(animated).Get());
        }
    }

    // Resize handles of the selected region (ShareX resize nodes; light ones are smaller).
    static constexpr int RD_L = 1, RD_T = 2, RD_R = 4, RD_B = 8, RD_Move = 16;
    std::vector<std::pair<int, D2D1_POINT_2F>> RegionNodes(const Shape& s) const {
        std::vector<std::pair<int, D2D1_POINT_2F>> v;
        if (s.tool == T_RegionFreehand) return v;
        D2D1_RECT_F b = RegionBounds(s);
        float cx = std::round((b.left + b.right) / 2), cy = std::round((b.top + b.bottom) / 2), r = b.right - 1, btm = b.bottom - 1;
        v = {{RD_L | RD_T, {b.left, b.top}}, {RD_T, {cx, b.top}}, {RD_R | RD_T, {r, b.top}}, {RD_R, {r, cy}},
             {RD_R | RD_B, {r, btm}}, {RD_B, {cx, btm}}, {RD_L | RD_B, {b.left, btm}}, {RD_L, {b.left, cy}}};
        return v;
    }
    float NodeSize() const { return std::round((o.lightNodes ? 7 : 11) * ui); }

    void DrawRegionNodes(const Shape& s) {
        float n = NodeSize();
        for (auto& nd : RegionNodes(s)) {
            D2D1_RECT_F q{std::round(nd.second.x - n / 2) + .5f, std::round(nd.second.y - n / 2) + .5f, 0, 0};
            q.right = q.left + n - 1;
            q.bottom = q.top + n - 1;
            g.br->SetColor(kWhite);
            rt->FillRectangle(q, g.br.Get());
            g.br->SetColor(Rgb(0, 0, 0));
            rt->DrawRectangle(q, g.br.Get(), 1);
        }
    }

    // Which finished region (and which part of it) is under the point: index, RD_* mode.
    std::pair<int, int> RegionHit(POINT p) const {
        float n = NodeSize() / 2 + 2;
        if (selRegion >= 0 && selRegion < (int)regions.size())
            for (auto& nd : RegionNodes(regions[selRegion]))
                if (std::abs(p.x - nd.second.x) <= n && std::abs(p.y - nd.second.y) <= n) return {selRegion, nd.first};
        for (int i = (int)regions.size() - 1; i >= 0; --i) {
            D2D1_RECT_F b = RegionBounds(regions[i]);
            if (p.x >= b.left && p.x < b.right && p.y >= b.top && p.y < b.bottom) return {i, RD_Move};
        }
        return {-1, 0};
    }

    void DragRegion(D2D1_POINT_2F delta) {
        if (selRegion < 0 || selRegion >= (int)regions.size()) return;
        Shape& s = regions[selRegion];
        if (regionDrag == RD_Move) {
            for (auto& q : s.pts) q.x += delta.x, q.y += delta.y;
            return;
        }
        if (s.pts.size() != 2) return;
        D2D1_RECT_F r = Norm(s.pts[0], s.pts[1]);  // inclusive end point
        if (regionDrag & RD_L) r.left += delta.x;
        if (regionDrag & RD_R) r.right += delta.x;
        if (regionDrag & RD_T) r.top += delta.y;
        if (regionDrag & RD_B) r.bottom += delta.y;
        if (r.left > r.right) std::swap(r.left, r.right), regionDrag ^= RD_L | RD_R;  // dragged past the opposite edge
        if (r.top > r.bottom) std::swap(r.top, r.bottom), regionDrag ^= RD_T | RD_B;
        s.pts = {{r.left, r.top}, {r.right, r.bottom}};
    }

    bool CursorOverBar() const {
        if (buttons.empty()) return false;
        D2D1_RECT_F b = BarRect();
        return cursor.x >= b.left && cursor.x < b.right && cursor.y >= b.top && cursor.y < b.bottom;
    }

    void RenderRegionOverlay() {
        if (creatingRegion && regionShape.tool != T_RegionFreehand && KeyDown(VK_MENU)) {
            g.br->SetColor(Rgb(255, 0, 0, 200));
            D2D1_POINT_2F a = regionShape.pts[0], b = regionShape.pts[1];
            for (auto& sz : kSnapSizes) {
                D2D1_POINT_2F e2 = SnapEnd(a, b, sz[0], sz[1]);
                D2D1_RECT_F r{std::min(a.x, e2.x), std::min(a.y, e2.y), std::max(a.x, e2.x) + 1, std::max(a.y, e2.y) + 1};
                rt->DrawRectangle({r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f}, g.br.Get(), 1);
            }
        }
        auto info = [&](const Shape& s) {
            D2D1_RECT_F b = RegionBounds(s);
            if (o.showInfo) AreaText({(LONG)b.left, (LONG)b.top, (LONG)b.right, (LONG)b.bottom});
        };
        for (const Shape& s : regions) RegionOutline(s, false);
        if (selRegion >= 0 && selRegion < (int)regions.size() && !creatingRegion) DrawRegionNodes(regions[selRegion]);
        if (RegionValid()) {
            RegionOutline(regionShape, true);
        } else if (hasHover && IsRegion(tool) && !creatingRegion && !regionDrag) {
            DashedRect({(float)hover.left, (float)hover.top, (float)hover.right, (float)hover.bottom}, true);
            if (o.showInfo) AreaText(hover);
        }
        for (const Shape& s : regions) info(s);
        if (RegionValid()) info(regionShape);
        if ((o.showMagnifier || o.showInfo) && !CursorOverBar() && cursor.x > -10000) CursorGraphics();
        if (o.crosshair) DrawCrosshair();
        if (o.showFps) DrawFps();
    }

    // Magnifier (ShareX Magnifier(): nearest-neighbour pixels, grid, light-blue crosshair) + "X: Y:" box.
    void CursorGraphics() {
        int n = std::clamp(o.magnifierCount | 1, 3, 35), ps = std::clamp(o.magnifierSize, 3, 30);
        float magW = (float)(n * ps - 1), totalW = 0, totalH = 0, infoPos = 0;
        if (o.showMagnifier) totalW = totalH = magW;
        POINT sp{cursor.x + vs.left, cursor.y + vs.top};
        wchar_t info[64];
        swprintf_s(info, L"X: %d Y: %d", sp.x, sp.y);
        D2D1_RECT_F ir{};
        if (o.showInfo) {
            if (o.showMagnifier) totalH += 10;
            infoPos = totalH;
            InfoBox(info, 0, 0, 3, &ir, true);
            totalW = std::max(totalW, ir.right - ir.left);
            totalH += ir.bottom - ir.top;
        }
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(sp, MONITOR_DEFAULTTONEAREST), &mi);
        float right = (float)(mi.rcMonitor.right - vs.left), bottom = (float)(mi.rcMonitor.bottom - vs.top);
        float x = (float)cursor.x + 10, y = (float)cursor.y + 10;
        if (x + totalW > right) x = cursor.x - 10 - totalW;
        if (y + totalH > bottom) y = cursor.y - 10 - totalH;
        if (o.showMagnifier) {
            D2D1_RECT_F m{x, y, x + magW, y + magW};
            ComPtr<ID2D1EllipseGeometry> circle;
            if (o.squareMagnifier) {
                rt->PushAxisAlignedClip(m, D2D1_ANTIALIAS_MODE_ALIASED);
            } else {
                D2D()->CreateEllipseGeometry(D2D1::Ellipse({x + magW / 2, y + magW / 2}, magW / 2, magW / 2), &circle);
                rt->PushLayer(D2D1::LayerParameters(m, circle.Get()), nullptr);
            }
            g.br->SetColor(kCanvas);
            rt->FillRectangle(m, g.br.Get());
            D2D1_RECT_F src{(float)(cursor.x - n / 2), (float)(cursor.y - n / 2), (float)(cursor.x - n / 2 + n), (float)(cursor.y - n / 2 + n)};
            // Clamp the source to the image so edge pixels are not stretched.
            D2D1_RECT_F cs{std::max(0.f, src.left), std::max(0.f, src.top), std::min((float)img->w, src.right),
                           std::min((float)img->h, src.bottom)};
            if (cs.right > cs.left && cs.bottom > cs.top) {
                D2D1_RECT_F cd{x + (cs.left - src.left) * ps, y + (cs.top - src.top) * ps, x + (cs.right - src.left) * ps,
                               y + (cs.bottom - src.top) * ps};
                rt->DrawBitmap(g.img.Get(), cd, 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, cs);
            }
            float W = (float)(n * ps), c0 = (W - ps) / 2, c1 = (W + ps) / 2;
            g.br->SetColor(Rgb(173, 216, 230, 125));
            rt->FillRectangle({x, y + c0, x + c0, y + c0 + ps}, g.br.Get());
            rt->FillRectangle({x + c1, y + c0, x + W, y + c0 + ps}, g.br.Get());
            rt->FillRectangle({x + c0, y, x + c0 + ps, y + c0}, g.br.Get());
            rt->FillRectangle({x + c0, y + c1, x + c0 + ps, y + W}, g.br.Get());
            g.br->SetColor(Rgb(0, 0, 0, 75));
            for (int k = 1; k < n; ++k) {
                float l = (float)(k * ps - 1);
                rt->FillRectangle({x + l, y, x + l + 1, y + W}, g.br.Get());
                rt->FillRectangle({x, y + l, x + W, y + l + 1}, g.br.Get());
            }
            g.br->SetColor(Rgb(0, 0, 0));
            rt->DrawRectangle({x + c0 - .5f, y + c0 - .5f, x + c0 + ps - .5f, y + c0 + ps - .5f}, g.br.Get(), 1);
            if (ps >= 6) {
                g.br->SetColor(kWhite);
                rt->DrawRectangle({x + c0 + .5f, y + c0 + .5f, x + c0 + ps - 1.5f, y + c0 + ps - 1.5f}, g.br.Get(), 1);
            }
            if (o.squareMagnifier) {
                rt->PopAxisAlignedClip();
                g.br->SetColor(kWhite);
                rt->DrawRectangle({x - .5f, y - .5f, x + magW + .5f, y + magW + .5f}, g.br.Get(), 1);
                g.br->SetColor(Rgb(0, 0, 0));
                rt->DrawRectangle({x + .5f, y + .5f, x + magW - .5f, y + magW - .5f}, g.br.Get(), 1);
            } else {
                rt->PopLayer();
                g.br->SetColor(kWhite);
                rt->DrawEllipse(D2D1::Ellipse({x + magW / 2, y + magW / 2}, magW / 2 + .5f, magW / 2 + .5f), g.br.Get(), 1);
                g.br->SetColor(Rgb(0, 0, 0));
                rt->DrawEllipse(D2D1::Ellipse({x + magW / 2, y + magW / 2}, magW / 2 - .5f, magW / 2 - .5f), g.br.Get(), 1);
            }
        }
        if (o.showInfo) {
            float w = ir.right - ir.left;
            InfoBox(info, std::round(x + totalW / 2 - w / 2), y + infoPos, 3);
        }
    }

    void UpdateHover() {
        hasHover = false;
        if (o.fixedSize) {  // ShareX IsFixedSize: a FixedSize rectangle centred on the cursor follows it
            if (cursor.x <= -10000) return;
            hover = {cursor.x - o.fixedW / 2, cursor.y - o.fixedH / 2, 0, 0};
            hover.right = hover.left + o.fixedW;
            hover.bottom = hover.top + o.fixedH;
            hasHover = true;
            return;
        }
        if (!o.detectWindows) return;
        POINT sp{cursor.x + vs.left, cursor.y + vs.top};
        for (const WindowRect& w : wins) {
            if (!PtInRect(&w.rect, sp)) continue;
            RECT r = w.rect, c;
            if (o.detectControls && ControlRectAt(w.hwnd, sp, c) && IntersectRect(&c, &c, &w.rect)) r = c;
            if (!IntersectRect(&r, &r, &vs)) continue;
            if (r.right - r.left < 16 || r.bottom - r.top < 16) continue;
            OffsetRect(&r, -vs.left, -vs.top);
            hover = r;
            hasHover = true;
            return;
        }
    }

    // QuickCrop: the region is captured on mouse up, annotations included.
    void FinishRegion(const Shape& rs) { FinishRegions({rs}); }

    // Several regions (multi region mode): their bounding box, with everything outside the regions
    // transparent (ShareX ApplyRegionPathToImage); a single rectangle is a plain crop.
    void FinishRegions(const std::vector<Shape>& list) {
        if (list.empty()) return;
        D2D1_RECT_F b{1e9f, 1e9f, -1e9f, -1e9f};
        for (const Shape& s : list) {
            D2D1_RECT_F r = RegionBounds(s);
            b = {std::min(b.left, r.left), std::min(b.top, r.top), std::max(b.right, r.right), std::max(b.bottom, r.bottom)};
        }
        b = {std::max(b.left, 0.f), std::max(b.top, 0.f), std::min(b.right, (float)img->w), std::min(b.bottom, (float)img->h)};
        if (b.right - b.left < 1 || b.bottom - b.top < 1) return;
        CommitText();
        ComPtr<ID2D1Geometry> mask;
        if (list.size() > 1 || list[0].tool != T_RegionRect || o.regionRadius > 0) mask = RegionsGeometry(list);
        result->image = Export(&b, mask.Get());
        result->rect = {(LONG)b.left + vs.left, (LONG)b.top + vs.top, (LONG)b.right + vs.left, (LONG)b.bottom + vs.top};
        result->ok = !result->image.empty();
        result->done = true;
    }

    void CancelRegion() {
        result->ok = false;
        result->done = true;
    }

    // A finished selection: captured at once (QuickCrop) or kept as one of several regions.
    void RegionSelected(const Shape& rs) {
        if (o.quickCrop) {
            FinishRegion(rs);
            return;
        }
        regions.push_back(rs);
        selRegion = (int)regions.size() - 1;
    }

    // ShareX "Capture regions" (Enter / double click).
    bool CaptureRegions() {
        std::vector<Shape> list = LiveRegions();
        if (list.empty()) return false;
        creatingRegion = false;
        FinishRegions(list);
        return true;
    }

    void CaptureArea(RECT target) {  // virtual-screen pixels
        Shape rs{T_RegionRect};
        rs.pts = {{(float)(target.left - vs.left), (float)(target.top - vs.top)},
                  {(float)(target.right - vs.left - 1), (float)(target.bottom - vs.top - 1)}};
        FinishRegion(rs);
    }

    RECT ActiveMonitorRect() const {
        POINT p{cursor.x + vs.left, cursor.y + vs.top};
        if (cursor.x <= -10000) GetCursorPos(&p);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
        return mi.rcMonitor;
    }

    // ShareX RegionCaptureAction for the extra mouse buttons ([region] RegionCaptureActionX1Click / X2Click).
    void RunRegionAction(const std::wstring& a, POINT at) {
        if (a == L"CancelCapture") {
            CancelRegion();
        } else if (a == L"RemoveShapeCancelCapture" || a == L"RemoveShape") {
            if (!RemoveShapeAt(at) && a == L"RemoveShapeCancelCapture") CancelRegion();
        } else if (a == L"SwapToolType") {
            SwapToolType();
        } else if (a == L"CaptureFullscreen") {
            CaptureArea(vs);
        } else if (a == L"CaptureActiveMonitor") {
            CaptureArea(ActiveMonitorRect());
        } else if (a == L"CaptureLastRegion") {
            RECT r;
            if (LoadLastRegion(r)) CaptureArea(r);
        }
        Invalidate();
    }

    // Right click: drop the region being drawn, else the region or annotation under the cursor.
    bool RemoveShapeAt(POINT p) {
        if (creatingRegion) {
            ReleaseCapture();
            creatingRegion = false;
            return true;
        }
        if (int hit = HitShape({(float)p.x, (float)p.y}); hit >= 0) {
            CommitText();
            selected = hit;
            DeleteSelected();
            return true;
        }
        if (int ri = RegionHit(p).first; ri >= 0) {
            regions.erase(regions.begin() + ri);
            selRegion = regions.empty() ? -1 : (int)regions.size() - 1;
            return true;
        }
        return false;
    }

    void SwapToolType() {  // ShareX middle click / Tab
        CommitText();
        if (IsRegion(tool)) {
            o.lastRegionTool = tool;
            SetTool((Tool)o.lastAnnotationTool);
        } else {
            if (tool != T_Select) o.lastAnnotationTool = tool;
            SetTool((Tool)o.lastRegionTool);
        }
    }

    void CaptureMenu(const Button& b) {
        HMENU m = CreatePopupMenu();
        RECT last;
        bool haveLast = LoadLastRegion(last);
        AppendMenuW(m, MF_STRING | (regions.empty() ? MF_GRAYED : 0), 3, Tr(L"Захват областей\tEnter", L"Capture regions\tEnter"));
        if (haveLast) AppendMenuW(m, MF_STRING, 4, Tr(L"Захват последней области", L"Capture last region"));
        AppendMenuW(m, MF_STRING, 1, Tr(L"Захват всего экрана\tSpace", L"Capture fullscreen\tSpace"));
        AppendMenuW(m, MF_STRING, 2, Tr(L"Захват активного монитора", L"Capture active monitor"));
        HMENU mons = CreatePopupMenu();
        std::vector<RECT> rects;
        struct Ctx { HMENU m; std::vector<RECT>* rects; } ctx{mons, &rects};
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM lp) -> BOOL {
            auto* c = (Ctx*)lp;
            wchar_t t[64];
            swprintf_s(t, L"%dx%d", r->right - r->left, r->bottom - r->top);
            c->rects->push_back(*r);
            AppendMenuW(c->m, MF_STRING, 100 + c->rects->size() - 1, t);
            return TRUE;
        }, (LPARAM)&ctx);
        AppendMenuW(m, MF_POPUP, (UINT_PTR)mons, Tr(L"Захват монитора", L"Capture monitor"));
        POINT p = MenuPos(b);
        int id = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        RECT target{};
        if (id == 3) {
            CaptureRegions();
            return;
        } else if (id == 4) {
            target = last;
        } else if (id == 1) {
            target = vs;
        } else if (id == 2) {
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromPoint({mon.left + vs.left + 1, mon.top + vs.top + 1}, MONITOR_DEFAULTTONEAREST), &mi);
            target = mi.rcMonitor;
        } else if (id >= 100 && id - 100 < (int)rects.size()) {
            target = rects[id - 100];
        } else {
            return;
        }
        CaptureArea(target);
    }

    void RegionOptionsMenu(const Button& b) {
        HMENU m = CreatePopupMenu();
        auto item = [&](int id, const wchar_t* t, bool on) { AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), id, t); };
        // Same items and order as ShareX's region capture Options menu (numeric fields become submenus).
        item(6, Tr(L"Режим нескольких областей", L"Multi region mode"), !o.quickCrop);
        item(1, Tr(L"Показывать позицию и размер", L"Show position and size info"), o.showInfo);
        item(2, Tr(L"Показывать лупу", L"Show magnifier"), o.showMagnifier);
        item(3, Tr(L"Квадратная лупа", L"Square shape magnifier"), o.squareMagnifier);
        HMENU cnt = CreatePopupMenu(), sz = CreatePopupMenu(), fixed = CreatePopupMenu();
        for (int v : {7, 9, 11, 15, 21, 25, 33})
            AppendMenuW(cnt, MF_STRING | (o.magnifierCount == v ? MF_CHECKED : 0), 100 + v, std::to_wstring(v).c_str());
        for (int v : {5, 8, 10, 12, 15, 20})
            AppendMenuW(sz, MF_STRING | (o.magnifierSize == v ? MF_CHECKED : 0), 200 + v, std::to_wstring(v).c_str());
        AppendMenuW(m, MF_POPUP, (UINT_PTR)cnt, Tr(L"Пикселей в лупе", L"Magnifier pixel count"));
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sz, Tr(L"Размер пикселя лупы", L"Magnifier pixel size"));
        item(7, Tr(L"Перекрестие во весь экран", L"Show screen wide crosshair"), o.crosshair);
        item(8, Tr(L"Облегчённые маркеры размера", L"Use light resize nodes"), o.lightNodes);
        item(9, Tr(L"Анимация", L"Enable animations"), o.animations);
        item(10, Tr(L"Область фиксированного размера", L"Fixed size region mode"), o.fixedSize);
        static const int kFixed[][2] = {{100, 100}, {250, 250}, {400, 300}, {640, 360}, {800, 600}, {1280, 720}, {1920, 1080}};
        bool custom = true;
        for (int i = 0; i < (int)std::size(kFixed); ++i) {
            bool on = o.fixedW == kFixed[i][0] && o.fixedH == kFixed[i][1];
            custom &= !on;
            AppendMenuW(fixed, MF_STRING | (on ? MF_CHECKED : 0), 300 + i,
                        (std::to_wstring(kFixed[i][0]) + L" x " + std::to_wstring(kFixed[i][1])).c_str());
        }
        if (custom)
            AppendMenuW(fixed, MF_STRING | MF_CHECKED, 0, (std::to_wstring(o.fixedW) + L" x " + std::to_wstring(o.fixedH)).c_str());
        AppendMenuW(m, MF_POPUP, (UINT_PTR)fixed,
                    (std::wstring(Tr(L"Фиксированный размер: ", L"Fixed size: ")) + std::to_wstring(o.fixedW) + L" x " +
                     std::to_wstring(o.fixedH)).c_str());
        item(11, Tr(L"Показывать FPS", L"Show FPS"), o.showFps);
        item(12, Tr(L"Переключаться на инструмент фигуры после выбора", L"Switch to drawing tool after shape selection"),
             o.switchToDrawing);
        item(13, Tr(L"Переключаться на выделение после рисования", L"Switch to selection tool after shape drawing"),
             o.switchToSelect);
        item(14, Tr(L"Закрепить панель", L"Lock menu"), o.menuLocked);
        item(15, Tr(L"Запоминать положение панели", L"Remember menu state"), o.rememberMenu);
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        item(4, Tr(L"Затемнять фон", L"Dim background"), o.dimming);
        item(5, Tr(L"Определять окна", L"Detect windows"), o.detectWindows);
        item(16, Tr(L"Определять элементы внутри окон", L"Detect controls inside windows"), o.detectControls);
        POINT p = MenuPos(b);
        int id = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (id == 1) o.showInfo = !o.showInfo;
        else if (id == 2) o.showMagnifier = !o.showMagnifier;
        else if (id == 3) o.squareMagnifier = !o.squareMagnifier;
        else if (id == 4) o.dimming = !o.dimming;
        else if (id == 5) o.detectWindows = !o.detectWindows;
        else if (id == 16) o.detectControls = !o.detectControls;
        else if (id == 6) {
            o.quickCrop = !o.quickCrop;
            if (o.quickCrop) regions.clear(), selRegion = -1;
        } else if (id == 7) o.crosshair = !o.crosshair;
        else if (id == 8) o.lightNodes = !o.lightNodes;
        else if (id == 9) o.animations = !o.animations;
        else if (id == 10) o.fixedSize = !o.fixedSize;
        else if (id == 11) o.showFps = !o.showFps, fpsTick = GetTickCount(), fpsFrames = 0;
        else if (id == 12) o.switchToDrawing = !o.switchToDrawing;
        else if (id == 13) o.switchToSelect = !o.switchToSelect;
        else if (id == 14) o.menuLocked = !o.menuLocked;
        else if (id == 15) o.rememberMenu = !o.rememberMenu;
        else if (id >= 300 && id < 300 + (int)std::size(kFixed)) o.fixedW = kFixed[id - 300][0], o.fixedH = kFixed[id - 300][1];
        else if (id > 100 && id < 200) o.magnifierCount = id - 100;
        else if (id > 200 && id < 300) o.magnifierSize = id - 200;
        if (id) {
            o.Save();
            LayoutToolbar();
            UpdateHover();
        }
        Invalidate();
    }

    // ---------- ShareX creation modifiers ----------
    D2D1_POINT_2F lastMouse{};
    D2D1_POINT_2F lastImagePt{};

    static bool KeyDown(int vk) { return GetKeyState(vk) < 0; }

    static D2D1_POINT_2F SnapToDegree(D2D1_POINT_2F a, D2D1_POINT_2F b, float degree, float startDegree) {
        const float rad = 3.14159265f / 180;
        float angle = std::atan2(b.y - a.y, b.x - a.x), start = startDegree * rad, snap = degree * rad;
        float na = std::round((angle + start) / snap) * snap - start;
        float dist = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        return {std::round(a.x + std::cos(na) * dist), std::round(a.y + std::sin(na) * dist)};
    }

    static constexpr int kSnapSizes[5][2] = {{426, 240}, {640, 360}, {854, 480}, {1280, 720}, {1920, 1080}};

    // Alt: stick to the nearest preset size within 30 px (ShareX SnapSizes / SnapDistance).
    D2D1_POINT_2F SnapToSize(D2D1_POINT_2F a, D2D1_POINT_2F b) const {
        float w = std::abs(b.x - a.x) + 1, h = std::abs(b.y - a.y) + 1, best = 30;
        int found = -1;
        for (int i = 0; i < 5; ++i) {
            float d = std::hypot(w - kSnapSizes[i][0], h - kSnapSizes[i][1]);
            if (d > 0 && d < best) best = d, found = i;
        }
        if (found < 0) return b;
        return SnapEnd(a, b, kSnapSizes[found][0], kSnapSizes[found][1]);
    }

    static D2D1_POINT_2F SnapEnd(D2D1_POINT_2F a, D2D1_POINT_2F b, int w, int h) {
        return {b.x >= a.x ? a.x + w - 1 : a.x - w + 1, b.y >= a.y ? a.y + h - 1 : a.y - h + 1};
    }

    // While a shape is being created: Ctrl moves the whole shape, Shift snaps the angle
    // (lines 45 deg, boxes to a square), Alt snaps to preset sizes.
    void UpdateCreating(std::vector<D2D1_POINT_2F>& pts, Tool t, D2D1_POINT_2F p, D2D1_POINT_2F delta) {
        if (KeyDown(VK_CONTROL)) {
            if (pts.size() == 2) {
                pts[0].x += delta.x;
                pts[0].y += delta.y;
            } else {
                for (auto& q : pts) q.x += delta.x, q.y += delta.y;
                return;
            }
        }
        if (pts.size() != 2) return;
        if (KeyDown(VK_SHIFT)) {
            bool line = t == T_Line || t == T_Arrow;
            p = SnapToDegree(pts[0], p, line ? 45.f : 90.f, line ? 0.f : 45.f);
        } else if (KeyDown(VK_MENU) && t != T_Line && t != T_Arrow) {
            p = SnapToSize(pts[0], p);
        }
        pts[1] = p;
    }

    // Arrow keys (ShareX): move the cursor 1 px (Shift: 10) while creating or with nothing selected;
    // otherwise move the selected shape, Ctrl/Alt resize it from the end/start corner.
    bool ArrowKey(WPARAM vk) {
        int speed = KeyDown(VK_SHIFT) ? 10 : 1;
        int dx = vk == VK_LEFT ? -speed : vk == VK_RIGHT ? speed : 0, dy = vk == VK_UP ? -speed : vk == VK_DOWN ? speed : 0;
        if (!dx && !dy) return false;
        bool creating = creatingRegion || drawing || cropping;
        if (creating || selected < 0) {
            POINT c;
            GetCursorPos(&c);
            SetCursorPos(c.x + dx, c.y + dy);  // produces the WM_MOUSEMOVE that updates the shape
            return true;
        }
        Shape& sh = shapes[selected];
        Snapshot();
        if ((KeyDown(VK_CONTROL) || KeyDown(VK_MENU)) && sh.pts.size() >= 2 && !IsFreehand(sh.tool)) {
            auto& q = KeyDown(VK_CONTROL) ? sh.pts.back() : sh.pts[0];
            q.x += dx, q.y += dy;
            if (IsText(sh.tool)) sh.autoSize = false;
            AutoCenterPoints(sh);
        } else {
            MoveShape(sh, (float)dx, (float)dy);
        }
        Invalidate();
        return true;
    }

    void Invalidate() {
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);  // no window in EditorTest
    }

    void UpdateTitle() {
        std::wstring t = L"Zipshot — ";
        t += path.empty() ? std::wstring(Tr(L"без имени", L"untitled")) : std::wstring(PathFindFileNameW(path.c_str()));
        if (dirty) t += L" *";
        SetWindowTextW(hwnd, t.c_str());
    }

    // ---------- history ----------
    void Snapshot() {
        undo.push_back({shapes, img});
        redo.clear();
        dirty = true;
        UpdateTitle();
    }

    void Restore(std::vector<HistoryEntry>& from, std::vector<HistoryEntry>& to) {
        CommitText();
        if (from.empty()) return;
        D2D1_RECT_F before = Crop();
        to.push_back({std::move(shapes), img});
        HistoryEntry e = std::move(from.back());
        from.pop_back();
        shapes = std::move(e.shapes);
        if (e.image && e.image != img) SetBitmap(e.image);
        cutPending = cutting = false;
        dragNode = -1;
        selected = -1;
        dirty = true;
        // Undo keeps the user's zoom and pan; only a change of the crop (the image size) re-fits the view.
        D2D1_RECT_F after = Crop();
        if (before.left != after.left || before.top != after.top || before.right != after.right || before.bottom != after.bottom)
            userZoom = false;
        LayoutAll();
        UpdateTitle();
        Invalidate();
    }

    void CommitText() {
        if (editingText < 0) return;
        if (editingText < (int)shapes.size() && shapes[editingText].text.empty()) {
            shapes.erase(shapes.begin() + editingText);
            if (selected == editingText) selected = -1;
        }
        editingText = -1;
        Invalidate();
    }

    void DeleteSelected() {
        if (selected < 0) return;
        Snapshot();
        if (editingText == selected) editingText = -1;
        shapes.erase(shapes.begin() + selected);
        selected = -1;
        RenumberSteps();
        Invalidate();
    }

    void DeleteAll() {
        CommitText();
        bool any = false;
        for (auto& s : shapes) any |= IsDocumentShape(s.tool);
        if (!any) return;
        Snapshot();
        shapes.erase(std::remove_if(shapes.begin(), shapes.end(), [](const Shape& s) { return IsDocumentShape(s.tool); }), shapes.end());
        selected = -1;
        Invalidate();
    }

    // ---------- file actions ----------
    void Copy() {
        CommitText();
        CopyImageToClipboard(Export());
        PlayUiSound(S_Action);
    }

    bool SaveTo(const std::wstring& p) {
        CommitText();
        Image out = Export();
        if (out.empty()) return false;
        bool jpeg = _wcsicmp(PathFindExtensionW(p.c_str()), L".jpg") == 0 || _wcsicmp(PathFindExtensionW(p.c_str()), L".jpeg") == 0;
        if (!EncodeImage(out, p, jpeg)) {
            PlayUiSound(S_Error);
            MessageBoxW(hwnd, (std::wstring(Tr(L"Не удалось сохранить ", L"Could not save ")) + p).c_str(), L"Zipshot", MB_ICONERROR);
            return false;
        }
        path = p;
        dirty = false;
        UpdateTitle();
        PlayUiSound(S_Action);
        return true;
    }

    bool Save() { return SaveTo(path.empty() ? NewOutputPath(L"png") : path); }

    bool SaveAs() {
        wchar_t file[MAX_PATH] = L"";
        if (!path.empty()) wcscpy_s(file, PathFindFileNameW(path.c_str()));
        std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"PNG\0*.png\0JPEG\0*.jpg\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = dir.c_str();
        ofn.lpstrDefExt = L"png";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn)) return false;
        return SaveTo(file);
    }

    // ---------- Save as GIF: the document over the merged timeline of its animated stickers ----------
    static bool RunHidden(std::wstring cmd, DWORD timeoutMs) {
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr,
                            &si, &pi))
            return false;
        bool ok = WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0;
        DWORD code = 1;
        if (ok) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return ok && code == 0;
    }

    // Frame times: every frame change of every animated shape, each looping, over the shortest common loop
    // (or the longest animation when that exceeds 10 s); changes less than 2/100 s apart (after rounding to the
    // GIF's hundredths) are merged, at most 500 frames.
    std::vector<long long> GifTimeline(long long& length) const {
        std::vector<const sx::Anim*> anims;
        for (const Shape& s : shapes)
            if (s.anim && s.anim->total > 0) anims.push_back(s.anim.get());
        length = 0;
        if (anims.empty()) return {0};
        long long lcm = 1, longest = 0;
        for (auto* a : anims) {
            longest = std::max<long long>(longest, a->total);
            lcm = lcm > 10000 ? lcm : std::lcm(lcm, (long long)a->total);
        }
        length = std::min<long long>(10000, lcm <= 10000 ? lcm : longest);
        std::vector<long long> t{0};
        for (auto* a : anims)
            for (long long base = 0; base < length; base += a->total) {
                long long x = base;
                for (int d : a->delays) {
                    x += d;
                    if (x < length) t.push_back(x);
                }
            }
        std::sort(t.begin(), t.end());
        std::vector<long long> merged;
        for (long long x : t)
            if (merged.empty() || (x + 5) / 10 - (merged.back() + 5) / 10 >= 2) merged.push_back(x);
        if (merged.size() > 500) {
            std::vector<long long> thin;
            for (size_t i = 0; i < 500; ++i) thin.push_back(merged[i * merged.size() / 500]);
            merged = std::move(thin);
        }
        return merged;
    }

    bool ExportGif(const std::wstring& out, std::wstring& err) {
        CommitText();
        std::wstring ff = FindFFmpeg();
        if (ff.empty()) {
            err = Tr(L"ffmpeg не найден (нужен для GIF).", L"ffmpeg was not found (needed for GIF).");
            return false;
        }
        long long length = 0;
        std::vector<long long> ts = GifTimeline(length);
        if (length <= 0) length = 1000;
        D2D1_RECT_F c = Crop();
        int w = (int)(c.right - c.left), h = (int)(c.bottom - c.top);
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICBitmap> bmp;
        ComPtr<ID2D1RenderTarget> t;
        D2D1_RENDER_TARGET_PROPERTIES rp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (w < 1 || h < 1 || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
            FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)) ||
            FAILED(D2D()->CreateWicBitmapRenderTarget(bmp.Get(), rp, &t))) {
            err = L"Direct2D";
            return false;
        }
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring dir = std::wstring(tmp) + L"zipshot_gif_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount());
        CreateDirectoryW(dir.c_str(), nullptr);
        Gfx gx = MakeGfx(t.Get(), *img);
        bool transparent = img->alpha;
        std::string list = "ffconcat version 1.0\n";
        std::vector<std::wstring> files;
        bool ok = true;
        long long lastCs = 10;
        std::wstring title0;
        if (hwnd) {
            wchar_t tb[512];
            GetWindowTextW(hwnd, tb, 512);
            title0 = tb;
        }
        for (size_t i = 0; i < ts.size() && ok; ++i) {
            animClock = ts[i];
            t->BeginDraw();
            t->Clear(transparent ? kClear : kWhite);
            t->SetTransform(D2D1::Matrix3x2F::Translation(-c.left, -c.top));
            DrawDocument(t.Get(), gx, false);
            ok = SUCCEEDED(t->EndDraw());
            Image f;
            f.w = w;
            f.h = h;
            f.px.resize((size_t)w * h);
            bmp->CopyPixels(nullptr, w * 4, (UINT)(f.px.size() * 4), (BYTE*)f.px.data());
            f.alpha = transparent;
            for (auto& px : f.px) {
                uint32_t a = px >> 24;
                if (!transparent) px |= 0xFF000000u;
                else if (a && a != 255)
                    px = a << 24 | std::min(255u, (px >> 16 & 255) * 255 / a) << 16 | std::min(255u, (px >> 8 & 255) * 255 / a) << 8 |
                         std::min(255u, (px & 255) * 255 / a);
            }
            wchar_t name[32];
            swprintf_s(name, L"f%05u.png", (unsigned)i);
            files.push_back(dir + L"\\" + name);
            ok = ok && EncodeImage(f, files.back(), false);
            // GIF delays are hundredths of a second: round the boundaries, not each delay, so nothing drifts.
            long long next = i + 1 < ts.size() ? ts[i + 1] : length;
            long long cs = (next + 5) / 10 - (ts[i] + 5) / 10;
            char line[96];
            if (i + 1 == ts.size()) lastCs = std::max<long long>(cs, 2);  // the gif muxer's -final_delay
            // framerate 100: the images' own time base, so the durations are not rounded to 1/25 s
            sprintf_s(line, "file 'f%05u.png'\noption framerate 100\nduration %.2f\n", (unsigned)i, std::max<long long>(cs, 1) / 100.0);
            list += line;
            if (hwnd && i % 10 == 0) {
                std::wstring tt = Tr(L"Zipshot — сохранение GIF ", L"Zipshot — saving GIF ") + std::to_wstring(i * 100 / ts.size()) + L"%";
                SetWindowTextW(hwnd, tt.c_str());
            }
        }
        animClock = -1;
        std::wstring listFile = dir + L"\\list.ffconcat";
        if (ok) {
            HANDLE fh = CreateFileW(listFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD wr = 0;
            ok = fh != INVALID_HANDLE_VALUE && WriteFile(fh, list.data(), (DWORD)list.size(), &wr, nullptr);
            if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
        }
        if (ok) {
            if (hwnd) SetWindowTextW(hwnd, Tr(L"Zipshot — кодирование GIF...", L"Zipshot — encoding GIF..."));
            std::wstring tmpOut = dir + L"\\out.gif";
            std::wstring vf = std::wstring(L"split[a][b];[a]palettegen=stats_mode=diff:reserve_transparent=") + (transparent ? L"1" : L"0") +
                              L"[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle" + (transparent ? L":alpha_threshold=128" : L"");
            std::wstring cmd = L"\"" + ff + L"\" -v error -y -f concat -safe 0 -i \"" + listFile + L"\" -vf \"" + vf +
                               L"\" -fps_mode vfr -loop 0 -final_delay " + std::to_wstring(lastCs) + L" \"" + tmpOut + L"\"";
            ok = RunHidden(cmd, 300000) && GetFileAttributesW(tmpOut.c_str()) != INVALID_FILE_ATTRIBUTES;
            if (ok) ok = MoveFileExW(tmpOut.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
            if (!ok) err = Tr(L"ffmpeg не смог собрать GIF.", L"ffmpeg could not build the GIF.");
            DeleteFileW(tmpOut.c_str());
        } else if (err.empty()) {
            err = Tr(L"Не удалось записать кадры.", L"Could not write the frames.");
        }
        for (auto& f : files) DeleteFileW(f.c_str());
        DeleteFileW(listFile.c_str());
        RemoveDirectoryW(dir.c_str());
        if (hwnd) SetWindowTextW(hwnd, title0.c_str());
        Log(L"ExportGif: %dx%d, %d frames over %lld ms -> %s (%s)", w, h, (int)ts.size(), length, out.c_str(), ok ? L"ok" : err.c_str());
        return ok;
    }

    void SaveGifCmd() {
        CommitText();
        if (!HasAnimation() &&
            MessageBoxW(hwnd, Tr(L"В изображении нет анимированных стикеров, GIF будет из одного кадра. Продолжить?",
                                 L"The image has no animated stickers, the GIF will have one frame. Continue?"),
                        L"Zipshot", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        std::wstring def = path.empty() ? NewOutputPath(L"gif") : path.substr(0, path.size() - wcslen(PathFindExtensionW(path.c_str()))) + L".gif";
        wchar_t file[MAX_PATH] = L"";
        wcscpy_s(file, PathFindFileNameW(def.c_str()));
        std::wstring dir = def.substr(0, def.find_last_of(L'\\'));
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"GIF\0*.gif\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = dir.c_str();
        ofn.lpstrDefExt = L"gif";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn)) return;
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        std::wstring err;
        bool ok = ExportGif(file, err);
        SetCursor(old);
        if (!ok) {
            PlayUiSound(S_Error);
            MessageBoxW(hwnd, (std::wstring(Tr(L"Не удалось сохранить GIF: ", L"Could not save the GIF: ")) + err).c_str(), L"Zipshot", MB_ICONERROR);
            return;
        }
        CopyFileToClipboard(file);
        MainWindowNotifyCapture(file);
        PlayUiSound(S_TaskDone);
    }

    // Edit > Add to my stickers: the source file (GIFs stay animated) or the shape's pixels as PNG.
    bool SelectedImageShape() const {
        return selected >= 0 && selected < (int)shapes.size() && IsImageShape(shapes[selected].tool) && shapes[selected].bmp;
    }
    void AddToMyStickers() {
        if (!SelectedImageShape()) return;
        const Shape& s = shapes[selected];
        std::wstring r = !s.src.empty() && GetFileAttributesW(s.src.c_str()) != INVALID_FILE_ATTRIBUTES ? sx::AddCustomStickerFile(s.src)
                                                                                                       : sx::AddCustomStickerImage(*s.bmp);
        PlayUiSound(r.empty() ? S_Error : S_Action);
    }

    void Apply() {
        CommitText();
        Image out = Export();
        if (Save()) {
            CopyImageToClipboard(out);
            DestroyWindow(hwnd);
        }
    }

    // ---------- options ----------
    // Colours and sizes apply to the shapes drawn next, never to the one already drawn (ShareX recolours
    // the last shape, which the user considers a bug).
    bool PickColor(D2D1_COLOR_F& c, const Button& b) {
        float rgba[4] = {c.r, c.g, c.b, c.a};
        POINT at = MenuPos(b);
        if (!ShowColorPicker(hwnd, at, rgba)) return false;
        c = {rgba[0], rgba[1], rgba[2], rgba[3]};
        return true;
    }

    POINT MenuPos(const Button& b) {
        POINT p{(LONG)b.r.left, (LONG)b.r.bottom};
        ClientToScreen(hwnd, &p);
        return p;
    }

    // ShareX "Tool options" drop-down for the current tool (ShapeManagerMenu.UpdateMenu decides what shows).
    void OptionsMenu(const Button& b) {
        HMENU m = CreatePopupMenu();
        std::vector<std::function<void()>> acts;  // menu id = index + 1
        auto item = [&](HMENU menu, const std::wstring& text, bool checked, std::function<void()> fn) {
            acts.push_back(std::move(fn));
            AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0), acts.size(), text.c_str());
        };
        auto sub = [&](HMENU parent, const wchar_t* title) {
            HMENU s = CreatePopupMenu();
            AppendMenuW(parent, MF_POPUP, (UINT_PTR)s, title);
            return s;
        };
        auto sep = [&](HMENU menu) {
            if (GetMenuItemCount(menu) > 0) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        };
        auto numbers = [&](HMENU parent, const wchar_t* title, std::initializer_list<int> vals, auto& target) {
            HMENU s = sub(parent, title);
            for (int v : vals) item(s, std::to_wstring(v), (int)target == v, [&target, v] { target = (std::remove_reference_t<decltype(target)>)v; });
        };
        auto names = [&](HMENU parent, const wchar_t* title, std::vector<std::wstring> list, int& target) {
            HMENU s = sub(parent, title);
            for (int i = 0; i < (int)list.size(); ++i) item(s, list[i], target == i, [&target, i] { target = i; });
        };
        auto textStyle = [&](TextStyle& ts) {
            HMENU fonts = sub(m, Tr(L"Шрифт", L"Font"));
            for (const std::wstring& f : sx::FontChoices(ts.font)) item(fonts, f, f == ts.font, [&ts, f] { ts.font = f; });
            item(m, Tr(L"Жирный", L"Bold"), ts.bold != 0, [&ts] { ts.bold = !ts.bold; });
            item(m, Tr(L"Курсив", L"Italic"), ts.italic != 0, [&ts] { ts.italic = !ts.italic; });
            item(m, Tr(L"Подчёркнутый", L"Underline"), ts.underline != 0, [&ts] { ts.underline = !ts.underline; });
            names(m, Tr(L"Выравнивание по горизонтали", L"Horizontal alignment"),
                  {Tr(L"По левому краю", L"Left"), Tr(L"По центру", L"Center"), Tr(L"По правому краю", L"Right")}, ts.alignH);
            names(m, Tr(L"Выравнивание по вертикали", L"Vertical alignment"),
                  {Tr(L"По верху", L"Top"), Tr(L"По середине", L"Middle"), Tr(L"По низу", L"Bottom")}, ts.alignV);
            item(m, Tr(L"Новая строка: Enter, OK: Ctrl + Enter", L"New line: Enter, OK: Ctrl + Enter"), ts.enterNewLine != 0,
                 [&ts] { ts.enterNewLine = !ts.enterNewLine; });
        };
        Tool t = tool;
        bool lineLike = t == T_Rect || t == T_Ellipse || IsFreehand(t) || IsLine(t);
        if (t == T_Magnify) numbers(m, Tr(L"Увеличение, %", L"Magnify strength, %"), {150, 200, 300, 400, 600, 800, 1000}, o.magnify);
        if (lineLike) numbers(m, Tr(L"Толщина обводки", L"Border size"), {1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 30}, o.borderSize);
        if (t == T_Rect || t == T_TextBackground || t == T_RegionRect)
            numbers(m, Tr(L"Радиус скругления", L"Corner radius"), {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 75, 100, 150},
                    t == T_RegionRect ? o.regionRadius : o.cornerRadius);
        if (lineLike)
            names(m, Tr(L"Стиль рамки", L"Border style"),
                  {Tr(L"Сплошная", L"Solid"), Tr(L"Пунктир", L"Dash"), Tr(L"Точки", L"Dot"), Tr(L"Штрихпунктир", L"DashDot"),
                   Tr(L"Штрихпунктир с двумя точками", L"DashDotDot")},
                  o.borderStyle);
        if (t == T_Cursor) {
            std::vector<std::wstring> cur;
            for (int i = 0; i < sx::CursorTypeCount(); ++i) cur.push_back(sx::CursorTypeName(i));
            names(m, Tr(L"Тип курсора", L"Cursor type"), cur, o.cursorType);
        }
        if (t == T_Blur) numbers(m, Tr(L"Радиус размытия", L"Blur radius"), {10, 20, 35, 50, 75, 100}, o.blur);
        if (t == T_Pixelate) numbers(m, Tr(L"Размер пикселя", L"Pixel size"), {5, 8, 10, 15, 20, 30, 50}, o.pixel);
        if (IsLine(t)) numbers(m, Tr(L"Центральных точек", L"Center points"), {0, 1, 2, 3, 4, 5}, o.centerPoints);
        if (t == T_Arrow || t == T_FreehandArrow)
            names(m, Tr(L"Направление стрелки", L"Arrow head direction"), {Tr(L"Конец", L"End"), Tr(L"Начало", L"Start"), Tr(L"Оба", L"Both")},
                  o.arrowDir);
        if (t == T_TextOutline) {
            numbers(m, Tr(L"Размер шрифта", L"Font size"), {8, 10, 12, 14, 18, 20, 25, 30, 36, 48, 60, 72}, o.outlineFont);
            numbers(m, Tr(L"Толщина обводки", L"Border size"), {0, 2, 3, 4, 5, 6, 8, 10}, o.outlineSize);
            sep(m);
            textStyle(o.outlineStyle);
        }
        if (t == T_TextBackground || t == T_SpeechBalloon) {
            numbers(m, Tr(L"Размер шрифта", L"Font size"), {12, 14, 16, 18, 20, 25, 30, 36, 48, 60, 72}, o.textFont);
            sep(m);
            textStyle(o.textStyle);
        }
        if (t == T_Step) {
            numbers(m, Tr(L"Размер шрифта", L"Font size"), {12, 14, 18, 20, 25, 30, 36, 48}, o.stepFont);
            HMENU first = sub(m, Tr(L"Значение на первом шаге", L"Value of first step"));
            for (int v : {0, 1, 2, 3, 4, 5, 10, 20, 50, 100})
                item(first, std::to_wstring(v), o.startStep == v, [this, v] { o.startStep = v; RenumberSteps(); });
            names(m, Tr(L"Тип шага", L"Step type"),
                  {Tr(L"Цифры", L"Numbers"), Tr(L"Буквы (верхний регистр)", L"Letters (Uppercase)"), Tr(L"Буквы (нижний регистр)", L"Letters (Lowercase)"),
                   Tr(L"Римские цифры (верхний регистр)", L"Roman numerals (Uppercase)"),
                   Tr(L"Римские цифры (нижний регистр)", L"Roman numerals (Lowercase)")},
                  o.stepType);
        }
        if (t == T_Sticker) numbers(m, Tr(L"Размер стикера", L"Sticker size"), {32, 48, 64, 96, 128, 192, 256}, o.stickerSize);
        if (lineLike || IsText(t) || t == T_Step || t == T_Magnify) {
            sep(m);
            item(m, Tr(L"Тень", L"Drop shadow"), o.shadow != 0, [this] { o.shadow = !o.shadow; });
            item(m, Tr(L"Цвет тени...", L"Drop shadow color..."), false, [this, &b] { PickColor(o.shadowColor, b); });
        }
        if (t == T_CutOut) {
            names(m, Tr(L"Эффект разрыва", L"Cut out effect"),
                  {Tr(L"Без эффекта", L"No effect"), Tr(L"Пила", L"Sawtooth"), Tr(L"Рваные края", L"Torn edges"), Tr(L"Волна", L"Wave")},
                  o.cutType);
            numbers(m, Tr(L"Размер эффекта", L"Cut out effect size"), {3, 5, 8, 10, 15, 20, 30, 50, 75, 100}, o.cutSize);
            item(m, Tr(L"Цвет фона разрыва...", L"Cut out background color..."), false, [this, &b] { PickColor(o.cutBg, b); });
        }
        POINT p = MenuPos(b);
        int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (id <= 0 || id > (int)acts.size()) return;
        acts[id - 1]();
        o.Save();
        Invalidate();
    }

    // Menu icons from the toolbar set (32bpp premultiplied DIBs, freed with the menu).
    HBITMAP MenuIcon(int id, std::vector<HBITMAP>& keep) {
        int px = (int)std::round(16 * ui);
        const IconData& d = Icon(id, px);
        if (!d.w) return nullptr;
        void* bits;
        HBITMAP bmp = CreateDib(d.w, d.h, &bits);
        if (!bmp) return nullptr;
        memcpy(bits, d.px.data(), d.px.size() * 4);
        keep.push_back(bmp);
        return bmp;
    }

    void EditMenu(const Button& b) {
        HMENU m = CreatePopupMenu();
        std::vector<HBITMAP> keep;
        bool sel = selected >= 0 && selected < (int)shapes.size() && IsDocumentShape(shapes[selected].tool);
        auto add = [&](int id, const wchar_t* text, int icon, bool enabled) {
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_BITMAP;
            mi.wID = id;
            mi.dwTypeData = (LPWSTR)text;
            mi.fState = enabled ? MFS_ENABLED : MFS_GRAYED;
            mi.hbmpItem = MenuIcon(icon, keep);
            InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
        };
        add(1, Tr(L"Отменить\tCtrl+Z", L"Undo\tCtrl+Z"), 123, !undo.empty());
        add(2, Tr(L"Повторить\tCtrl+Y", L"Redo\tCtrl+Y"), 124, !redo.empty());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        add(5, Tr(L"Вставить картинку/текст\tCtrl+V", L"Paste image/text\tCtrl+V"), 103, true);
        add(6, Tr(L"Дублировать\tCtrl+D", L"Duplicate\tCtrl+D"), 146, sel);
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        add(3, Tr(L"Удалить\tDel", L"Delete\tDel"), 125, selected >= 0);
        add(4, Tr(L"Удалить всё\tShift+Del", L"Delete all\tShift+Del"), 116, true);
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        add(7, Tr(L"На передний план\tHome", L"Bring to front\tHome"), 147, sel);
        add(8, Tr(L"Переместить вперед\tPage up", L"Bring forward\tPage up"), 148, sel);
        add(9, Tr(L"Переместить назад\tPage down", L"Send backward\tPage down"), 149, sel);
        add(10, Tr(L"На задний план\tEnd", L"Send to back\tEnd"), 150, sel);
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        add(11, Tr(L"Добавить в мои стикеры", L"Add to my stickers"), 143, SelectedImageShape());
        POINT p = MenuPos(b);
        int id = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        for (HBITMAP h : keep) DeleteObject(h);
        if (id == 1) Restore(undo, redo);
        else if (id == 2) Restore(redo, undo);
        else if (id == 3) DeleteSelected();
        else if (id == 4) DeleteAll();
        else if (id == 5) Paste(false);
        else if (id == 6) Duplicate(false);
        else if (id == 7) Reorder(O_Front);
        else if (id == 8) Reorder(O_Forward);
        else if (id == 9) Reorder(O_Backward);
        else if (id == 10) Reorder(O_Back);
        else if (id == 11) AddToMyStickers();
    }

    // ---------- Image menu (ShareX tsddbImage, editor only) ----------
    // Operations replace the base bitmap as one undo step (the history entry keeps the previous ImagePtr).
    // They act on what is visible: a pending crop is applied first, as ShareX crops destructively.

    void DrawChecker() {
        if (!g.checker) {
            uint32_t px[16 * 16];
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) px[y * 16 + x] = ((x / 8 + y / 8) % 2) ? 0xFF3A3A3A : 0xFF4A4A4A;
            ComPtr<ID2D1Bitmap> b;
            rt->CreateBitmap(D2D1::SizeU(16, 16), px, 64,
                             D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &b);
            if (b)
                rt->CreateBitmapBrush(b.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                           D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                                      &g.checker);
        }
        if (!g.checker) return;
        D2D1_RECT_F c = Crop();
        c = {std::max(c.left, 0.f), std::max(c.top, 0.f), std::min(c.right, (float)img->w), std::min(c.bottom, (float)img->h)};
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->FillRectangle(ToScreen(c), g.checker.Get());
    }

    void SetBitmap(ImagePtr p) {
        img = std::move(p);
        fxCache.clear();  // blur/pixelate/highlight were computed from the old bitmap
        g.fx.clear();
        if (rt) UploadBitmap(rt.Get(), g, *img);
    }

    // The bitmap cut to the crop and the shapes moved with it (crop shapes dropped).
    ImagePtr VisibleDocument(std::vector<Shape>& out) const {
        D2D1_RECT_F c = Crop();
        out.clear();
        for (const Shape& s : shapes) {
            if (s.tool == T_Crop) continue;
            Shape t = s;
            MoveShape(t, -c.left, -c.top);
            out.push_back(std::move(t));
        }
        RECT r{(LONG)c.left, (LONG)c.top, (LONG)c.right, (LONG)c.bottom};
        if (r.left == 0 && r.top == 0 && r.right == img->w && r.bottom == img->h) return img;
        return std::make_shared<Image>(ImgCrop(*img, r));
    }

    void ReplaceDocument(Image result, std::vector<Shape> newShapes) {
        if (result.empty()) {
            PlayUiSound(S_Error);
            return;
        }
        Snapshot();
        shapes = std::move(newShapes);
        for (Shape& s : shapes)
            if (IsFreehand(s.tool)) BuildGeo(s);
        SetBitmap(std::make_shared<Image>(std::move(result)));
        selected = editingText = -1;
        userZoom = false;  // fit the new size
        if (hwnd) {
            LayoutAll();
            Invalidate();
        }
    }

    // Moves a shape's points through `map`; `k` scales stroke widths and font sizes (Image size).
    template <class F>
    void MapShape(Shape& s, F map, float k) {
        if (IsText(s.tool) && k == 1 && !s.pts.empty()) {  // text keeps its size: move its box centre
            D2D1_RECT_F b = Bounds(s);
            D2D1_POINT_2F c{(b.left + b.right) / 2, (b.top + b.bottom) / 2}, n = map(c);
            MoveShape(s, n.x - c.x, n.y - c.y);  // the whole box and the balloon tail
        } else {
            for (auto& q : s.pts) q = map(q);
            s.tail = map(s.tail);
            if (HasBoxNodes(s.tool) && s.pts.size() >= 2) {  // keep box shapes normalised after rotate/flip
                D2D1_RECT_F r = Norm(s.pts[0], s.pts[1]);
                s.pts[0] = {r.left, r.top};
                s.pts[1] = {r.right, r.bottom};
            }
        }
        if (k != 1) {
            s.size *= k;
            if (s.tool != T_Magnify) s.size2 *= k;  // outline width, blur radius, pixel size; magnify is a %
            s.radius *= k;
            UpdateTextBox(s);
        }
    }

    template <class Op, class F>
    void GeometryOp(Op op, F map, float k = 1) {
        CommitText();
        std::vector<Shape> sh;
        ImagePtr base = VisibleDocument(sh);
        Image res = op(*base);
        float W = (float)base->w, H = (float)base->h;
        for (Shape& s : sh) MapShape(s, [&](D2D1_POINT_2F p) { return map(p, W, H); }, k);
        ReplaceDocument(std::move(res), std::move(sh));
    }

    void RotateImage(int quarters) {
        GeometryOp([&](const Image& im) { return ImgRotate90(im, quarters); },
                   [&](D2D1_POINT_2F p, float W, float H) -> D2D1_POINT_2F {
                       if (quarters == 1) return {H - p.y, p.x};
                       if (quarters == 3) return {p.y, W - p.x};
                       return {W - p.x, H - p.y};
                   });
    }

    void FlipImage(bool horizontal) {
        GeometryOp([&](const Image& im) { return ImgFlip(im, horizontal); },
                   [&](D2D1_POINT_2F p, float W, float H) -> D2D1_POINT_2F {
                       return horizontal ? D2D1_POINT_2F{W - p.x, p.y} : D2D1_POINT_2F{p.x, H - p.y};
                   });
    }

    void ResizeImage(int w, int h, int mode) {
        D2D1_RECT_F c = Crop();
        float kx = w / (c.right - c.left), ky = h / (c.bottom - c.top);
        GeometryOp([&](const Image& im) { return ImgResize(im, w, h, mode); },
                   [&](D2D1_POINT_2F p, float, float) -> D2D1_POINT_2F { return {p.x * kx, p.y * ky}; }, std::sqrt(kx * ky));
    }

    void CanvasImage(int l, int t, int r, int b, uint32_t argb) {
        if (!l && !t && !r && !b) return;  // ShareX AddCanvas: nothing to do
        GeometryOp([&](const Image& im) { return ImgCanvas(im, l, t, r, b, argb); },
                   [&](D2D1_POINT_2F p, float, float) -> D2D1_POINT_2F { return {p.x + l, p.y + t}; });
    }

    // Effects can change the size (shadow, border, canvas...); shapes keep their place relative to the
    // image centre, which is what ShareX's re-centred canvas does.
    void EffectsImage(const std::vector<Effect>& stack) {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        float nw = 0, nh = 0;  // the result size is known only after the op, which runs before the shapes move
        GeometryOp(
            [&](const Image& im) {
                Image r = ApplyEffects(im, stack);
                nw = (float)r.w, nh = (float)r.h;
                return r;
            },
            [&](D2D1_POINT_2F p, float W, float H) -> D2D1_POINT_2F { return {p.x + (nw - W) / 2, p.y + (nh - H) / 2}; });
        SetCursor(old);
    }

    void AutoCrop() {
        CommitText();
        Image vis = Export();  // ShareX looks at the result image, annotations included
        RECT r = ImgAutoCropRect(vis, 4);
        if (r.left == 0 && r.top == 0 && r.right == vis.w && r.bottom == vis.h) return;
        D2D1_RECT_F c = Crop();
        Snapshot();
        shapes.push_back({T_Crop, {}, {}, 0, 0, {{c.left + r.left, c.top + r.top}, {c.left + r.right, c.top + r.bottom}}});
        selected = -1;
        userZoom = false;
        if (hwnd) {
            LayoutAll();
            Invalidate();
        }
    }

    bool ConfirmReplace() {
        CommitText();
        if (!dirty) return true;
        int r = MessageBoxW(hwnd, Tr(L"Сохранить изменения?", L"Save changes?"), L"Zipshot", MB_YESNOCANCEL | MB_ICONQUESTION);
        return r == IDNO || (r == IDYES && Save());
    }

    void NewDocument(ImagePtr p, const std::wstring& newPath) {
        Snapshot();  // undoable, as in ShareX (CreateCanvasMemento)
        shapes.clear();
        SetBitmap(std::move(p));
        path = newPath;
        dirty = false;
        selected = editingText = -1;
        userZoom = false;
        UpdateTitle();
        LayoutAll();
        Invalidate();
    }

    void NewImageCmd() {
        NewImageOptions n;
        ImageSizeOptions s;
        CanvasOptions c;
        LoadImageEditorOptions(n, s, c);
        if (!ShowNewImageDialog(hwnd, n)) return;
        SaveImageEditorOptions(n, s, c);
        if (!ConfirmReplace()) return;
        NewDocument(std::make_shared<Image>(ImgNew(n.w, n.h, n.transparent ? 0 : n.color)), L"");
    }

    void OpenImageCmd() {
        wchar_t file[MAX_PATH] = L"";
        std::wstring dir = path.empty() ? SaveDirRoot() : path.substr(0, path.find_last_of(L'\\'));
        OPENFILENAMEW ofn{sizeof(ofn)};
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Image files\0*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.bmp;*.tif;*.tiff;*.ico;*.webp;*.heic;*.heif;*.avif;*.jxr;*.wdp;*.dds\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = dir.c_str();
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) return;
        auto p = std::make_shared<Image>();
        if (!LoadImageFile(file, *p) || p->empty()) {
            PlayUiSound(S_Error);
            MessageBoxW(hwnd, (std::wstring(Tr(L"Не удалось открыть ", L"Could not open ")) + file).c_str(), L"Zipshot", MB_ICONERROR);
            return;
        }
        if (!ConfirmReplace()) return;
        NewDocument(p, file);
    }

    void ImageSizeCmd() {
        NewImageOptions n;
        ImageSizeOptions s;
        CanvasOptions c;
        LoadImageEditorOptions(n, s, c);
        D2D1_RECT_F cr = Crop();
        int w = (int)(cr.right - cr.left), h = (int)(cr.bottom - cr.top);
        if (!ShowImageSizeDialog(hwnd, w, h, s)) return;
        SaveImageEditorOptions(n, s, c);
        if (s.w != w || s.h != h) ResizeImage(s.w, s.h, s.resampling);
    }

    void CanvasSizeCmd() {
        NewImageOptions n;
        ImageSizeOptions s;
        CanvasOptions c;
        LoadImageEditorOptions(n, s, c);
        if (!ShowCanvasSizeDialog(hwnd, c)) return;
        SaveImageEditorOptions(n, s, c);
        CanvasImage(c.left, c.top, c.right, c.bottom, c.color);
    }

    void EffectsCmd() {
        CommitText();
        std::vector<Shape> unused;
        ImagePtr base = VisibleDocument(unused);
        std::vector<Effect> stack = LoadEffectStack();
        if (!ShowImageEffectsDialog(hwnd, base, stack)) return;
        if (std::any_of(stack.begin(), stack.end(), [](const Effect& e) { return e.enabled; })) EffectsImage(stack);
    }

    HBITMAP MenuIcon(int id) {
        int px = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(hwnd));
        const IconData& d = Icon(id, px);
        if (!d.w) return nullptr;
        void* bits;
        HBITMAP b = CreateDib(d.w, d.h, &bits);  // premultiplied 32bpp: menus alpha-blend it
        if (b) memcpy(bits, d.px.data(), d.px.size() * 4);
        return b;
    }

    void ImageMenu(const Button& b) {
        CommitText();
        struct Item { int id, icon; const wchar_t *ru, *en; };
        const Item items[] = {
            {1, 171, L"Новое изображение...", L"New image..."},
            {2, 172, L"Открыть файл изображения...", L"Open image file..."},
            {0, 0, nullptr, nullptr},
            {3, 173, L"Размер изображения...", L"Image size..."},
            {4, 174, L"Размер полотна...", L"Canvas size..."},
            {5, 120, L"Кадрировать (C)", L"Crop image (C)"},
            {6, 175, L"Автокадрирование", L"Auto crop image"},
            {0, 0, nullptr, nullptr},
            {7, 176, L"Повернуть на 90° по часовой стрелке", L"Rotate 90° clockwise"},
            {8, 177, L"Повернуть на 90° против часовой стрелки", L"Rotate 90° counter clockwise"},
            {9, 178, L"Повернуть на 180°", L"Rotate 180°"},
            {0, 0, nullptr, nullptr},
            {10, 179, L"Отразить по горизонтали", L"Flip horizontal"},
            {11, 180, L"Отразить по вертикали", L"Flip vertical"},
            {0, 0, nullptr, nullptr},
            {12, 181, L"Эффекты изображения...", L"Image effects..."},
            {0, 0, nullptr, nullptr},
            {13, 0, L"Сохранить как GIF (анимированные стикеры)...", L"Save as GIF (animated stickers)..."},
        };
        HMENU m = CreatePopupMenu();
        std::vector<HBITMAP> bmps;
        for (const Item& it : items) {
            if (!it.id) {
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                continue;
            }
            AppendMenuW(m, MF_STRING, it.id, Tr(it.ru, it.en));
            if (HBITMAP hb = it.icon ? MenuIcon(it.icon) : nullptr) {
                MENUITEMINFOW mi{sizeof(mi)};
                mi.fMask = MIIM_BITMAP;
                mi.hbmpItem = hb;
                SetMenuItemInfoW(m, it.id, FALSE, &mi);
                bmps.push_back(hb);
            }
        }
        POINT p = MenuPos(b);
        int id = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        for (HBITMAP hb : bmps) DeleteObject(hb);
        switch (id) {
        case 1: NewImageCmd(); break;
        case 2: OpenImageCmd(); break;
        case 3: ImageSizeCmd(); break;
        case 4: CanvasSizeCmd(); break;
        case 5: SetTool(T_Crop); break;
        case 6: AutoCrop(); break;
        case 7: RotateImage(1); break;
        case 8: RotateImage(3); break;
        case 9: RotateImage(2); break;
        case 10: FlipImage(true); break;
        case 11: FlipImage(false); break;
        case 12: EffectsCmd(); break;
        case 13: SaveGifCmd(); break;
        }
    }

    void ColorButton(const Button& b) {
        D2D1_COLOR_F* target = b.kind == B_Border ? BorderRef() : b.kind == B_Fill ? FillRef() : &o.highlight;
        if (!PickColor(*target, b)) return;
        o.Save();
        Invalidate();
    }

    void SetTool(Tool t) {
        CommitText();
        tool = t;
        if (IsFreehand(t) || (selected >= 0 && shapes[selected].tool != t)) selected = -1;
        LayoutToolbar();
        Invalidate();
    }

    void ClickButton(const Button& b) {
        showTip = false;
        switch (b.kind) {
        case B_Apply: Apply(); return;
        case B_Save: Save(); break;
        case B_SaveAs: SaveAs(); break;
        case B_Copy: Copy(); break;
        case B_Close: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case B_Tool: SetTool((Tool)b.idx); break;
        case B_Border:
        case B_Fill:
        case B_Highlight: ColorButton(b); break;
        case B_Options: OptionsMenu(b); break;
        case B_Edit: EditMenu(b); break;
        case B_Capture: CaptureMenu(b); return;
        case B_RegionOptions: RegionOptionsMenu(b); break;
        case B_Image: ImageMenu(b); break;
        default: break;
        }
        Invalidate();
    }

    // ---------- mouse ----------
    Shape NewShape(D2D1_POINT_2F p) const {
        Shape s{tool};
        s.pts = {p};
        s.shadow = o.shadow != 0;
        s.shadowColor = o.shadowColor;
        s.borderStyle = o.borderStyle;
        s.arrowDir = o.arrowDir;
        switch (tool) {
        case T_Rect: case T_Ellipse:
            s.color = o.border; s.color2 = o.fill; s.size = o.borderSize; s.radius = (float)o.cornerRadius; break;
        case T_Freehand: case T_FreehandArrow: case T_Line: case T_Arrow:
            s.color = o.border; s.size = o.borderSize; break;
        case T_TextOutline:
            s.color = o.outlineColor; s.color2 = o.outlineText; s.size = o.outlineFont; s.size2 = o.outlineSize; s.ts = o.outlineStyle; break;
        case T_TextBackground: case T_SpeechBalloon:
            s.color = o.textColor; s.color2 = o.textFill; s.size = o.textFont; s.ts = o.textStyle; s.radius = (float)o.cornerRadius; break;
        case T_Step: {
            s.color = o.stepText; s.color2 = o.stepFill; s.size = o.stepFont; s.stepType = o.stepType;
            float r = StepRadius(s);
            s.tail = {p.x + r + 5, p.y + r + 5};  // ShareX: below right of the circle until dragged
            break;
        }
        case T_Magnify:
            s.color = o.border; s.size2 = o.magnify; break;
        case T_Blur: s.size2 = o.blur; break;
        case T_Pixelate: s.size2 = o.pixel; break;
        case T_Highlight: s.color2 = o.highlight; break;
        default: break;
        }
        return s;
    }

    int HitShape(D2D1_POINT_2F p) const {
        for (int i = (int)shapes.size() - 1; i >= 0; --i) {
            if (!IsDocumentShape(shapes[i].tool)) continue;
            D2D1_RECT_F b = Bounds(shapes[i]);
            float tol = 4 / scale;
            if (p.x >= b.left - tol && p.x <= b.right + tol && p.y >= b.top - tol && p.y <= b.bottom + tol) return i;
        }
        return -1;
    }

    bool OverSelected(D2D1_POINT_2F p) const {
        if (selected < 0) return false;
        D2D1_RECT_F b = Bounds(shapes[selected]);
        float tol = 4 / scale;
        return p.x >= b.left - tol && p.x <= b.right + tol && p.y >= b.top - tol && p.y <= b.bottom + tol;
    }

    int ButtonAt(int x, int y) const {
        for (int i = 0; i < (int)buttons.size(); ++i)
            if (buttons[i].kind != B_Sep && x >= buttons[i].r.left && x < buttons[i].r.right && y >= buttons[i].r.top &&
                y < buttons[i].r.bottom)
                return i;
        return -1;
    }

    void MouseDown(int x, int y, bool dbl) {
        int bi = ButtonAt(x, y);
        if (bi >= 0 && buttons[bi].kind == B_Grip) {
            gripping = true;
            gripStart = {x, y};
            SetCapture(hwnd);
            return;
        }
        if (bi >= 0) { ClickButton(buttons[bi]); return; }
        D2D1_POINT_2F sp{(float)x, (float)y};
        if (cutPending) {  // ShareX CutOutTool: confirm / cancel buttons next to the band
            D2D1_RECT_F ok, no;
            CutButtons(ok, no);
            if (InRect(ok, sp)) { ApplyCutOut(); return; }
            if (InRect(no, sp)) { cutPending = false; Invalidate(); return; }
        }
        if (!region && y < tbH) return;
        D2D1_POINT_2F p = ToImage(x, y);
        // The selected shape's resize nodes take the click first (ShareX: DrawableObjects before shapes).
        Node node;
        if (!dbl && NodeAt(x, y, node)) {
            CommitText();
            if (NodeAt(x, y, node)) {
                StartNodeDrag(node);
                SetCapture(hwnd);
                Invalidate();
                return;
            }
        }
        if (region && IsRegion(tool)) {
            CommitText();
            if (dbl && !regions.empty()) {  // ShareX: double click captures the regions
                CaptureRegions();
                return;
            }
            if (!o.quickCrop) {  // multi region mode: grab a finished region to move or resize it
                auto hit = RegionHit({x, y});
                if (hit.first >= 0) {
                    selRegion = hit.first;
                    regionDrag = hit.second;
                    if (o.switchToDrawing) SetTool(regions[selRegion].tool);
                    lastMouse = {(float)x, (float)y};
                    SetCapture(hwnd);
                    Invalidate();
                    return;
                }
            }
            if (o.fixedSize) {  // the fixed rectangle under the cursor is taken on release
                fixedClick = true;
                SetCapture(hwnd);
                return;
            }
            creatingRegion = true;
            regionShape = Shape{tool};
            regionShape.pts = {{(float)x, (float)y}};
            lastMouse = {(float)x, (float)y};
            if (tool != T_RegionFreehand) regionShape.pts.push_back({(float)x, (float)y});
            SetCapture(hwnd);
            Invalidate();
            return;
        }
        // Double-click (ShareX OnDoubleClicked): edit a text, pick another image file or sticker.
        if (dbl && !IsFreehand(tool)) {
            int h = HitShape(p);
            if (h >= 0 && (shapes[h].tool == T_Image || shapes[h].tool == T_Sticker)) {
                CommitText();
                selected = h;
                ReplaceImage(h);
                return;
            }
        }
        // Clicking an existing text of the same kind edits it (ShareX: double-click).
        if (IsText(tool) || (dbl && !IsFreehand(tool))) {
            int h = HitShape(p);
            if (h >= 0 && IsText(shapes[h].tool) && (dbl || shapes[h].tool == tool)) {
                CommitText();
                selected = editingText = h;
                Snapshot();
                Invalidate();
                return;
            }
        }
        CommitText();
        SetCapture(hwnd);
        // A pen always starts a new stroke, even over an existing object's bounds.
        // Switching to Select remains the explicit way to move or reshape it.
        if (tool == T_Select || (!IsFreehand(tool) && OverSelected(p))) {
            int h = tool == T_Select ? HitShape(p) : selected;
            if (tool == T_Select && OverSelected(p)) h = selected;
            selected = h;
            if (h >= 0) {
                Snapshot();
                moving = true;
                dragStart = p;
                if (region && o.switchToDrawing && tool == T_Select) tool = shapes[h].tool, LayoutToolbar();  // ShareX option
            }
            Invalidate();
            return;
        }
        if (tool == T_Crop) {
            cropping = true;
            cropA = cropB = p;
            return;
        }
        if (tool == T_CutOut) {
            cutting = true;
            cutPending = false;
            cutA = cutB = p;
            selected = -1;
            Invalidate();
            return;
        }
        if (tool == T_Image || tool == T_Sticker || tool == T_Cursor || (tool == T_ImageScreen && !region)) {
            ReleaseCapture();
            PlaceImageTool(p);
            return;
        }
        Snapshot();
        Shape s = NewShape(p);
        if (IsText(tool)) {
            ReleaseCapture();
            UpdateTextBox(s);
            if (tool == T_SpeechBalloon) s.tail = {s.pts[0].x, s.pts[1].y + 30};  // ShareX: below the bottom-left corner
            shapes.push_back(std::move(s));
            selected = editingText = (int)shapes.size() - 1;
            Invalidate();
            return;
        }
        if (tool == T_Step) {
            shapes.push_back(std::move(s));
            selected = (int)shapes.size() - 1;
            RenumberSteps();
            ReleaseCapture();
            Invalidate();
            return;
        }
        if (tool == T_SmartEraser) {
            int ix = std::clamp((int)p.x, 0, img->w - 1), iy = std::clamp((int)p.y, 0, img->h - 1);
            uint32_t c = img->px[(size_t)iy * img->w + ix];
            s.color2 = Rgb(c >> 16 & 255, c >> 8 & 255, c & 255);
        }
        if (IsTwoPoint(tool)) s.pts.push_back(p);
        lastImagePt = p;
        shapes.push_back(std::move(s));
        selected = (int)shapes.size() - 1;
        drawing = true;
        Invalidate();
    }

    void MouseMove(int x, int y, bool buttonDown) {
        if (region) {
            cursor = {x, y};
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            if (gripping) {
                gripDX += x - gripStart.x;
                gripDY += y - gripStart.y;
                gripStart = {x, y};
                LayoutToolbar();
                Invalidate();
                return;
            }
            D2D1_POINT_2F mp{(float)x, (float)y}, delta{mp.x - lastMouse.x, mp.y - lastMouse.y};
            lastMouse = mp;
            if (regionDrag) {
                DragRegion(delta);
                Invalidate();
                return;
            }
            if (creatingRegion) {
                if (regionShape.tool == T_RegionFreehand && !KeyDown(VK_CONTROL)) {
                    D2D1_POINT_2F l = regionShape.pts.back();
                    if (std::abs(l.x - mp.x) + std::abs(l.y - mp.y) >= 1) regionShape.pts.push_back(mp);
                } else {
                    UpdateCreating(regionShape.pts, regionShape.tool, mp, delta);
                }
                Invalidate();
                return;
            }
            if (IsRegion(tool)) UpdateHover();
            Invalidate();
        }
        if (!buttonDown && !drawing && !moving && !cropping && !panning && dragNode < 0 && !cutting) {
            int bi = ButtonAt(x, y);
            if (bi != hoverBtn) {
                hoverBtn = bi;
                showTip = false;
                KillTimer(hwnd, 1);
                if (bi >= 0) SetTimer(hwnd, 1, 450, nullptr);
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                Invalidate();
            }
            if (cutPending) {
                D2D1_RECT_F ok, no;
                CutButtons(ok, no);
                D2D1_POINT_2F sp{(float)x, (float)y};
                int h = InRect(ok, sp) ? 0 : InRect(no, sp) ? 1 : -1;
                if (h != cutHover) {
                    cutHover = h;
                    Invalidate();
                }
            }
        }
        if (panning) {
            offX = panOX + (x - panStart.x);
            offY = panOY + (y - panStart.y);
            Invalidate();
            return;
        }
        D2D1_POINT_2F p = ToImage(x, y);
        if (dragNode >= 0 && selected >= 0) {
            DragNode(p, KeyDown(VK_SHIFT));
            Invalidate();
            return;
        }
        if (cutting) {
            cutB = p;
            Invalidate();
            return;
        }
        if (cropping) {
            D2D1_RECT_F c = Crop();
            cropB = {std::clamp(p.x, c.left, c.right), std::clamp(p.y, c.top, c.bottom)};
            Invalidate();
            return;
        }
        if (moving && selected >= 0) {
            float dx = p.x - dragStart.x, dy = p.y - dragStart.y;
            dragStart = p;
            MoveShape(shapes[selected], dx, dy);
            Invalidate();
            return;
        }
        if (!drawing) return;
        Shape& s = shapes.back();
        D2D1_POINT_2F delta{p.x - lastImagePt.x, p.y - lastImagePt.y};
        lastImagePt = p;
        if (IsFreehand(s.tool) && !KeyDown(VK_CONTROL)) {
            D2D1_POINT_2F l = s.pts.back();
            if (std::abs(l.x - p.x) + std::abs(l.y - p.y) < 1.0f / scale) return;
            s.pts.push_back(p);
            BuildGeo(s);
        } else {
            UpdateCreating(s.pts, s.tool, p, delta);
            if (IsFreehand(s.tool)) BuildGeo(s);
        }
        Invalidate();
    }

    void MouseUp() {
        ReleaseCapture();
        if (gripping) {
            gripping = false;
            return;
        }
        if (dragNode >= 0) {
            EndNodeDrag();
            return;
        }
        if (cutting) {
            cutting = false;
            cutPending = CutValid();
            cutHover = -1;
            Invalidate();
            return;
        }
        if (regionDrag) {
            regionDrag = 0;
            Invalidate();
            return;
        }
        if (creatingRegion || fixedClick) {
            bool valid = RegionValid();
            Shape rs = regionShape;
            creatingRegion = fixedClick = false;
            if (valid) {
                RegionSelected(rs);
            } else if (hasHover) {  // a click selects the window/control (or the fixed size rectangle) under the cursor
                rs = Shape{T_RegionRect};
                rs.pts = {{(float)hover.left, (float)hover.top}, {(float)hover.right - 1, (float)hover.bottom - 1}};
                RegionSelected(rs);
            }
            Invalidate();
            return;
        }
        if (moving) {
            moving = false;
            Invalidate();
            return;
        }
        if (cropping) {
            cropping = false;
            D2D1_RECT_F r = Norm(cropA, cropB);
            r = {std::floor(r.left), std::floor(r.top), std::ceil(r.right), std::ceil(r.bottom)};
            if (r.right - r.left >= 4 && r.bottom - r.top >= 4) {
                Snapshot();
                Shape c{T_Crop};
                c.pts = {{r.left, r.top}, {r.right, r.bottom}};
                shapes.push_back(std::move(c));
                selected = -1;
                userZoom = false;
                LayoutAll();
            }
            Invalidate();
            return;
        }
        if (!drawing) return;
        drawing = false;
        Shape& s = shapes.back();
        bool tiny = IsTwoPoint(s.tool) && std::abs(s.pts[0].x - s.pts[1].x) < 2 && std::abs(s.pts[0].y - s.pts[1].y) < 2;
        if (s.tool == T_ImageScreen && !tiny) {  // region surface: copy that part of the frozen screen (ShareX)
            D2D1_RECT_F r = Norm(s.pts[0], s.pts[1]);
            RECT ir{(LONG)std::floor(r.left), (LONG)std::floor(r.top), (LONG)std::ceil(r.right), (LONG)std::ceil(r.bottom)};
            Image im = sx::CropPremul(*img, ir);
            if (im.empty()) {
                tiny = true;
            } else {
                ir = {std::max<LONG>(ir.left, 0), std::max<LONG>(ir.top, 0), 0, 0};
                s.pts = {{(float)ir.left, (float)ir.top}, {(float)(ir.left + im.w), (float)(ir.top + im.h)}};
                s.bmp = std::make_shared<Image>(std::move(im));
            }
        }
        if (tiny) {
            shapes.pop_back();
            undo.pop_back();
            selected = -1;
            Invalidate();
            return;
        }
        if (IsLine(s.tool) && o.centerPoints > 0) {  // LineDrawingShape: CenterPointCount nodes between the ends
            s.pts.insert(s.pts.begin() + 1, (size_t)std::clamp(o.centerPoints, 0, 5), s.pts[0]);
            AutoCenterPoints(s);
        }
        if (s.pts.size() >= 2) {
            D2D1_RECT_F r = Norm(s.pts[0], s.pts.back());
            s.initSize = {r.right - r.left, r.bottom - r.top};
        }
        if (IsFreehand(s.tool)) selected = -1;  // stay ready for the next stroke, without selection handles
        if (region && o.switchToSelect && !IsFreehand(s.tool)) {  // freehand remains a continuous drawing tool
            tool = T_Select;
            LayoutToolbar();
        }
        Invalidate();
    }

    void Zoom(float factor, int x, int y) {
        float ns = std::clamp(scale * factor, 0.05f, 32.0f);
        offX = x - (x - offX) * ns / scale;
        offY = y - (y - offY) * ns / scale;
        scale = ns;
        userZoom = true;
        Invalidate();
    }

    void OnChar(wchar_t ch) {
        if (editingText < 0) return;
        Shape& s = shapes[editingText];
        if (ch == 27) { CommitText(); return; }
        if (ch == L'\r' || ch == L'\n') {  // '\n' is Ctrl+Enter
            bool ctrlEnter = ch == L'\n' || GetKeyState(VK_CONTROL) < 0;
            // ShareX EnterKeyNewLine: Enter adds a line and Ctrl+Enter finishes, or the other way round
            // (Shift+Enter also adds a line, as before).
            bool newline = s.ts.enterNewLine ? !ctrlEnter : (ctrlEnter || GetKeyState(VK_SHIFT) < 0);
            if (!newline) {
                CommitText();
                return;
            }
            s.text += L'\n';
        } else if (ch == 8) {
            if (!s.text.empty()) s.text.pop_back();
        } else if (ch == 22) {  // Ctrl+V
            if (OpenClipboard(hwnd)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* t = (const wchar_t*)GlobalLock(h)) {
                        for (const wchar_t* c = t; *c; ++c)
                            if (*c != L'\r') s.text += *c;
                        GlobalUnlock(h);
                    }
                }
                CloseClipboard();
            }
        } else if (ch >= 32) {
            s.text += ch;
        }
        UpdateTextBox(s);
        dirty = true;
        Invalidate();
    }

    bool OnKey(WPARAM vk) {
        if (editingText >= 0) return false;
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (cutPending && (vk == VK_RETURN || vk == VK_ESCAPE)) {  // the band's confirm / cancel buttons
            if (vk == VK_RETURN) ApplyCutOut();
            cutPending = false;
            Invalidate();
            return true;
        }
        if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) return ArrowKey(vk);
        if (region) {
            if (vk == VK_INSERT) {  // keyboard region selection: Insert starts at the cursor, Insert ends it
                if (creatingRegion) {
                    Shape rs = regionShape;
                    bool valid = RegionValid();
                    creatingRegion = false;
                    if (valid) RegionSelected(rs);
                } else if (IsRegion(tool)) {
                    creatingRegion = true;
                    regionShape = Shape{tool};
                    regionShape.pts = {{(float)cursor.x, (float)cursor.y}};
                    if (tool != T_RegionFreehand) regionShape.pts.push_back(regionShape.pts[0]);
                    lastMouse = regionShape.pts[0];
                }
                Invalidate();
                return true;
            }
            if (vk == VK_DELETE && creatingRegion) {
                creatingRegion = false;
                ReleaseCapture();
                Invalidate();
                return true;
            }
            if (vk == VK_ESCAPE) { CancelRegion(); return true; }
            if (vk == VK_RETURN) { CaptureRegions(); return true; }  // ShareX "Capture regions"
            if (vk == VK_SPACE) { CaptureArea(vs); return true; }      // ShareX "Capture fullscreen"
            if (vk == VK_DELETE && !shift && selected < 0 && selRegion >= 0 && selRegion < (int)regions.size()) {
                regions.erase(regions.begin() + selRegion);
                selRegion = regions.empty() ? -1 : (int)regions.size() - 1;
                Invalidate();
                return true;
            }
            if (vk == VK_TAB) { SwapToolType(); return true; }
            if (vk == VK_NUMPAD0) { SetTool(T_RegionRect); return true; }
            if (vk == VK_RETURN || vk == 'C' || (ctrl && (vk == 'S' || vk == 'C' || vk == '0'))) return true;
        }
        if (ctrl) {
            if (vk == 'Z') shift ? Restore(redo, undo) : Restore(undo, redo);
            else if (vk == 'Y') Restore(redo, undo);
            else if (vk == 'C') Copy();
            else if (vk == 'S') shift ? SaveAs() : Save();
            else if (vk == 'D') Duplicate(true);
            else if (vk == 'V') Paste(true);
            else if (vk == '0') { userZoom = false; LayoutAll(); Invalidate(); }
            else return false;
            return true;
        }
        if (vk == VK_HOME) { Reorder(O_Front); return true; }
        if (vk == VK_END) { Reorder(O_Back); return true; }
        if (vk == VK_PRIOR) { Reorder(O_Forward); return true; }
        if (vk == VK_NEXT) { Reorder(O_Backward); return true; }
        if (vk == VK_DELETE) { shift ? DeleteAll() : DeleteSelected(); return true; }
        if (vk == VK_RETURN) { Apply(); return true; }
        if (vk == VK_ESCAPE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return true; }
        if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) { SetTool((Tool)kNumpadTools[vk - VK_NUMPAD0]); return true; }
        for (int i = 0; i < T_Count; ++i)
            if (kTools[i].key && vk == (WPARAM)kTools[i].key && ToolAvailable(i)) { SetTool((Tool)i); return true; }
        return false;
    }

    HCURSOR CursorFor(POINT p) const {
        Node n;
        if (NodeAt(p.x, p.y, n)) return NodeCursor(n.id);
        if (cutPending) {
            D2D1_RECT_F ok, no;
            CutButtons(ok, no);
            D2D1_POINT_2F sp{(float)p.x, (float)p.y};
            if (InRect(ok, sp) || InRect(no, sp)) return LoadCursorW(nullptr, IDC_HAND);
        }
        if (region) {
            if (ButtonAt(p.x, p.y) >= 0 || CursorOverBar()) return LoadCursorW(nullptr, IDC_ARROW);
            D2D1_POINT_2F ip{(float)p.x, (float)p.y};
            if (IsRegion(tool) && !o.quickCrop && !creatingRegion) {  // finished regions: move / resize
                int mode = regionDrag ? regionDrag : RegionHit(p).second;
                if (mode == RD_Move) return LoadCursorW(nullptr, IDC_SIZEALL);
                if (mode == (RD_L | RD_T) || mode == (RD_R | RD_B)) return LoadCursorW(nullptr, IDC_SIZENWSE);
                if (mode == (RD_R | RD_T) || mode == (RD_L | RD_B)) return LoadCursorW(nullptr, IDC_SIZENESW);
                if (mode == RD_L || mode == RD_R) return LoadCursorW(nullptr, IDC_SIZEWE);
                if (mode == RD_T || mode == RD_B) return LoadCursorW(nullptr, IDC_SIZENS);
            }
            if (!IsRegion(tool) && (tool == T_Select ? HitShape(ip) >= 0 : !IsFreehand(tool) && OverSelected(ip))) return LoadCursorW(nullptr, IDC_SIZEALL);
            if (IsText(tool)) return LoadCursorW(nullptr, IDC_IBEAM);
            static HCURSOR cross = LoadCursorW(g_inst, L"CROSSHAIR");  // ShareX's own Crosshair.cur
            return cross ? cross : LoadCursorW(nullptr, IDC_CROSS);
        }
        if (ButtonAt(p.x, p.y) >= 0 || p.y < tbH) return LoadCursorW(nullptr, IDC_ARROW);
        D2D1_POINT_2F ip = ToImage(p.x, p.y);
        if (tool == T_Select) return LoadCursorW(nullptr, HitShape(ip) >= 0 ? IDC_SIZEALL : IDC_ARROW);
        if (!IsFreehand(tool) && OverSelected(ip)) return LoadCursorW(nullptr, IDC_SIZEALL);
        if (IsText(tool)) return LoadCursorW(nullptr, IDC_IBEAM);
        return LoadCursorW(nullptr, IDC_CROSS);
    }
};

bool g_quietEditor = false;  // tests: off-screen, never activated

LRESULT CALLBACK EditorProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* e = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!e && m != WM_NCCREATE) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_NCCREATE: {
        e = (Editor*)((CREATESTRUCTW*)l)->lpCreateParams;
        e->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)e);
        break;
    }
    case WM_SIZE:
        if (!e) break;
        if (e->rt) e->rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        e->LayoutAll();
        e->Invalidate();
        return 0;
    case WM_DPICHANGED: {
        if (e->region) return 0;  // spans monitors; the toolbar uses the active monitor's scale
        e->ui = HIWORD(w) / 96.0f;
        e->tipFont = Font(L"Segoe UI", 12 * e->ui, false);
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        e->Render();
        ValidateRect(h, nullptr);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            SetCursor(e->CursorFor(p));
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (w == 9) {  // animated stickers
            ++e->animTicks;
            if (!IsWindowVisible(h) || IsIconic(h)) {
                KillTimer(h, 9);
                e->animTimer = false;
            } else if (e->AnimSignature() != e->animSig) {
                e->Invalidate();
            }
            return 0;
        }
        if (w == 2) {  // region capture: marching ants (skip if a paint just ran, never starve mouse input)
            if (e->region && (e->o.showFps || (e->o.animations && (e->hasHover || e->creatingRegion || e->o.crosshair)))) {
                if (e->o.showFps || GetTickCount64() - e->lastPaintMs >= 40) e->Invalidate();
            }
            return 0;
        }
        KillTimer(h, 1);
        e->showTip = e->hoverBtn >= 0;
        e->Invalidate();
        return 0;
    case WM_MOUSELEAVE:
        if (e->region && !e->creatingRegion) e->cursor = {-10000, -10000};
        e->hoverBtn = -1;
        e->showTip = false;
        e->Invalidate();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        e->MouseDown((short)LOWORD(l), (short)HIWORD(l), m == WM_LBUTTONDBLCLK);
        return 0;
    case WM_MOUSEMOVE:
        e->MouseMove((short)LOWORD(l), (short)HIWORD(l), (w & MK_LBUTTON) != 0);
        return 0;
    case WM_LBUTTONUP:
        e->MouseUp();
        return 0;
    case WM_RBUTTONUP:
        if (e->region) {  // ShareX: RemoveShapeCancelCapture; with regions kept, a click on nothing captures them
            if (e->RemoveShapeAt({(short)LOWORD(l), (short)HIWORD(l)})) e->Invalidate();
            else if (!e->CaptureRegions()) e->CancelRegion();
            return 0;
        }
        // ShareX: right click deletes the shape under the cursor.
        if (int hit = e->HitShape(e->ToImage((short)LOWORD(l), (short)HIWORD(l))); hit >= 0) {
            e->CommitText();
            e->selected = hit;
            e->DeleteSelected();
        }
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        if (e->region) return TRUE;
        break;
    case WM_XBUTTONUP:
        if (e->region) {  // ShareX RegionCaptureActionX1Click / X2Click
            e->RunRegionAction(GET_XBUTTON_WPARAM(w) == XBUTTON1 ? e->o.actionX1 : e->o.actionX2, {(short)LOWORD(l), (short)HIWORD(l)});
            return TRUE;
        }
        break;
    case WM_MBUTTONDOWN:
        if (e->region) {  // ShareX: SwapToolType
            e->SwapToolType();
            return 0;
        }
        e->panning = true;
        e->panStart = {(short)LOWORD(l), (short)HIWORD(l)};
        e->panOX = e->offX;
        e->panOY = e->offY;
        e->userZoom = true;
        SetCapture(h);
        return 0;
    case WM_MBUTTONUP:
        e->panning = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEWHEEL: {
        if (e->region) return 0;
        POINT p{(short)LOWORD(l), (short)HIWORD(l)};
        ScreenToClient(h, &p);
        e->Zoom(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1.2f : 1 / 1.2f, p.x, p.y);
        return 0;
    }
    case WM_KEYDOWN:
        if (e->OnKey(w)) return 0;
        break;
    case WM_SYSKEYDOWN:
        if ((w == VK_LEFT || w == VK_RIGHT || w == VK_UP || w == VK_DOWN) && e->OnKey(w)) return 0;
        if (w == VK_MENU || w == VK_F10) {
            if (e->region || e->drawing) e->Invalidate();  // Alt shows the snap sizes
            return 0;
        }
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (e->region) e->Invalidate();
        if (w == VK_MENU) return 0;
        break;
    case WM_CHAR:
        e->OnChar((wchar_t)w);
        return 0;
    case WM_CLOSE:
        if (e->region) {
            e->CancelRegion();
            return 0;
        }
        e->CommitText();
        if (e->dirty) {
            int r = MessageBoxW(h, Tr(L"Сохранить изменения?", L"Save changes?"), L"Zipshot", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES && !e->Save()) return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        if (e && !e->region && !g_quietEditor) {
            e->o.lastEditorTool = e->tool;
            e->o.Save();
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        delete e;
        TrimMemory();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Test windows must belong to this thread; a user may have another editor open.
HWND OwnEditorWindow() {
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM context) -> BOOL {
        wchar_t cls[64]{};
        GetClassNameW(h, cls, (int)std::size(cls));
        if (wcscmp(cls, L"ZipshotEditor") != 0) return TRUE;
        *reinterpret_cast<HWND*>(context) = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

}  // namespace

bool RegionLightNodes() {
    static bool loaded = false;
    if (!loaded) {  // before any editor/region window has loaded the options
        g_lightNodes = GetPrivateProfileIntW(L"region", L"UseLightResizeNodes", 0, (ExeDir() + L"\\zipshot.ini").c_str()) != 0;
        loaded = true;
    }
    return g_lightNodes;
}

void OpenEditor(ImagePtr img, const std::wstring& path) {
    if (!img || img->empty()) return;
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = g_inst;
        wc.hIcon = g_icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ZipshotEditor";
        RegisterClassW(&wc);
        reg = true;
    }
    auto* e = new Editor;
    e->img = img;
    e->path = path;
    e->o.Load();
    if (!IsRegion((Tool)e->o.lastEditorTool) && e->o.lastEditorTool >= 0 && e->o.lastEditorTool < T_Count) e->tool = (Tool)e->o.lastEditorTool;
    // Size to fit the image on the monitor under the cursor (ShareX "auto size" start mode).
    POINT cp;
    GetCursorPos(&cp);
    HMONITOR mon = MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;
    UINT dpi = MonitorDpi(mon);
    e->ui = dpi / 96.0f;
    e->tipFont = Font(L"Segoe UI", 12 * e->ui, false);
    int minW = (int)(620 * e->ui);
    int cw = std::max(minW, img->w + (int)(20 * e->ui)), ch = img->h + (int)(52 * e->ui);
    RECT r{0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int ww = std::min<int>(r.right - r.left, (wa.right - wa.left) * 95 / 100);
    int wh = std::min<int>(r.bottom - r.top, (wa.bottom - wa.top) * 95 / 100);
    int x = wa.left + ((wa.right - wa.left) - ww) / 2, y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    if (g_quietEditor) x = y = -12000;
    HWND h = CreateWindowExW(0, L"ZipshotEditor", L"Zipshot", WS_OVERLAPPEDWINDOW, x, y, ww, wh, nullptr, nullptr, g_inst, e);
    if (!h) { delete e; return; }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    e->UpdateTitle();
    if (g_quietEditor) {
        ShowWindow(h, SW_SHOWNOACTIVATE);
        return;
    }
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

bool EditorTest(const std::wstring& in, const std::wstring& out) {
    auto img = std::make_shared<Image>();
    if (!LoadImageFile(in, *img)) return false;
    auto* e = new Editor;
    e->img = img;
    float w = (float)img->w, h = (float)img->h;
    auto add = [&](Tool t, std::vector<D2D1_POINT_2F> pts, std::wstring text = L"") -> Shape& {
        e->tool = t;
        Shape s = e->NewShape(pts[0]);
        s.pts = pts;
        s.text = text;
        if (t == T_SmartEraser) s.color2 = Rgb(40, 40, 40);
        if (IsFreehand(t)) Editor::BuildGeo(s);
        Editor::UpdateTextBox(s);
        e->shapes.push_back(std::move(s));
        e->RenumberSteps();
        return e->shapes.back();
    };
    std::wstring base = out.substr(0, out.size() - 4);
    // Rectangle: dashed border, corner radius; ellipse: dotted, filled, no shadow.
    e->o.borderStyle = 1;
    e->o.cornerRadius = 12;
    add(T_Rect, {{w * .05f, h * .08f}, {w * .3f, h * .35f}});
    e->o.borderStyle = 2;
    e->o.fill = Rgb(0, 120, 255, 255);
    e->o.shadow = 0;
    add(T_Ellipse, {{w * .33f, h * .08f}, {w * .48f, h * .35f}});
    e->o.fill = kClear;
    e->o.shadow = 1;
    e->o.borderStyle = 0;
    // Arrow curved through a dragged centre point, heads at both ends; plain line with a straight centre point.
    e->o.arrowDir = 2;
    Shape& ar = add(T_Arrow, {{w * .6f, h * .75f}, {w * .62f, h * .5f}, {w * .4f, h * .45f}});
    ar.centerActive = true;
    e->o.arrowDir = 0;
    e->o.borderStyle = 3;
    add(T_Line, {{w * .05f, h * .9f}, {w * .175f, h * .925f}, {w * .3f, h * .95f}});
    e->o.borderStyle = 0;
    std::vector<D2D1_POINT_2F> pen;
    for (int i = 0; i < 60; ++i) pen.push_back({w * .05f + i * w * .004f, h * .7f + std::sin(i * .3f) * h * .04f});
    e->o.arrowDir = 2;
    add(T_FreehandArrow, pen);
    e->o.arrowDir = 0;
    // Text outline: bold italic underline; text background: two lines, right aligned in a resized box.
    e->o.outlineStyle.bold = e->o.outlineStyle.italic = e->o.outlineStyle.underline = 1;
    e->o.outlineFont = 20;
    add(T_TextOutline, {{w * .05f, h * .42f}}, L"Текст с обводкой");
    e->o.textStyle.alignH = 2;
    Shape& tb = add(T_TextBackground, {{w * .05f, h * .52f}}, L"Текст на фоне\nвторая строка");
    tb.autoSize = false;
    tb.pts[1] = {tb.pts[1].x + 60, tb.pts[1].y + 20};
    e->o.textStyle.alignH = 1;
    Shape& sb = add(T_SpeechBalloon, {{w * .33f, h * .4f}}, L"Speech balloon");
    sb.tail = {sb.pts[0].x - 30, sb.pts[1].y + 50};
    // Steps: numbers, letters, roman numerals; the second has its tail dragged out.
    add(T_Step, {{w * .55f, h * .2f}});
    e->o.stepType = 1;
    Shape& st = add(T_Step, {{w * .6f, h * .2f}});
    st.tail = {w * .64f, h * .3f};
    st.tailActive = true;
    e->o.stepType = 3;
    e->o.startStep = 1;
    add(T_Step, {{w * .65f, h * .2f}});
    e->o.stepType = 0;
    add(T_Blur, {{w * .7f, h * .05f}, {w * .95f, h * .3f}});
    add(T_Pixelate, {{w * .7f, h * .35f}, {w * .95f, h * .55f}});
    add(T_Highlight, {{w * .35f, h * .6f}, {w * .6f, h * .65f}});
    add(T_Magnify, {{w * .75f, h * .65f}, {w * .95f, h * .9f}});
    add(T_SmartEraser, {{w * .62f, h * .85f}, {w * .72f, h * .95f}});
    // Image shapes: a scaled copy of the image, the first sticker of the pack, a cursor.
    Image part = sx::CropPremul(*img, {0, 0, img->w / 3, img->h / 3});
    e->InsertImage(sx::ResizeLimit(part, (int)(std::min(w, h) * .25f)), {w * .5f, h * .82f}, true, T_Image);
    std::vector<std::wstring> stickers = sx::StickerFiles(sx::StickerFolder());
    Image sticker;
    if (!stickers.empty() && e->LoadSticker(stickers[std::min<size_t>(5, stickers.size() - 1)], sticker))
        e->InsertImage(std::move(sticker), {w * .9f, h * .45f}, true, T_Sticker);
    e->InsertImage(sx::CursorImage(3), {w * .52f, h * .12f}, false, T_Cursor);
    Image res = e->Export();
    bool ok = !res.empty() && EncodeImage(res, out, false);
    // Cut out a vertical band with torn edges, then undo it: the image size must come back.
    e->o.cutType = 2;
    e->o.cutSize = 12;
    e->cutA = {w * .4f, h * .1f};
    e->cutB = {w * .55f, h * .2f};
    e->ApplyCutOut();
    Image cut = e->Export();
    EncodeImage(cut, base + L"_cut.png", false);
    e->o.cutType = 3;  // and a horizontal band (wave) on top of it
    e->cutA = {w * .1f, h * .58f};
    e->cutB = {w * .15f, h * .68f};
    e->ApplyCutOut();
    EncodeImage(e->Export(), base + L"_cut2.png", false);
    e->Restore(e->undo, e->redo);
    e->Restore(e->undo, e->redo);
    Image undone = e->Export();
    EncodeImage(undone, base + L"_undo.png", false);
    Log(L"EditorTest: %dx%d, cut %dx%d, undo %dx%d", res.w, res.h, cut.w, cut.h, undone.w, undone.h);
    ok = ok && cut.w < res.w && undone.w == res.w && undone.h == res.h;
    delete e;
    return ok;
}

// Places animated stickers (NotoAnimated) on `in`, one resized with its nodes, and exports an animated GIF.
bool GifExportTest(const std::wstring& in, const std::wstring& out) {
    auto img = std::make_shared<Image>();
    if (!LoadImageFile(in, *img)) return false;
    auto* e = new Editor;
    e->img = img;
    float w = (float)img->w, h = (float)img->h;
    std::wstring dir = sx::StickersRoot() + L"\\NotoAnimated";
    std::vector<std::wstring> gifs;
    for (const wchar_t* n : {L"1f600.gif", L"1f525.gif", L"2764.gif", L"1f389.gif"})
        if (GetFileAttributesW((dir + L"\\" + n).c_str()) != INVALID_FILE_ATTRIBUTES) gifs.push_back(dir + L"\\" + n);
    for (auto& f : sx::StickerFiles(dir))
        if (gifs.size() < 3 && std::find(gifs.begin(), gifs.end(), f) == gifs.end()) gifs.push_back(f);
    if (gifs.size() < 2) {
        Log(L"GifExportTest: no animated stickers in %s", dir.c_str());
        delete e;
        return false;
    }
    e->o.stickerSize = 128;
    e->tool = T_Sticker;
    D2D1_POINT_2F at[] = {{w * .25f, h * .5f}, {w * .5f, h * .4f}, {w * .75f, h * .55f}};
    for (size_t i = 0; i < gifs.size() && i < 3; ++i) {
        Image im;
        std::shared_ptr<const sx::Anim> anim;
        if (!e->LoadSticker(gifs[i], im, &anim) || !anim) continue;
        e->InsertImage(std::move(im), at[i], true, T_Sticker, anim, gifs[i]);
        Log(L"GifExportTest: %s %d frames %d ms", gifs[i].c_str(), (int)anim->frames.size(), anim->total);
    }
    if (e->shapes.size() >= 2) {  // the second one dragged bigger by its bottom-right node
        Shape& s = e->shapes[1];
        s.pts[1] = {s.pts[1].x + 64, s.pts[1].y + 64};
    }
    e->tool = T_TextBackground;
    Shape t = e->NewShape({w * .05f, h * .08f});
    t.text = L"GIF: stickers stacked";
    Editor::UpdateTextBox(t);
    e->shapes.push_back(t);
    std::wstring err;
    bool ok = e->ExportGif(out, err);
    long long len = 0;
    size_t planned = e->GifTimeline(len).size();
    // The canvas timer: on while an animated sticker is in a visible editor, off once it is gone.
    g_quietEditor = true;
    OpenEditor(img, in);
    if (HWND hw = OwnEditorWindow()) {
        auto* we = (Editor*)GetWindowLongPtrW(hw, GWLP_USERDATA);
        LARGE_INTEGER pf, pa, pb;
        QueryPerformanceFrequency(&pf);
        QueryPerformanceCounter(&pa);
        for (int k = 0; k < 10; ++k) {
            we->Invalidate();
            UpdateWindow(hw);
        }
        QueryPerformanceCounter(&pb);
        Log(L"GifExportTest: editor paint without stickers %.1f ms", (pb.QuadPart - pa.QuadPart) * 100.0 / pf.QuadPart);
        we->shapes = e->shapes;
        we->Invalidate();
        int paints = 0;
        long long lastSig = -2;
        for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 600;) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
            if (we->animSig != lastSig) ++paints, lastSig = we->animSig;
            Sleep(5);
        }
        bool onWith = we->animTimer;
        LARGE_INTEGER qf, qa, qb;
        QueryPerformanceFrequency(&qf);
        QueryPerformanceCounter(&qa);
        for (int k = 0; k < 10; ++k) {
            we->Invalidate();
            UpdateWindow(hw);
        }
        QueryPerformanceCounter(&qb);
        Log(L"GifExportTest: editor paint with 3 animated stickers %.1f ms (off-screen, includes present)",
            (qb.QuadPart - qa.QuadPart) * 100.0 / qf.QuadPart);
        we->shapes.clear();
        we->Invalidate();
        UpdateWindow(hw);
        Log(L"GifExportTest: canvas timer %s with stickers (%d ticks, %d distinct frames painted in 600 ms off-screen), %s without",
            onWith ? L"on" : L"OFF", we->animTicks, paints, we->animTimer ? L"STILL ON" : L"off");
        ok = ok && onWith && !we->animTimer && we->animTicks > 4 && paints > 2;
        we->dirty = false;
        DestroyWindow(hw);
    }
    delete e;
    sx::Anim check;
    bool anim = ok && sx::LoadAnimation(out, check);
    Log(L"GifExportTest: %s, planned %d frames / %lld ms, decoded %d frames / %d ms", ok ? L"ok" : err.c_str(), (int)planned, len,
        (int)check.frames.size(), check.total);
    if (anim) {  // first and middle frame as PNG for a look
        std::wstring base = out.substr(0, out.size() - 4);
        for (size_t k : {(size_t)0, check.frames.size() / 2}) {
            Image f = *check.frames[k];
            for (auto& px : f.px) {
                uint32_t a = px >> 24;
                if (a && a != 255)
                    px = a << 24 | std::min(255u, (px >> 16 & 255) * 255 / a) << 16 | std::min(255u, (px >> 8 & 255) * 255 / a) << 8 |
                         std::min(255u, (px & 255) * 255 / a);
            }
            EncodeImage(f, base + L"_frame" + std::to_wstring(k) + L".png", false);
        }
    }
    return anim && check.frames.size() >= 2;
}

// Opens a real editor window off-screen without activation and grabs it with PrintWindow,
// so the toolbar can be checked without disturbing whoever uses the desktop.
bool EditorWindowTest(const std::wstring& in, const std::wstring& out) {
    auto img = std::make_shared<Image>();
    if (!LoadImageFile(in, *img)) return false;
    g_quietEditor = true;
    OpenEditor(img, in);
    HWND h = OwnEditorWindow();
    if (!h) return false;
    SetWindowPos(h, nullptr, -12000, -12000, 1500, 900, SWP_NOZORDER | SWP_NOACTIVATE);
    auto* e = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
    e->ui = 1.5f;  // the main monitor's scale
    e->tipFont = Font(L"Segoe UI", 12 * e->ui, false);
    auto grab = [&](const std::wstring& file) {
        InvalidateRect(h, nullptr, FALSE);
        UpdateWindow(h);
        RECT r;
        GetWindowRect(h, &r);
        int w = r.right - r.left, hh = r.bottom - r.top;
        void* bits;
        HBITMAP bmp = CreateDib(w, hh, &bits);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(dc, bmp);
        PrintWindow(h, dc, PW_RENDERFULLCONTENT);
        GdiFlush();
        Image shot;
        shot.w = w;
        shot.h = hh;
        shot.px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)w * hh);
        SelectObject(dc, old);
        DeleteDC(dc);
        DeleteObject(bmp);
        return EncodeImage(shot, file, false);
    };
    // 1: a speech balloon selected (8 box nodes + tail node), tooltip on its toolbar button.
    e->tool = T_SpeechBalloon;
    e->LayoutAll();
    D2D1_RECT_F c = e->Crop();
    float w = c.right - c.left, hh = c.bottom - c.top;
    Shape b = e->NewShape({w * .3f, hh * .3f});
    b.text = L"Speech balloon";
    Editor::UpdateTextBox(b);
    b.tail = {w * .25f, hh * .55f};
    e->shapes.push_back(b);
    e->tool = T_Arrow;
    Shape a = e->NewShape({w * .55f, hh * .7f});
    a.pts = {{w * .55f, hh * .7f}, {w * .7f, hh * .45f}, {w * .85f, hh * .6f}};
    a.centerActive = true;
    e->shapes.push_back(a);
    e->tool = T_SpeechBalloon;
    e->selected = 0;
    for (int i = 0; i < (int)e->buttons.size(); ++i)
        if (e->buttons[i].kind == B_Tool && e->buttons[i].idx == T_SpeechBalloon) e->hoverBtn = i;
    e->showTip = true;
    bool ok = grab(out);
    // 2: the curved arrow selected (end and centre nodes), a cut out band waiting for confirmation.
    e->showTip = false;
    e->hoverBtn = -1;
    e->tool = T_CutOut;
    e->LayoutToolbar();
    e->selected = 1;
    e->cutA = {w * .1f, hh * .2f};
    e->cutB = {w * .2f, hh * .3f};
    e->cutPending = e->CutValid();
    ok = grab(out.substr(0, out.size() - 4) + L"_nodes.png") && ok;
    e->dirty = false;
    DestroyWindow(h);
    return ok;
}
// Reproducible product footage: real editor renders from an explicit synthetic input.
// Never reads the desktop or clipboard and never activates or moves user windows.
bool EditorDemoTest(const std::wstring& in, const std::wstring& directory) {
    auto img = std::make_shared<Image>();
    if (!LoadImageFile(in, *img)) return false;
    CreateDirectoryW(directory.c_str(), nullptr);
    g_quietEditor = true;
    OpenEditor(img, in);
    HWND h = OwnEditorWindow();
    if (!h) return false;
    SetWindowPos(h, nullptr, -12000, -12000, 1500, 940, SWP_NOZORDER | SWP_NOACTIVATE);
    auto* e = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
    e->ui = 1.25f;
    e->tool = T_Select;
    e->LayoutAll();
    e->o.border = Rgb(71, 220, 193);
    e->o.borderSize = 4;
    e->o.shadow = 0;
    auto grab = [&](int step) {
        e->LayoutToolbar();
        InvalidateRect(h, nullptr, FALSE);
        UpdateWindow(h);
        RECT r{};
        GetWindowRect(h, &r);
        void* bits = nullptr;
        HBITMAP bmp = CreateDib(r.right-r.left, r.bottom-r.top, &bits);
        HDC dc = CreateCompatibleDC(nullptr);
        if (!bmp || !bits || !dc) { if (bmp) DeleteObject(bmp); if (dc) DeleteDC(dc); return false; }
        HGDIOBJ old = SelectObject(dc, bmp);
        bool painted = PrintWindow(h, dc, PW_RENDERFULLCONTENT) != 0;
        GdiFlush();
        Image shot;
        shot.w = r.right-r.left;
        shot.h = r.bottom-r.top;
        shot.px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)shot.w*shot.h);
        SelectObject(dc, old);
        DeleteDC(dc);
        DeleteObject(bmp);
        return painted && EncodeImage(shot, directory + L"\\editor-" + std::to_wstring(step) + L".png", false);
    };
    float w = (float)img->w, hh = (float)img->h;
    auto add = [&](Tool tool, D2D1_POINT_2F from, D2D1_POINT_2F to) -> Shape& {
        e->tool = tool;
        Shape s = e->NewShape(from);
        s.pts = {from, to};
        e->shapes.push_back(std::move(s));
        e->selected = (int)e->shapes.size()-1;
        return e->shapes.back();
    };
    bool ok = grab(0);
    add(T_Rect, {w*.195f, hh*.23f}, {w*.435f, hh*.45f});
    ok = grab(1) && ok;
    add(T_Arrow, {w*.57f, hh*.61f}, {w*.36f, hh*.39f});
    ok = grab(2) && ok;
    add(T_Blur, {w*.72f, hh*.755f}, {w*.92f, hh*.83f});
    ok = grab(3) && ok;
    e->o.textFont = 24;
    e->o.textFill = Rgb(19, 104, 99);
    Shape& text = add(T_TextBackground, {w*.48f, hh*.63f}, {w*.75f, hh*.73f});
    text.text = L"Ready to share";
    Editor::UpdateTextBox(text);
    e->tool = T_Select;
    e->selected = -1;
    ok = grab(4) && ok;
    ok = EncodeImage(e->Export(), directory + L"\\result.png", false) && ok;
    e->dirty = false;
    DestroyWindow(h);
    g_quietEditor = false;
    return ok;
}

bool RegionCapture(ImagePtr screen, RECT vs, Image& out, RECT& rect) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"ZipshotRegion";
        RegisterClassW(&wc);
        reg = true;
    }
    RegionResult res;
    auto* e = new Editor;
    e->region = true;
    e->result = &res;
    e->img = screen;
    e->vs = vs;
    e->o.Load();
    e->tool = IsRegion((Tool)e->o.lastRegionTool) ? (Tool)e->o.lastRegionTool : T_RegionRect;
    if (e->o.rememberMenu) e->gripDX = e->o.menuDX, e->gripDY = e->o.menuDY;  // ShareX RememberMenuState
    e->fpsTick = GetTickCount();
    e->wins = VisibleWindows();
    POINT cp;
    GetCursorPos(&cp);
    HMONITOR mon = MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    e->mon = mi.rcMonitor;
    OffsetRect(&e->mon, -vs.left, -vs.top);
    UINT dpi = MonitorDpi(mon);
    e->ui = dpi / 96.0f;
    e->tipFont = Font(L"Segoe UI", 12 * e->ui, false);
    e->infoFont = Font(L"Verdana", 12 * e->ui, false);
    e->cursor = {cp.x - vs.left, cp.y - vs.top};
    e->UpdateHover();
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ZipshotRegion", L"Zipshot", WS_POPUP, vs.left, vs.top,
                             vs.right - vs.left, vs.bottom - vs.top, nullptr, nullptr, g_inst, e);
    if (!h) {
        delete e;
        return false;
    }
    e->LayoutAll();
    if (D2D1_RECT_F bar = e->BarRect(); bar.left < 0 || bar.top < 0 || bar.right > vs.right - vs.left || bar.bottom > vs.bottom - vs.top) {
        e->gripDX = e->gripDY = 0;  // remembered spot is off the current screens
        e->LayoutAll();
    }
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(h);
    SetTimer(h, 2, e->o.showFps ? 16 : 50, nullptr);
    // Hotkeys keep working in here: a nested capture (a screenshot of this screen) runs on top.
    MSG msg;
    bool quit = false;
    while (!res.done) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            quit = true;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(h)) {
        if (IsRegion(e->tool)) e->o.lastRegionTool = e->tool;
        else if (e->tool != T_Select) e->o.lastAnnotationTool = e->tool;
        if (e->o.rememberMenu) e->o.menuDX = e->gripDX, e->o.menuDY = e->gripDY;
        e->o.Save();
        DestroyWindow(h);
    }
    if (quit) PostQuitMessage(0);
    if (!res.ok) return false;
    out = std::move(res.image);
    rect = res.rect;
    return true;
}

// Region surface rendered off-screen with a fake cursor, a region being dragged and one stroke, then
// grabbed with PrintWindow: lets the look be checked without taking over the desktop.
bool RegionWindowTest(const std::wstring& out) {
    RECT vs = VirtualScreen();
    auto screen = std::make_shared<Image>(CaptureRect(vs, false));
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = g_inst;
        wc.lpszClassName = L"ZipshotRegionTest";
        RegisterClassW(&wc);
        reg = true;
    }
    RegionResult res;
    auto* e = new Editor;
    e->region = true;
    e->result = &res;
    e->img = screen;
    e->vs = vs;
    e->o.Load();
    e->tool = T_RegionRect;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    e->mon = mi.rcMonitor;
    OffsetRect(&e->mon, -vs.left, -vs.top);
    e->ui = 1.5f;
    e->tipFont = Font(L"Segoe UI", 12 * e->ui, false);
    e->infoFont = Font(L"Verdana", 12 * e->ui, false);
    int ox = -vs.left;
    Shape pen = e->NewShape({0, 0});
    pen.tool = T_Freehand;
    pen.color = e->o.border;
    pen.size = e->o.borderSize;
    pen.pts.clear();
    for (int i = 0; i < 80; ++i) pen.pts.push_back({(float)(ox + 300 + i * 8), (float)(700 + std::sin(i * .25f) * 60)});
    Editor::BuildGeo(pen);
    e->shapes.push_back(pen);
    e->creatingRegion = true;
    e->regionShape = Shape{T_RegionRect};
    e->regionShape.pts = {{(float)(ox + 250), 500.f}, {(float)(ox + 1300), 1000.f}};
    e->cursor = {ox + 1300, 1000};
    // ZIPSHOT_TEST_REGION_EXTRAS=1: multi region mode with two finished regions (one selected), crosshair and FPS.
    wchar_t extras[8] = {};
    if (GetEnvironmentVariableW(L"ZIPSHOT_TEST_REGION_EXTRAS", extras, 8) && extras[0] == L'1') {
        e->o.quickCrop = false;
        e->o.crosshair = true;
        e->o.showFps = true;
        e->fps = 60;
        Shape a{T_RegionRect}, b{T_RegionEllipse};
        a.pts = {{(float)(ox + 1450), 150.f}, {(float)(ox + 1850), 450.f}};
        b.pts = {{(float)(ox + 1500), 550.f}, {(float)(ox + 1900), 900.f}};
        e->regions = {a, b, e->regionShape};  // the dragged one finished too, so the selected region shows its nodes
        e->creatingRegion = false;
        e->selRegion = 0;
    }
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"ZipshotRegionTest", L"", WS_POPUP, -30000, -30000,
                             vs.right - vs.left, vs.bottom - vs.top, nullptr, nullptr, g_inst, e);
    if (!h) return false;
    e->LayoutAll();
    ShowWindow(h, SW_SHOWNOACTIVATE);
    e->Render();
    int w = vs.right - vs.left, hh = vs.bottom - vs.top;
    void* bits;
    HBITMAP bmp = CreateDib(w, hh, &bits);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bmp);
    PrintWindow(h, dc, PW_RENDERFULLCONTENT);
    GdiFlush();
    Image shot;
    shot.w = w;
    shot.h = hh;
    shot.px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)w * hh);
    for (auto& p : shot.px) p |= 0xFF000000u;
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bmp);
    // Also check the QuickCrop result of that region (multi region mode: all regions).
    if (e->o.quickCrop) e->FinishRegion(e->regionShape);
    else e->CaptureRegions();
    std::wstring cropOut = out.substr(0, out.size() - 4) + L"_crop.png";
    EncodeImage(res.image, cropOut, false);
    DestroyWindow(h);
    return EncodeImage(shot, out, false);
}

// App icon (tray, title bars) from the same icon set.
HICON LoadAppIcon(int px) {
    const IconData& d = Icon(133, px);
    if (!d.w) return nullptr;
    void* bits;
    HBITMAP color = CreateDib(d.w, d.h, &bits);
    auto* p = (uint32_t*)bits;
    for (size_t i = 0; i < d.px.size(); ++i) {  // icons want straight alpha
        uint32_t c = d.px[i], a = c >> 24;
        p[i] = a == 0 ? 0 : a << 24 | std::min(255u, (c >> 16 & 255) * 255 / a) << 16 | std::min(255u, (c >> 8 & 255) * 255 / a) << 8 |
                                std::min(255u, (c & 255) * 255 / a);
    }
    HBITMAP mask = CreateBitmap(d.w, d.h, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// --test-imageops (part 2): a real editor window off-screen, shapes on the image, every geometric Image
// menu operation, then undo all and redo all; exports each state and checks undo restores the pixels.
bool EditorImageOpsTest(const std::wstring& in, const std::wstring& outDir) {
    auto img = std::make_shared<Image>();
    if (!LoadImageFile(in, *img)) return false;
    g_quietEditor = true;
    OpenEditor(img, in);
    HWND h = nullptr;  // our own thread's editor, never another process's window
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM lp) -> BOOL {
        wchar_t cls[64];
        GetClassNameW(w, cls, 64);
        if (wcscmp(cls, L"ZipshotEditor") != 0) return TRUE;
        *(HWND*)lp = w;
        return FALSE;
    }, (LPARAM)&h);
    if (!h) return false;
    SetWindowPos(h, nullptr, -12000, -12000, 1400, 900, SWP_NOZORDER | SWP_NOACTIVATE);
    auto* e = (Editor*)GetWindowLongPtrW(h, GWLP_USERDATA);
    float w = (float)img->w, hh = (float)img->h;
    auto add = [&](Tool t, std::vector<D2D1_POINT_2F> pts, std::wstring text = L"") {
        e->tool = t;
        Shape s = e->NewShape(pts[0]);
        s.pts = pts;
        s.text = text;
        if (t == T_Step) s.step = 1;
        if (IsFreehand(t)) Editor::BuildGeo(s);
        e->shapes.push_back(std::move(s));
    };
    add(T_Rect, {{w * .1f, hh * .1f}, {w * .4f, hh * .4f}});
    add(T_Arrow, {{w * .6f, hh * .7f}, {w * .85f, hh * .2f}});
    add(T_TextBackground, {{w * .1f, hh * .6f}}, L"Zipshot");
    add(T_Step, {{w * .5f, hh * .5f}});
    add(T_Blur, {{w * .45f, hh * .05f}, {w * .7f, hh * .25f}});
    std::vector<D2D1_POINT_2F> pen;
    for (int i = 0; i < 40; ++i) pen.push_back({w * .1f + i * w * .006f, hh * .85f + std::sin(i * .4f) * hh * .04f});
    add(T_Freehand, pen);
    e->tool = T_Select;
    bool ok = true;
    auto save = [&](const wchar_t* name) {
        Image out = e->Export();
        bool r = !out.empty() && EncodeImage(out, outDir + L"\\" + name + L".png", false);
        Log(L"test-imageops: %s %dx%d %s", name, out.w, out.h, r ? L"ok" : L"FAILED");
        ok &= r;
        return out;
    };
    Image start = save(L"doc_0_start");
    ImagePtr original = e->img;
    e->RotateImage(1);
    save(L"doc_1_rot90cw");
    e->FlipImage(true);
    save(L"doc_2_fliph");
    e->ResizeImage(e->img->w / 2, e->img->h / 2, RS_HighQualityBicubic);
    save(L"doc_3_resize50");
    e->CanvasImage(30, 30, 30, 30, 0xFF2060C0);
    save(L"doc_4_canvas");
    std::vector<Effect> fx{MakeEffect(E_RoundedCorners), MakeEffect(E_Shadow)};
    e->EffectsImage(fx);
    Image last = save(L"doc_5_effects");
    e->AutoCrop();
    save(L"doc_6_autocrop");
    size_t steps = e->undo.size();
    for (size_t i = 0; i < steps; ++i) e->Restore(e->undo, e->redo);
    Image back = save(L"doc_7_undo_all");
    bool same = back.w == start.w && back.h == start.h && back.px == start.px && e->img == original;
    Log(L"test-imageops: undo %zu steps back to the start: %s", steps, same ? L"identical" : L"DIFFERENT");
    ok &= same;
    for (size_t i = 0; i < steps; ++i) e->Restore(e->redo, e->undo);
    Image again = save(L"doc_8_redo_all");
    // Redo ends on the auto crop; drop it to compare with the effects state.
    e->Restore(e->undo, e->redo);
    Image fxAgain = e->Export();
    bool redoSame = fxAgain.w == last.w && fxAgain.h == last.h && fxAgain.px == last.px;
    Log(L"test-imageops: redo back to the effects state: %s", redoSame ? L"identical" : L"DIFFERENT");
    ok &= redoSame;
    // Memory: history entries share bitmaps; count the distinct ones.
    std::vector<Image*> distinct;
    for (auto* list : {&e->undo, &e->redo})
        for (auto& he : *list)
            if (std::find(distinct.begin(), distinct.end(), he.image.get()) == distinct.end()) distinct.push_back(he.image.get());
    Log(L"test-imageops: history %zu entries, %zu distinct bitmaps", e->undo.size() + e->redo.size(), distinct.size());
    e->dirty = false;
    DestroyWindow(h);
    return ok;
}
