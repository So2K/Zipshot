#pragma once
#include "common.h"
#include <d2d1.h>
#include <string>
#include <vector>

// ShareX annotation pieces that do not need the editor's internals (editor.cpp keeps the shape model):
// image loading for image/sticker/cursor shapes, the clipboard, step labels, the cut-out tool's image
// operation, curved lines, and the sticker picker popup.
namespace shapes_ext {

// Shape images are premultiplied BGRA; `alpha` is set when any pixel is not opaque.
bool LoadImagePremul(const std::wstring& path, Image& out);
Image ResizeLimit(const Image& premul, int size);  // ImageHelpers.ResizeImageLimit (keeps aspect, never upscales)
Image PremulCopy(const Image& straight);           // straight (document) pixels -> premultiplied
Image CropPremul(const Image& straight, RECT r);   // a region of the document as a shape image

// ShapeManager.PasteFromClipboard: an image (PNG, DIBV5, DIB) or the first image file in a file drop list.
bool ClipboardImage(HWND owner, Image& premul);
bool ClipboardText(HWND owner, std::wstring& text);

// Cursor tool: ShareX Helpers.CursorList (system cursors), default index 3 (Default).
int CursorTypeCount();
const wchar_t* CursorTypeName(int i);
Image CursorImage(int i);

// StepType: Numbers, LettersUppercase, LettersLowercase, RomanNumeralsUppercase, RomanNumeralsLowercase.
std::wstring StepLabel(int number, int type);

// ImageHelpers.CutOutBitmapMiddle: removes [start, start + size) along x (horizontal) or y and joins the
// two parts; effect 0 None, 1 ZigZag, 2 TornEdge, 3 Wave on the joined edges, `bg` behind them.
// Straight alpha in and out.
Image CutOutMiddle(const Image& src, bool horizontal, int start, int size, int effect, int effectSize, D2D1_COLOR_F bg);

// GDI+ DrawCurve (cardinal spline, tension 0.5) through `pts` as cubic Bezier segments:
// p0, c1, c2, p1, c1, c2, p2 ... (1 + 3 * (n - 1) points).
std::vector<D2D1_POINT_2F> CurveBeziers(const std::vector<D2D1_POINT_2F>& pts);

// ---- stickers (stickers.cpp) ----
// Packs live in folders under StickersRoot(): ZIPSHOT_STICKERS_ROOT, [stickers] Root, <exe>\Stickers.
// A pack folder may carry pack.ini ([pack] name, name_ru, short, order, animated) and index.tsv
// (file, name, group, keywords) written by tools/fetch_stickers.py; plain folders of images work too.
// Custom ("My stickers") is <root>\Custom; Recent (32) is kept in zipshot.ini [stickers] Recent.
std::wstring StickersRoot();
std::wstring CustomStickerFolder();  // created on demand
// The BlobEmoji pack ([annotation] StickerFolder, <root>\BlobEmoji or ShareX's).
std::wstring StickerFolder();
std::vector<std::wstring> StickerFiles(const std::wstring& folder);
// Popup picker (pack tabs, categories, search, my stickers, recent), anchored at `at` (screen px). `size` is
// StickerSize and can be changed in the popup. False if cancelled.
bool PickSticker(HWND owner, POINT at, float ui, std::wstring& path, int& size);
// Adds to My stickers: a copy of an image file (animated GIFs stay animated) or a premultiplied image as PNG.
// Returns the new file or "".
std::wstring AddCustomStickerFile(const std::wstring& file);
std::wstring AddCustomStickerImage(const Image& premul);

// Animated images (GIF): every frame composited (offsets, disposal, transparency), premultiplied BGRA.
struct Anim {
    std::vector<std::shared_ptr<Image>> frames;  // all the same size
    std::vector<int> delays;                     // ms per frame
    int total = 0;                               // sum of delays
    int FrameAt(long long ms) const;
};
// False unless the file has two or more frames. `maxFrames` caps long animations (the rest is dropped).
bool LoadAnimation(const std::wstring& path, Anim& out, int maxFrames = 500);
std::shared_ptr<Anim> ResizeAnim(const Anim& a, int size);  // ResizeLimit on every frame

// Font families offered for the text tools: common installed ones plus `current`.
std::vector<std::wstring> FontChoices(const std::wstring& current);

}  // namespace shapes_ext
