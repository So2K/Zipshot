#pragma once
#include "common.h"

// ShareX "Image combiner": stack screenshots into one picture.

struct CombineOptions {
    bool horizontal = false;  // ShareX default: Vertical
    int alignment = 0;        // 0 LeftOrTop, 1 Center, 2 RightOrBottom
    int space = 0;            // px between images
    int wrapAfter = 0;        // > 0: new row/column every N images
    bool autoFill = true;     // fill gaps with the first image's bottom-right pixel, else transparent
};
CombineOptions LoadCombineOptions();  // zipshot.ini [combiner]
void SaveCombineOptions(const CombineOptions& o);

// Layout of ImageHelpers.CombineImages: one rect per size, `canvas` = union.
std::vector<RECT> CombineLayout(const std::vector<SIZE>& sizes, bool horizontal, int alignment, int space, int wrapAfter,
                                SIZE& canvas);
// Area-averaging resize (BGRA, premultiplied or opaque).
Image ScaleImage(const Image& src, int w, int h);

// Loads `paths` (WIC, off the UI thread), combines, saves a new capture file, then on the UI thread copies it
// (AfterCopy), plays the done sound, adds it to the main window, calls `onSaved` and shows the toast (AfterToast).
void CombineFilesAsync(std::vector<std::wstring> paths, CombineOptions opt, std::function<void(const std::wstring&)> onSaved);

// Modal dark dialog with a live preview. Returns true (and the possibly reordered `paths`) on Combine.
bool ShowCombineDialog(HWND owner, std::vector<std::wstring>& paths, CombineOptions& opt);

// Test seam: the main window's registered IDropTarget (nullptr when closed).
struct IDropTarget;
IDropTarget* MainWindowDropTargetForTest();

Image CombineImages(const std::vector<Image>& images, bool horizontal, int alignment, int space, int wrapAfter, bool autoFill);
