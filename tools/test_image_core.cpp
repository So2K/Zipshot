#include "../src/combine.h"
#include "../src/clipboard_dib.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// Pure pixel/layout regressions. No windows, clipboard access, config, or desktop capture.
void Log(const wchar_t*, ...) {}
HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HICON g_icon = nullptr;
void RunOnMain(std::function<void()>) { std::abort(); }
std::wstring ExeDir() { std::abort(); }
bool IsEnglish() { return true; }
void MainWindowNotifyCapture(const std::wstring&) { std::abort(); }
HBITMAP CreateDib(int, int, void**) { std::abort(); }
bool GetSettingBool(const wchar_t*, bool) { std::abort(); }
std::wstring NewOutputPath(const wchar_t*) { std::abort(); }
bool SaveImageAuto(const Image&, std::wstring&) { std::abort(); }
bool LoadImageFile(const std::wstring&, Image&) { std::abort(); }
void CopyImageToClipboard(const Image&) { std::abort(); }
void PlayUiSound(Sound) { std::abort(); }
void ShowToast(ImagePtr, const std::wstring&, const std::wstring&) { std::abort(); }

static int failures = 0;
static void Check(bool condition, const char* name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
static Image Pixels(int w, int h, bool alpha, std::initializer_list<uint32_t> px) {
    Image im;
    im.w = w; im.h = h; im.alpha = alpha; im.px = px;
    return im;
}

int main() {
    Image translucent = Pixels(2, 1, true, {0x80112233u, 0x00445566u});
    Image opaque = Pixels(1, 1, false, {0x00123456u});
    for (bool fill : {false, true}) {
        Image combined = CombineImages({translucent, opaque}, true, 0, 0, 0, fill);
        Check(combined.w == 3 && combined.h == 1, "combine dimensions without gaps");
        Check(combined.alpha, "combine preserves transparency without gaps");
        Check(combined.px.size() == 3 && combined.px[0] == 0x80112233u && combined.px[1] == 0x00445566u,
              "combine preserves straight alpha colour bytes");
        Check(combined.px.size() == 3 && combined.px[2] == 0xFF123456u, "combine normalizes opaque source alpha");
    }
    Image filled = CombineImages({opaque, opaque}, true, 0, 1, 0, true);
    Check(!filled.alpha && filled.px == std::vector<uint32_t>({0xFF123456u, 0xFF123456u, 0xFF123456u}),
          "autofill uses opaque source colour");
    Image gaps = CombineImages({opaque, opaque}, true, 0, 1, 0, false);
    Check(gaps.alpha && gaps.px == std::vector<uint32_t>({0xFF123456u, 0u, 0xFF123456u}),
          "transparent gap preserves opaque capture pixels");
    Image transparentFill = CombineImages({translucent, opaque}, false, 0, 1, 0, true);
    Check(transparentFill.alpha, "autofill preserves transparent source colour");
    SIZE canvas;
    auto rects = CombineLayout({{2, 1}, {1, 3}, {4, 2}}, true, 1, 2, 2, canvas);
    Check(canvas.cx == 5 && canvas.cy == 7 && rects[0].top == 1 && rects[2].top == 5,
          "wrapped centered layout");
    std::vector<BYTE> dib(sizeof(BITMAPINFOHEADER) + 4, 0), bmp;
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi); bi.biWidth = 1; bi.biHeight = 1; bi.biPlanes = 1; bi.biBitCount = 32;
    std::memcpy(dib.data(), &bi, sizeof(bi));
    WORD bitCount = 0;
    using shapes_ext::detail::WrapClipboardDib;
    Check(WrapClipboardDib(dib.data(), dib.size(), bmp, bitCount) && bitCount == 32 &&
              ((BITMAPFILEHEADER*)bmp.data())->bfOffBits == sizeof(BITMAPFILEHEADER) + sizeof(bi),
          "valid 32bpp DIB wraps with correct offset");
    Check(!WrapClipboardDib(nullptr, 0, bmp, bitCount), "null DIB rejected");
    Check(!WrapClipboardDib(dib.data(), sizeof(bi) - 1, bmp, bitCount), "truncated DIB header rejected");
    bi.biSize = 124;
    std::memcpy(dib.data(), &bi, sizeof(bi));
    Check(!WrapClipboardDib(dib.data(), dib.size(), bmp, bitCount), "oversized DIB header rejected");
    bi.biSize = sizeof(bi); bi.biClrUsed = MAXDWORD;
    std::memcpy(dib.data(), &bi, sizeof(bi));
    Check(!WrapClipboardDib(dib.data(), dib.size(), bmp, bitCount), "overflowing DIB palette rejected");
    bi.biClrUsed = 0; bi.biCompression = BI_BITFIELDS;
    std::memcpy(dib.data(), &bi, sizeof(bi));
    Check(!WrapClipboardDib(dib.data(), dib.size(), bmp, bitCount), "truncated DIB bitfields rejected");
    bi.biCompression = BI_RGB; bi.biBitCount = 8;
    std::memcpy(dib.data(), &bi, sizeof(bi));
    Check(!WrapClipboardDib(dib.data(), dib.size(), bmp, bitCount), "truncated default DIB palette rejected");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
