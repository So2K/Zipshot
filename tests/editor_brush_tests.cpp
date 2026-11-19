// Real production Editor mouse handlers on synthetic data, with only host-input
// boundaries replaced. No HWND, clipboard, desktop capture or user settings.
#include "../src/common.h"
#include <cstdio>
#include <cstdlib>
#include <commdlg.h>

static int captureCalls = 0, releaseCalls = 0, unexpectedDialogs = 0;
static HWND WINAPI BrushSetCapture(HWND) { ++captureCalls; return nullptr; }
static BOOL WINAPI BrushReleaseCapture() { ++releaseCalls; return TRUE; }
static SHORT WINAPI BrushKeyState(int) { return 0; }
static BOOL WINAPI BrushClientRect(HWND, LPRECT rect) { *rect = {0, 0, 1000, 800}; return TRUE; }
static BOOL WINAPI BrushTrackMouse(TRACKMOUSEEVENT*) { return TRUE; }
static BOOL WINAPI BrushSetTitle(HWND, LPCWSTR) { return TRUE; }
static BOOL WINAPI BrushAskFile(LPOPENFILENAMEW) { ++unexpectedDialogs; return FALSE; }
static UINT WINAPI BrushProfileInt(LPCWSTR, LPCWSTR, INT fallback, LPCWSTR) { return (UINT)fallback; }
#define SetCapture BrushSetCapture
#define ReleaseCapture BrushReleaseCapture
#define GetKeyState BrushKeyState
#define GetClientRect BrushClientRect
#define TrackMouseEvent BrushTrackMouse
#define SetWindowTextW BrushSetTitle
#define GetOpenFileNameW BrushAskFile
#define GetPrivateProfileIntW BrushProfileInt
#include "../src/editor.cpp"
#undef GetPrivateProfileIntW
#undef GetOpenFileNameW
#undef SetWindowTextW
#undef TrackMouseEvent
#undef GetClientRect
#undef GetKeyState
#undef ReleaseCapture
#undef SetCapture

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HICON g_icon = nullptr;
void Log(const wchar_t*, ...) {}
bool IsEnglish() { return true; }
void TrimMemory() {}
std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory(path);
    return directory.substr(0, directory.find_last_of(L"\\/")) + L"\\synthetic-config";
}
void RunOnMain(std::function<void()>) { std::abort(); }
void RunCommand(int) { std::abort(); }
bool LoadLastRegion(RECT&) { return false; }
void SaveLastRegion(const RECT&) { std::abort(); }
void MainWindowNotifyCapture(const std::wstring&) { std::abort(); }
void ShowToast(ImagePtr, const std::wstring&, const std::wstring&) { std::abort(); }
void RecorderStart(RECT, bool) { std::abort(); }
bool RecorderActive() { return false; }
bool RecorderBusy() { return false; }
void RecorderStop() { std::abort(); }

namespace {
int checks = 0, failed = 0;
void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) ++failed;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
}
bool SamePoints(const Shape& a, const Shape& b) {
    if (a.tool != b.tool || a.pts.size() != b.pts.size()) return false;
    for (size_t i = 0; i < a.pts.size(); ++i)
        if (a.pts[i].x != b.pts[i].x || a.pts[i].y != b.pts[i].y) return false;
    return true;
}
Editor MakeEditor(Tool pen, bool region) {
    Editor e;
    e.img = std::make_shared<Image>();
    e.img->w = 640; e.img->h = 480;
    e.img->px.assign(640 * 480, 0xFF303030u);
    e.region = region; e.mon = {0, 0, 640, 480};
    e.tool = pen; e.scale = 1; e.tbH = 32;
    // Even an imported auto-select preference must not interrupt a brush.
    e.o.switchToSelect = true;
    return e;
}
void Stroke(Editor& e, int x, int y, bool doubleClick = false) {
    e.MouseDown(x, y, doubleClick);
    e.MouseMove(x + 25, y + 25, true);
    e.MouseMove(x + 60, y + 55, true);
    e.MouseUp();
}
void ContinuousStrokes(Tool pen, bool region) {
    Editor e = MakeEditor(pen, region);
    Stroke(e, 100, 140);
    Check(e.shapes.size() == 1 && e.shapes[0].pts.size() >= 3, "first pen stroke is retained");
    Check(e.selected == -1 && !e.moving && e.dragNode == -1, "completed pen stroke has no move or resize selection");
    Check(e.tool == pen, "pen remains active after release, including region auto-select preference");
    const Shape first = e.shapes[0];
    // Simulate an existing selection retained by another command or older state.
    e.selected = 0;
    Check(e.CursorFor({130, 170}) != LoadCursorW(nullptr, IDC_SIZEALL), "pen does not advertise moving over old stroke");
    e.MouseDown(125, 165, false);  // inside the first stroke's bounding box
    Check(e.drawing && !e.moving && e.dragNode == -1 && e.shapes.size() == 2, "pen starts a new overlapping stroke instead of moving the old one");
    e.MouseMove(150, 180, true); e.MouseMove(175, 190, true); e.MouseUp();
    Check(SamePoints(e.shapes[0], first), "overlapping pen gesture leaves original stroke unchanged");
    Check(e.shapes.size() == 2 && e.shapes[1].pts.size() >= 3 && e.selected == -1, "second stroke finishes independently without selection");
    Check(e.undo.size() == 2 && e.redo.empty(), "each completed pen gesture has its own undo entry");
    const Shape second = e.shapes[1];
    e.Restore(e.undo, e.redo);
    Check(e.shapes.size() == 1 && SamePoints(e.shapes[0], first), "undo removes only the second stroke");
    e.Restore(e.redo, e.undo);
    Check(e.shapes.size() == 2 && SamePoints(e.shapes[0], first) && SamePoints(e.shapes[1], second), "redo restores both original stroke geometries");
    e.SetTool(T_Select); e.buttons.clear();
    e.MouseDown(100, 140, false);
    Check(e.moving && e.selected == 0 && !e.drawing, "Select tool still picks and moves an existing pen stroke");
    e.MouseMove(120, 150, true); e.MouseUp();
    Check(e.shapes.size() == 2 && e.shapes[0].pts[0].x == first.pts[0].x + 20 && e.shapes[0].pts[0].y == first.pts[0].y + 10 && SamePoints(e.shapes[1], second), "Select move changes only the chosen old stroke");
    e.Restore(e.undo, e.redo);
    Check(SamePoints(e.shapes[0], first) && SamePoints(e.shapes[1], second), "Select move remains undoable");
    e.selected = 0;
    e.SetTool(pen);
    Check(e.selected == -1, "switching back to pen clears an existing pen selection");
}
void ResizeNodeDoesNotCapturePen(Tool pen, bool region) {
    Editor e = MakeEditor(pen, region);
    Shape rectangle{T_Rect}; rectangle.pts = {{100, 140}, {300, 330}}; rectangle.size = 3;
    e.shapes.push_back(rectangle); e.selected = 0;
    Editor::Node node;
    Check(!e.NodeAt(100, 140, node), "pen ignores resize node on a previously selected shape");
    Check(e.CursorFor({100, 140}) != LoadCursorW(nullptr, IDC_SIZENWSE), "pen cursor does not offer resizing a selected rectangle");
    Stroke(e, 100, 140);
    Check(e.shapes.size() == 2 && SamePoints(e.shapes[0], rectangle), "drawing over old resize handle creates a stroke and preserves rectangle");
    const Shape stroke = e.shapes[1];
    e.SetTool(T_Select); e.buttons.clear(); e.selected = 0;
    Check(e.NodeAt(100, 140, node), "Select tool still exposes rectangle resize handles");
    e.MouseDown(100, 140, false);
    Check(e.dragNode >= 0 && !e.drawing, "Select starts the existing rectangle's node drag");
    e.MouseMove(80, 120, true); e.MouseUp();
    Check(e.shapes.size() == 2 && e.shapes[0].pts[0].x == 80 && e.shapes[0].pts[0].y == 120 && SamePoints(e.shapes[1], stroke), "Select resize preserves the separate pen stroke");
}
void DoubleClickStillDraws(Tool pen, bool region) {
    Editor e = MakeEditor(pen, region);
    Shape text{T_TextBackground}; text.pts = {{100, 140}, {300, 280}}; text.text = L"unchanged"; text.autoSize = false;
    e.shapes.push_back(text); e.selected = 0;
    Stroke(e, 140, 180, true);
    Check(e.shapes.size() == 2 && e.editingText == -1 && e.shapes[0].text == text.text && SamePoints(e.shapes[0], text), "rapid pen double-click over old text draws instead of entering text editing");
}
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (Tool pen : {T_Freehand, T_FreehandArrow})
        for (bool region : {false, true}) {
            std::printf("CASE %s / %s\n", pen == T_Freehand ? "brush" : "freehand arrow", region ? "region" : "editor");
            ContinuousStrokes(pen, region);
            ResizeNodeDoesNotCapturePen(pen, region);
            DoubleClickStillDraws(pen, region);
        }
    Check(captureCalls > 0 && releaseCalls > 0, "production input handlers used the isolated capture boundary");
    Check(unexpectedDialogs == 0, "pen tests never invoke an image-file dialog");
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("%d assertions, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
