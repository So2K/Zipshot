#pragma once
#include "imageops.h"

// Dark ShareX-style dialogs for the editor's "Image" menu (NewImageForm, ImageSizeForm, CanvasSizeForm,
// ImageEffectsForm). All are modal to `owner`; Enter = OK, Esc = Cancel, Tab moves between fields.

struct NewImageOptions {  // ShareX RegionCaptureOptions.EditorNewImage* defaults
    int w = 800, h = 600;
    bool transparent = false;
    uint32_t color = 0xFFFFFFFFu;  // ARGB
};
struct ImageSizeOptions {
    int w = 0, h = 0;               // result
    int resampling = RS_Bicubic;    // ShareX ImageEditorResizeInterpolationMode default
    bool keepAspect = true;
};
struct CanvasOptions {
    int left = 0, top = 0, right = 0, bottom = 0;
    uint32_t color = 0;             // ShareX EditorCanvasColor = Transparent
};

// zipshot.ini [imageeditor]: NewImageWidth, NewImageHeight, NewImageTransparent, NewImageColor, CanvasColor,
// ResizeResampling, ResizeKeepAspect (colours as AARRGGBB hex).
void LoadImageEditorOptions(NewImageOptions& n, ImageSizeOptions& s, CanvasOptions& c);
void SaveImageEditorOptions(const NewImageOptions& n, const ImageSizeOptions& s, const CanvasOptions& c);

bool ShowNewImageDialog(HWND owner, NewImageOptions& o);
bool ShowImageSizeDialog(HWND owner, int curW, int curH, ImageSizeOptions& o);
bool ShowCanvasSizeDialog(HWND owner, CanvasOptions& o);
// Edits the effect stack with a live preview of `src`; true = apply (the stack is also saved to zipshot.ini).
bool ShowImageEffectsDialog(HWND owner, ImagePtr src, std::vector<Effect>& stack);
