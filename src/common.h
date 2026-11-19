#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// BGRA, top-down. Alpha is 255 unless `alpha` is set (ellipse/freehand regions).
struct Image {
    int w = 0, h = 0;
    bool alpha = false;
    std::vector<uint32_t> px;
    bool empty() const { return w <= 0 || h <= 0; }
    Image crop(RECT r) const;
};
using ImagePtr = std::shared_ptr<Image>;

// app.cpp
extern HINSTANCE g_inst;
extern HWND g_main;
extern HICON g_icon;
void RunOnMain(std::function<void()> fn);   // safe from any thread
void TrimMemory();
void Log(const wchar_t* fmt, ...);
bool EditorDemoTest(const std::wstring& in, const std::wstring& directory);
std::wstring ExeDir();

// i18n.cpp: UI language (zipshot.ini [main] Language=ru|en). Wrap every user-visible string: Tr(L"Захват", L"Capture").
bool IsEnglish();
void SetEnglish(bool en);
inline const wchar_t* Tr(const wchar_t* ru, const wchar_t* en) { return IsEnglish() ? en : ru; }

// Commands shared by hotkeys, tray and the main window.
enum Cmd { C_Full = 1, C_Region, C_Window, C_Video, C_Gif, C_Folder, C_Settings, C_Rehook, C_EditLast, C_Quit, C_ShowMain,
           C_ImportShareX, C_LangRu, C_LangEn, C_ActiveMonitor, C_ToggleAutostart, C_LastRegion };
void RunCommand(int cmd);
struct HotkeyInfo { std::wstring keys, description; bool registered; };
std::vector<HotkeyInfo> HotkeyStatus();

// Autostart: HKCU\...\Run "Zipshot" (shows in Settings > Apps > Startup and Task Manager > Startup apps).
bool AutostartEnabled();
void SetAutostart(bool on);

// mainwindow.cpp
void ShowMainWindow();
void MainWindowNotifyCapture(const std::wstring& path);  // a file was just saved (still, GIF or video)
void MainWindowRefresh();                                 // language/settings changed: relayout + repaint if open
bool MainWindowWriteReady(const std::wstring& path);       // optional CLI readiness report, no image/file names

// sharex_import.cpp
bool ImportShareXSettings(std::wstring& report);         // ShareX configs -> zipshot.ini; false if none found

// capture.cpp
RECT VirtualScreen();
Image CaptureRect(RECT r, bool cursor);      // r in virtual-screen pixels
HBITMAP CreateDib(int w, int h, void** bits); // 32bpp top-down
std::vector<RECT> VisibleWindowRects();      // z-order, top first
struct WindowRect { HWND hwnd; RECT rect; };
std::vector<WindowRect> VisibleWindows();
bool ControlRectAt(HWND top, POINT screenPt, RECT& out);

// output.cpp
void LoadSettings();
// Persistent switches in zipshot.ini [main]. After-capture keys: AfterCopy, AfterSave, AfterToast, AfterEditor (defaults 1,1,1,0).
bool GetSettingBool(const wchar_t* key, bool def);
void SetSettingBool(const wchar_t* key, bool value);
std::wstring SaveDirRoot();
std::wstring NewOutputPath(const wchar_t* ext); // <SaveDir>\yyyy-MM\<proc>_<rand10>.<ext>
bool SaveImageAuto(const Image& img, std::wstring& path); // png, jpg if png too big
bool EncodeImage(const Image& img, const std::wstring& path, bool jpeg);
bool LoadImageFile(const std::wstring& path, Image& out);
void CopyImageToClipboard(const Image& img);
void CopyFileToClipboard(const std::wstring& path);
enum Sound { S_Capture, S_TaskDone, S_Error, S_Action };  // ShareX's own sounds
void PlayUiSound(Sound s);
std::wstring FindFFmpeg();

// editor.cpp, region mode: ShareX-style capture surface with the annotation toolbar.
// Returns false if cancelled; `out` is the annotated crop, `rect` its virtual-screen rectangle.
bool RegionCapture(ImagePtr screen, RECT vs, Image& out, RECT& rect);

// overlay.cpp (region pick for recordings)
bool SelectRegion(const Image& screen, RECT vs, RECT& out, bool forRecording);
bool DumpOverlayTest(const Image& screen, RECT vs, POINT cursor, RECT sel, const std::wstring& outPng);

// colorpicker.cpp: modern popup colour picker anchored at `at` (screen px). rgba in 0..1; false if cancelled.
bool ShowColorPicker(HWND owner, POINT at, float rgba[4]);
bool ColorPickerTest(const std::wstring& out);

// editor.cpp
void OpenEditor(ImagePtr img, const std::wstring& path);
HICON LoadAppIcon(int px);
bool EditorTest(const std::wstring& in, const std::wstring& out);
bool EditorWindowTest(const std::wstring& in, const std::wstring& out);
bool GifExportTest(const std::wstring& in, const std::wstring& out);  // animated stickers -> animated GIF
bool RegionWindowTest(const std::wstring& out);
bool ImageOpsTest(const std::wstring& in, const std::wstring& outDir);  // imagedialogs.cpp: Image menu ops, effects, dialogs

// toast.cpp
void ShowToast(ImagePtr img, const std::wstring& path, const std::wstring& title);

// recorder.cpp
bool RecorderActive();
bool RecorderBusy();
void RecorderStart(RECT r, bool gif);
void RecorderStop();
