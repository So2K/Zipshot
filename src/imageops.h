#pragma once
#include "common.h"
#include <atomic>

// Pixel operations behind the editor's "Image" menu (ShareX ShapeManager image items + ImageEffectsLib).
// All functions take and return straight-alpha BGRA images (see Image in common.h). The result's `alpha`
// flag is set only when some pixel is actually transparent, so opaque results stay JPEG-friendly.

// ShareX ImageInterpolationMode, same order (index is stored in zipshot.ini).
enum Resampling { RS_HighQualityBicubic, RS_Bicubic, RS_HighQualityBilinear, RS_Bilinear, RS_NearestNeighbor, RS_Count };
const wchar_t* ResamplingName(int mode);

Image ImgNew(int w, int h, uint32_t argb);
Image ImgResize(const Image& src, int w, int h, int resampling);
Image ImgCanvas(const Image& src, int left, int top, int right, int bottom, uint32_t argb);  // negative margins crop
Image ImgRotate90(const Image& src, int quarterTurnsCW);                                    // 1, 2 or 3
Image ImgFlip(const Image& src, bool horizontal);
Image ImgCrop(const Image& src, RECT r);
// ShareX FindAutoCropRectangle: trims borders the colour of the top-left (left/top) and bottom-right
// (right/bottom) corner pixels; `tolerance` is per channel (0 = exact, as ShareX).
RECT ImgAutoCropRect(const Image& src, int tolerance);
// Downscaled copy that fits w x h (area average), for previews.
Image ImgThumbnail(const Image& src, int maxW, int maxH);

// ---------- image effects (ShareX ImageEffectsLib subset) ----------
enum EffectType {
    E_Brightness, E_Contrast, E_Saturation, E_Hue, E_Gamma, E_Grayscale, E_Inverse, E_Sepia, E_Polaroid,
    E_Blur, E_Sharpen, E_Pixelate, E_Border, E_Shadow, E_RoundedCorners, E_Outline, E_Reflection,
    E_Canvas, E_Resize, E_Rotate, E_TextWatermark, E_Count
};
enum ParamKind { PK_Float, PK_Int, PK_Bool, PK_Color, PK_Choice, PK_Text };
struct EffectParamDef {
    const wchar_t* ru;
    const wchar_t* en;
    ParamKind kind;
    double def, lo, hi, step;
    const wchar_t* choicesRu = nullptr;  // '|' separated
    const wchar_t* choicesEn = nullptr;
};
constexpr int kMaxEffectParams = 16;
struct EffectDef {
    const wchar_t* key;  // ShareX class name, used in zipshot.ini
    const wchar_t* ru;
    const wchar_t* en;
    int count;
    EffectParamDef params[kMaxEffectParams];
};
const EffectDef& GetEffectDef(int type);

struct Effect {
    int type = 0;
    bool enabled = true;
    double v[kMaxEffectParams]{};  // colours are ARGB stored exactly in the double
    std::wstring text;             // the PK_Text parameter
};
Effect MakeEffect(int type);
std::wstring EffectName(const Effect& e);
std::wstring EffectSummary(const Effect& e);
Image ApplyEffect(const Image& src, const Effect& e);
// Enabled effects in order. Returns an empty image if `cancel` was raised.
Image ApplyEffects(const Image& src, const std::vector<Effect>& stack, const std::atomic<int>* cancel = nullptr, int gen = 0);

// zipshot.ini [effects]: Count, Effect1..N = Key|enabled|v0;v1;...|text
std::vector<Effect> LoadEffectStack();
void SaveEffectStack(const std::vector<Effect>& stack);

// Editor document test (editor.cpp): applies the menu operations to a document with shapes, checks undo.
bool EditorImageOpsTest(const std::wstring& in, const std::wstring& outDir);
