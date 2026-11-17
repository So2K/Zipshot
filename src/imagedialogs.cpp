#include "imagedialogs.h"
#include <dwmapi.h>
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

// A tiny dark form toolkit (labels, numeric/text edits, owner-drawn checks, choices, colour swatches,
// buttons, an owner-drawn list) and the four ShareX image dialogs built on it.

namespace {

const COLORREF kBg = RGB(39, 39, 39), kField = RGB(46, 46, 46), kText = RGB(231, 233, 234), kAccent = RGB(0, 138, 255),
               kFrame = RGB(67, 67, 67), kDim = RGB(150, 152, 155), kButton = RGB(56, 56, 58), kButtonDown = RGB(30, 30, 32);
constexpr UINT WM_PREVIEW = WM_APP + 7;

enum CtlKind { K_Edit, K_TextEdit, K_Check, K_Choice, K_Color, K_Button, K_List };

struct Ctl {
    CtlKind kind;
    int id = 0;
    HWND hwnd = nullptr;
    RECT frame{};  // client pixels
    double num = 0, lo = -1e9, hi = 1e9, step = 1;
    bool isFloat = false;
    bool checked = false;
    int choice = 0;
    std::vector<std::wstring> choices;
    uint32_t color = 0;
    std::wstring text;
    bool primary = false;
    int param = -1;  // effects dialog: parameter index of the selected effect
    double Value() const { return std::clamp(num, lo, hi); }
};

struct Label {
    std::wstring text;
    RECT r;
    bool bold = false, dim = false;
    UINT fmt = DT_LEFT;
    int tag = 0;  // effects dialog: 1 = belongs to the parameter panel
};

struct Form {
    HWND hwnd = nullptr, owner = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    HBRUSH fieldBrush = nullptr, bgBrush = nullptr;
    std::vector<Ctl> ctls;
    std::vector<Label> labels;
    int W = 400, H = 200;  // client size, 96-dpi units
    bool done = false, ok = false, quiet = false, updating = false;
    std::function<void(Ctl&)> onChange;                    // a value was edited by the user
    std::function<void(int id, int code)> onCommand;       // buttons and list notifications
    std::function<void(HDC)> onPaint;                      // extra painting
    std::function<void(const DRAWITEMSTRUCT&)> onDrawList;  // owner-drawn list items
    std::function<int(int vk)> onListKey;                  // WM_VKEYTOITEM; -1 = default
    std::function<bool(int x, int item)> onListClick;      // true = handled
    std::function<void()> onPreview;                       // WM_PREVIEW

    int S(double v) const { return (int)std::lround(v * dpi / 96.0); }
    RECT R(double x, double y, double w, double h) const { return {S(x), S(y), S(x + w), S(y + h)}; }
    Ctl* Find(int id) {
        for (auto& c : ctls)
            if (c.id == id) return &c;
        return nullptr;
    }
    Ctl* FindHwnd(HWND h) {
        for (auto& c : ctls)
            if (c.hwnd == h) return &c;
        return nullptr;
    }
};

std::wstring Fmt(double v, bool isFloat) {
    wchar_t b[40];
    if (isFloat) swprintf_s(b, L"%g", std::round(v * 1000) / 1000);
    else swprintf_s(b, L"%d", (int)std::lround(v));
    return b;
}

uint32_t ToArgb(const float c[4]) {
    auto q = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.f, 1.f) * 255); };
    return q(c[3]) << 24 | q(c[0]) << 16 | q(c[1]) << 8 | q(c[2]);
}

void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}
void FrameRectColor(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FrameRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

// Colour swatch with a checkerboard under transparent colours.
void Swatch(HDC dc, RECT r, uint32_t argb) {
    int a = argb >> 24, cell = std::max(3, (int)(r.bottom - r.top) / 3);
    for (int y = r.top; y < r.bottom; y += cell)
        for (int x = r.left; x < r.right; x += cell) {
            bool dark = ((x - r.left) / cell + (y - r.top) / cell) % 2;
            int base = dark ? 200 : 255;
            int cr = (base * (255 - a) + (int)(argb >> 16 & 255) * a) / 255, cg = (base * (255 - a) + (int)(argb >> 8 & 255) * a) / 255,
                cb = (base * (255 - a) + (int)(argb & 255) * a) / 255;
            RECT q{x, y, std::min(x + cell, (int)r.right), std::min(y + cell, (int)r.bottom)};
            FillRectColor(dc, q, RGB(cr, cg, cb));
        }
    FrameRectColor(dc, r, RGB(15, 15, 15));
}

LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l);

void Subclass(HWND h) {
    SetPropW(h, L"ZsProc", (HANDLE)GetWindowLongPtrW(h, GWLP_WNDPROC));
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SubProc);
}

void SetEditText(Form& f, Ctl& c) {
    f.updating = true;
    SetWindowTextW(c.hwnd, c.kind == K_TextEdit ? c.text.c_str() : Fmt(c.Value(), c.isFloat).c_str());
    f.updating = false;
}

void Step(Form& f, Ctl& c, int dir) {
    c.num = std::clamp(c.Value() + dir * c.step, c.lo, c.hi);
    if (!c.isFloat) c.num = std::round(c.num);
    SetEditText(f, c);
    SendMessageW(c.hwnd, EM_SETSEL, 0, -1);
    if (f.onChange) f.onChange(c);
}

void Redraw(Ctl& c) { InvalidateRect(c.hwnd, nullptr, FALSE); }

void ChooseFrom(Form& f, Ctl& c) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < c.choices.size(); ++i)
        AppendMenuW(m, MF_STRING | ((int)i == c.choice ? MF_CHECKED : 0), i + 1, c.choices[i].c_str());
    POINT p{c.frame.left, c.frame.bottom};
    ClientToScreen(f.hwnd, &p);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, f.hwnd, nullptr);
    DestroyMenu(m);
    if (id > 0 && id - 1 != c.choice) {
        c.choice = id - 1;
        Redraw(c);
        if (f.onChange) f.onChange(c);
    }
}

void PickColor(Form& f, Ctl& c) {
    float rgba[4] = {(c.color >> 16 & 255) / 255.f, (c.color >> 8 & 255) / 255.f, (c.color & 255) / 255.f, (c.color >> 24) / 255.f};
    POINT p{c.frame.left, c.frame.bottom};
    ClientToScreen(f.hwnd, &p);
    if (!ShowColorPicker(f.hwnd, p, rgba)) return;
    c.color = ToArgb(rgba);
    Redraw(c);
    if (f.onChange) f.onChange(c);
}

LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto orig = (WNDPROC)GetPropW(h, L"ZsProc");
    auto* f = (Form*)GetWindowLongPtrW(GetParent(h), GWLP_USERDATA);
    Ctl* c = f ? f->FindHwnd(h) : nullptr;
    if (c) {
        if (c->kind == K_Edit) {
            if (m == WM_KEYDOWN && (w == VK_UP || w == VK_DOWN)) {
                Step(*f, *c, w == VK_UP ? 1 : -1);
                return 0;
            }
            if (m == WM_MOUSEWHEEL) {
                Step(*f, *c, GET_WHEEL_DELTA_WPARAM(w) > 0 ? 1 : -1);
                return 0;
            }
        }
        if (c->kind == K_Choice && m == WM_KEYDOWN && (w == VK_UP || w == VK_DOWN) && !c->choices.empty()) {
            int n = (int)c->choices.size();
            c->choice = (c->choice + (w == VK_DOWN ? 1 : n - 1)) % n;
            Redraw(*c);
            if (f->onChange) f->onChange(*c);
            return 0;
        }
        if (c->kind == K_List && m == WM_LBUTTONDOWN && f->onListClick) {
            LRESULT r = SendMessageW(h, LB_ITEMFROMPOINT, 0, l);
            if (!HIWORD(r) && f->onListClick((short)LOWORD(l), (int)LOWORD(r))) {
                SetFocus(h);
                return 0;
            }
        }
    }
    if (m == WM_NCDESTROY) {
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)orig);
        RemovePropW(h, L"ZsProc");
    }
    return CallWindowProcW(orig, h, m, w, l);
}

// ---------- control creation (coordinates in 96-dpi units) ----------

void AddLabel(Form& f, const std::wstring& t, double x, double y, double w, double h = 24, UINT fmt = DT_LEFT, bool bold = false,
              bool dim = false, int tag = 0) {
    f.labels.push_back({t, f.R(x, y, w, h), bold, dim, fmt, tag});
}

HWND Child(Form& f, const wchar_t* cls, const wchar_t* text, DWORD style, RECT r, int id) {
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, r.left, r.top, r.right - r.left, r.bottom - r.top, f.hwnd,
                             (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)f.font, FALSE);
    return h;
}

Ctl& AddCtl(Form& f, Ctl c) {
    f.ctls.push_back(std::move(c));
    return f.ctls.back();
}

void PlaceEdit(Form& f, Ctl& c, DWORD style) {
    TEXTMETRICW tm{};
    HDC dc = GetDC(f.hwnd);
    HGDIOBJ old = SelectObject(dc, f.font);
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(f.hwnd, dc);
    int lh = tm.tmHeight + 2, pad = f.S(6);
    int top = c.frame.top + (c.frame.bottom - c.frame.top - lh) / 2;
    RECT r{c.frame.left + pad, top, c.frame.right - pad, top + lh};
    c.hwnd = Child(f, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | style, r, c.id);
    SendMessageW(c.hwnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    Subclass(c.hwnd);
    SetEditText(f, c);
}

Ctl& AddNumber(Form& f, int id, double x, double y, double w, double val, double lo, double hi, double step, bool isFloat, int param = -1) {
    Ctl c{K_Edit, id};
    c.frame = f.R(x, y, w, 24);
    c.num = val, c.lo = lo, c.hi = hi, c.step = step, c.isFloat = isFloat, c.param = param;
    Ctl& r = AddCtl(f, c);
    PlaceEdit(f, r, 0);
    return r;
}

Ctl& AddText(Form& f, int id, double x, double y, double w, const std::wstring& text, int param = -1) {
    Ctl c{K_TextEdit, id};
    c.frame = f.R(x, y, w, 24);
    c.text = text, c.param = param;
    Ctl& r = AddCtl(f, c);
    PlaceEdit(f, r, 0);
    return r;
}

Ctl& AddOwnerDraw(Form& f, Ctl c, const std::wstring& text) {
    c.text = text;
    Ctl& r = AddCtl(f, c);
    r.hwnd = Child(f, L"BUTTON", text.c_str(), WS_TABSTOP | BS_OWNERDRAW, r.frame, r.id);
    Subclass(r.hwnd);
    return r;
}

Ctl& AddCheck(Form& f, int id, double x, double y, double w, const std::wstring& text, bool on, int param = -1) {
    Ctl c{K_Check, id};
    c.frame = f.R(x, y, w, 24);
    c.checked = on, c.param = param;
    return AddOwnerDraw(f, c, text);
}

Ctl& AddChoice(Form& f, int id, double x, double y, double w, std::vector<std::wstring> items, int sel, int param = -1) {
    Ctl c{K_Choice, id};
    c.frame = f.R(x, y, w, 24);
    c.choices = std::move(items);
    c.choice = std::clamp(sel, 0, std::max(0, (int)c.choices.size() - 1));
    c.param = param;
    return AddOwnerDraw(f, c, L"");
}

Ctl& AddColor(Form& f, int id, double x, double y, double w, const std::wstring& text, uint32_t argb, int param = -1) {
    Ctl c{K_Color, id};
    c.frame = f.R(x, y, w, 24);
    c.color = argb, c.param = param;
    return AddOwnerDraw(f, c, text);
}

Ctl& AddButton(Form& f, int id, double x, double y, double w, const std::wstring& text, bool primary = false) {
    Ctl c{K_Button, id};
    c.frame = f.R(x, y, w, 28);
    c.primary = primary;
    return AddOwnerDraw(f, c, text);
}

Ctl& AddList(Form& f, int id, double x, double y, double w, double h) {
    Ctl c{K_List, id};
    c.frame = f.R(x, y, w, h);
    Ctl& r = AddCtl(f, c);
    RECT in{r.frame.left + 1, r.frame.top + 1, r.frame.right - 1, r.frame.bottom - 1};
    r.hwnd = Child(f, L"LISTBOX", L"",
                   WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_WANTKEYBOARDINPUT, in, id);
    using SetThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    if (auto fn = (SetThemeFn)GetProcAddress(LoadLibraryW(L"uxtheme.dll"), "SetWindowTheme")) fn(r.hwnd, L"DarkMode_Explorer", nullptr);
    Subclass(r.hwnd);
    return r;
}

std::vector<std::wstring> Split(const wchar_t* s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (; s && *s; ++s) {
        if (*s == L'|') out.push_back(cur), cur.clear();
        else cur += *s;
    }
    out.push_back(cur);
    return out;
}

void RemoveCtl(Form& f, int id) {
    for (size_t i = 0; i < f.ctls.size(); ++i)
        if (f.ctls[i].id == id) {
            DestroyWindow(f.ctls[i].hwnd);
            f.ctls.erase(f.ctls.begin() + i);
            return;
        }
}

// ---------- painting ----------

void DrawText1(HDC dc, const std::wstring& t, RECT r, UINT fmt, COLORREF c) {
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, t.c_str(), (int)t.size(), &r, fmt | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void PaintForm(Form& f, HDC dc) {
    RECT cr;
    GetClientRect(f.hwnd, &cr);
    FillRectColor(dc, cr, kBg);
    for (const Label& l : f.labels) {
        HGDIOBJ old = SelectObject(dc, l.bold ? f.bold : f.font);
        DrawText1(dc, l.text, l.r, l.fmt, l.dim ? kDim : kText);
        SelectObject(dc, old);
    }
    HWND focus = GetFocus();
    for (const Ctl& c : f.ctls) {
        if (c.kind == K_Edit || c.kind == K_TextEdit) {
            FillRectColor(dc, c.frame, kField);
            FrameRectColor(dc, c.frame, focus == c.hwnd ? kAccent : kFrame);
        } else if (c.kind == K_List) {
            FrameRectColor(dc, c.frame, kFrame);
        }
    }
    if (f.onPaint) f.onPaint(dc);
}

void DrawOwner(Form& f, const DRAWITEMSTRUCT& d) {
    Ctl* c = f.FindHwnd(d.hwndItem);
    if (!c) return;
    if (c->kind == K_List) {
        if (f.onDrawList) f.onDrawList(d);
        return;
    }
    HDC dc = d.hDC;
    RECT r = d.rcItem;
    bool focus = d.itemState & ODS_FOCUS, down = d.itemState & ODS_SELECTED, disabled = d.itemState & ODS_DISABLED;
    HGDIOBJ old = SelectObject(dc, f.font);
    FillRectColor(dc, r, kBg);
    COLORREF text = disabled ? RGB(110, 110, 112) : kText;
    switch (c->kind) {
    case K_Check: {
        int s = f.S(14), y = (r.top + r.bottom - s) / 2;
        RECT box{r.left + f.S(1), y, r.left + f.S(1) + s, y + s};
        FillRectColor(dc, box, c->checked ? (disabled ? RGB(70, 70, 72) : kAccent) : kField);
        FrameRectColor(dc, box, focus ? kAccent : c->checked ? kAccent : RGB(100, 100, 104));
        if (c->checked) {  // tick
            HPEN pen = CreatePen(PS_SOLID, std::max(2, f.S(2)), RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(dc, pen);
            POINT pts[3] = {{box.left + s * 3 / 14, box.top + s * 7 / 14}, {box.left + s * 6 / 14, box.top + s * 10 / 14},
                            {box.left + s * 11 / 14, box.top + s * 4 / 14}};
            Polyline(dc, pts, 3);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        RECT tr{box.right + f.S(8), r.top, r.right, r.bottom};
        DrawText1(dc, c->text, tr, DT_LEFT, text);
        if (focus) {
            SIZE z;
            GetTextExtentPoint32W(dc, c->text.c_str(), (int)c->text.size(), &z);
            RECT u{tr.left - f.S(2), r.bottom - f.S(2), std::min((int)r.right, (int)(tr.left + z.cx) + f.S(2)), r.bottom - f.S(1)};
            FillRectColor(dc, u, kAccent);
        }
        break;
    }
    case K_Choice: {
        FillRectColor(dc, r, kField);
        FrameRectColor(dc, r, focus || down ? kAccent : kFrame);
        RECT tr{r.left + f.S(7), r.top, r.right - f.S(22), r.bottom};
        DrawText1(dc, c->choices.empty() ? L"" : c->choices[c->choice], tr, DT_LEFT, text);
        int cx = r.right - f.S(12), cy = (r.top + r.bottom) / 2, k = f.S(4);
        POINT tri[3] = {{cx - k, cy - k / 2}, {cx + k + 1, cy - k / 2}, {cx, cy + k / 2 + 1}};
        SetDCBrushColor(dc, kDim);
        SetDCPenColor(dc, kDim);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH)), op = SelectObject(dc, GetStockObject(DC_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        break;
    }
    case K_Color: {
        FillRectColor(dc, r, down ? kButtonDown : kButton);
        FrameRectColor(dc, r, focus ? kAccent : kFrame);
        int s = r.bottom - r.top - f.S(8);
        RECT sw{r.left + f.S(4), r.top + f.S(4), r.left + f.S(4) + s, r.top + f.S(4) + s};
        Swatch(dc, sw, c->color);
        RECT tr{sw.right + f.S(8), r.top, r.right - f.S(4), r.bottom};
        DrawText1(dc, c->text, tr, DT_LEFT, text);
        break;
    }
    case K_Button: {
        COLORREF bg = c->primary ? (down ? RGB(0, 105, 200) : kAccent) : (down ? kButtonDown : kButton);
        if (disabled) bg = RGB(50, 50, 52);
        FillRectColor(dc, r, bg);
        FrameRectColor(dc, r, focus ? (c->primary ? RGB(255, 255, 255) : kAccent) : c->primary ? kAccent : kFrame);
        DrawText1(dc, c->text, r, DT_CENTER, c->primary && !disabled ? RGB(255, 255, 255) : text);
        break;
    }
    default: break;
    }
    SelectObject(dc, old);
}

void Finish(Form& f, bool ok) {
    f.ok = ok;
    f.done = true;
}

void Command(Form& f, int id, int code, HWND from) {
    if (id == IDOK || id == IDCANCEL) {
        if (code == BN_CLICKED || !from) Finish(f, id == IDOK);
        return;
    }
    Ctl* c = f.Find(id);
    if (!c) return;
    switch (c->kind) {
    case K_Edit:
    case K_TextEdit:
        if (code == EN_CHANGE && !f.updating) {
            wchar_t buf[1024];
            GetWindowTextW(c->hwnd, buf, 1024);
            if (c->kind == K_TextEdit) {
                c->text = buf;
            } else {
                wchar_t* end = nullptr;
                for (wchar_t* p = buf; *p; ++p)
                    if (*p == L',') *p = L'.';
                double v = wcstod(buf, &end);
                if (end == buf) return;
                c->num = v;
            }
            if (f.onChange) f.onChange(*c);
        } else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
            RECT r = c->frame;
            InvalidateRect(f.hwnd, &r, FALSE);
            if (code == EN_KILLFOCUS && c->kind == K_Edit) {
                c->num = c->Value();
                SetEditText(f, *c);
            }
            if (code == EN_SETFOCUS) SendMessageW(c->hwnd, EM_SETSEL, 0, -1);
        }
        break;
    case K_Check:
        if (code == BN_CLICKED) {
            c->checked = !c->checked;
            Redraw(*c);
            if (f.onChange) f.onChange(*c);
        }
        break;
    case K_Choice:
        if (code == BN_CLICKED) ChooseFrom(f, *c);
        break;
    case K_Color:
        if (code == BN_CLICKED) PickColor(f, *c);
        break;
    default:
        if (f.onCommand) f.onCommand(id, code);
        break;
    }
}

LRESULT CALLBACK FormProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* f = (Form*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        f = (Form*)((CREATESTRUCTW*)l)->lpCreateParams;
        f->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)f);
    }
    if (!f) return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cr;
        GetClientRect(h, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(mem, bmp);
        PaintForm(*f, mem);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)w, kText);
        SetBkColor((HDC)w, kField);
        return (LRESULT)f->fieldBrush;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetTextColor((HDC)w, kText);
        SetBkColor((HDC)w, kBg);
        return (LRESULT)f->bgBrush;
    case WM_MEASUREITEM: {
        auto* mi = (MEASUREITEMSTRUCT*)l;
        mi->itemHeight = f->S(26);
        return TRUE;
    }
    case WM_DRAWITEM: DrawOwner(*f, *(const DRAWITEMSTRUCT*)l); return TRUE;
    case WM_COMMAND: Command(*f, LOWORD(w), HIWORD(w), (HWND)l); return 0;
    case WM_VKEYTOITEM:
        if (f->onListKey) return f->onListKey(LOWORD(w));
        return -1;
    case WM_PREVIEW:
        if (f->onPreview) f->onPreview();
        return 0;
    case WM_CLOSE: Finish(*f, false); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Creates the window (hidden). Controls are added after this, so they have a parent.
bool CreateForm(Form& f, HWND owner, const wchar_t* title) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = FormProc;
        wc.hInstance = g_inst;
        wc.hIcon = g_icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ZipshotImageDialog";
        reg = RegisterClassW(&wc) != 0;
    }
    f.owner = owner;
    f.dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    if (f.quiet) f.dpi = 144;  // tests render at the main monitor's 150 %
    f.font = CreateFontW(-MulDiv(9, f.dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    f.bold = CreateFontW(-MulDiv(9, f.dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    f.fieldBrush = CreateSolidBrush(kField);
    f.bgBrush = CreateSolidBrush(kBg);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, ex = WS_EX_DLGMODALFRAME | (f.quiet ? WS_EX_NOACTIVATE : 0);
    RECT r{0, 0, f.S(f.W), f.S(f.H)};
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, f.dpi);
    int ww = r.right - r.left, wh = r.bottom - r.top, x = -30000, y = -30000;
    if (!f.quiet) {
        RECT orc{};
        if (owner) GetWindowRect(owner, &orc);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromRect(&orc, MONITOR_DEFAULTTONEAREST), &mi);
        x = std::clamp((int)(orc.left + orc.right - ww) / 2, (int)mi.rcWork.left, std::max((int)mi.rcWork.left, (int)mi.rcWork.right - ww));
        y = std::clamp((int)(orc.top + orc.bottom - wh) / 2, (int)mi.rcWork.top, std::max((int)mi.rcWork.top, (int)mi.rcWork.bottom - wh));
    }
    std::wstring t = std::wstring(L"Zipshot - ") + title;
    HWND h = CreateWindowExW(ex, L"ZipshotImageDialog", t.c_str(), style, x, y, ww, wh, owner, nullptr, g_inst, &f);
    if (!h) return false;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    return true;
}

void DestroyForm(Form& f) {
    if (f.hwnd && IsWindow(f.hwnd)) {
        SetWindowLongPtrW(f.hwnd, GWLP_USERDATA, 0);
        DestroyWindow(f.hwnd);
    }
    f.hwnd = nullptr;
    DeleteObject(f.font);
    DeleteObject(f.bold);
    DeleteObject(f.fieldBrush);
    DeleteObject(f.bgBrush);
}

// Off-screen render for the self-test (never activated, never on a visible monitor).
std::wstring g_shotPath;

void Shot(Form& f) {
    ShowWindow(f.hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(f.hwnd);
    RECT r;
    GetWindowRect(f.hwnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    void* bits;
    HBITMAP bmp = CreateDib(w, h, &bits);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bmp);
    PrintWindow(f.hwnd, dc, PW_RENDERFULLCONTENT);
    GdiFlush();
    Image shot;
    shot.w = w;
    shot.h = h;
    shot.px.assign((uint32_t*)bits, (uint32_t*)bits + (size_t)w * h);
    for (auto& p : shot.px) p |= 0xFF000000u;
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bmp);
    EncodeImage(shot, g_shotPath, false);
}

bool RunForm(Form& f) {
    if (f.quiet) {
        Shot(f);
        return false;
    }
    if (f.owner) EnableWindow(f.owner, FALSE);
    ShowWindow(f.hwnd, SW_SHOW);
    SetForegroundWindow(f.hwnd);
    for (auto& c : f.ctls)
        if (GetWindowLongW(c.hwnd, GWL_STYLE) & WS_TABSTOP) {
            SetFocus(c.hwnd);
            break;
        }
    MSG msg;
    while (!f.done && IsWindow(f.hwnd)) {
        int r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage((int)msg.wParam);
            break;
        }
        if (msg.message == WM_KEYDOWN && (msg.hwnd == f.hwnd || IsChild(f.hwnd, msg.hwnd))) {
            if (msg.wParam == VK_ESCAPE) {
                Finish(f, false);
                continue;
            }
            if (msg.wParam == VK_RETURN) {
                // Enter presses a focused push button (Add, Remove...), anywhere else it is OK.
                Ctl* c = f.FindHwnd(GetFocus());
                if (c && c->kind == K_Button && c->id != IDOK) SendMessageW(f.hwnd, WM_COMMAND, MAKEWPARAM(c->id, BN_CLICKED), (LPARAM)c->hwnd);
                else Finish(f, true);
                continue;
            }
        }
        if (IsDialogMessageW(f.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (f.owner) {
        EnableWindow(f.owner, TRUE);
        SetActiveWindow(f.owner);
    }
    return f.ok;
}

void OkCancel(Form& f) {
    AddButton(f, IDOK, f.W - 212, f.H - 42, 96, Tr(L"ОК", L"OK"), true);
    AddButton(f, IDCANCEL, f.W - 108, f.H - 42, 96, Tr(L"Отменить", L"Cancel"));
}

std::wstring Hex(uint32_t v) {
    wchar_t b[16];
    swprintf_s(b, L"%08X", v);
    return b;
}
std::wstring Ini() { return ExeDir() + L"\\zipshot.ini"; }

// ---------- effects dialog state ----------

struct Preview {
    std::mutex mu;
    std::atomic<int> gen{0};
    bool running = false, pending = false, ready = false;
    Image thumb;   // fitted to the preview box
    int w = 0, h = 0;  // full result size
};

enum { ID_List = 100, ID_Add, ID_Remove, ID_Up, ID_Down, ID_Reset, ID_Param = 200 };

struct EffectsDlg {
    Form f;
    ImagePtr src;
    std::vector<Effect> stack;
    int sel = -1;
    std::shared_ptr<Preview> prev = std::make_shared<Preview>();
    RECT box{};  // preview area, client px

    void RefreshList() {
        HWND lb = f.Find(ID_List)->hwnd;
        SendMessageW(lb, WM_SETREDRAW, FALSE, 0);
        SendMessageW(lb, LB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < stack.size(); ++i) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)L"");
        if (sel >= (int)stack.size()) sel = (int)stack.size() - 1;
        SendMessageW(lb, LB_SETCURSEL, sel, 0);
        SendMessageW(lb, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(lb, nullptr, TRUE);
        EnableWindow(f.Find(ID_Remove)->hwnd, sel >= 0);
        EnableWindow(f.Find(ID_Up)->hwnd, sel > 0);
        EnableWindow(f.Find(ID_Down)->hwnd, sel >= 0 && sel + 1 < (int)stack.size());
    }

    void BuildParams() {
        for (size_t i = f.ctls.size(); i-- > 0;)
            if (f.ctls[i].param >= 0) RemoveCtl(f, f.ctls[i].id);
        f.labels.erase(std::remove_if(f.labels.begin(), f.labels.end(), [](const Label& l) { return l.tag == 1; }), f.labels.end());
        double x = 262, y = 12, lw = 128, cw = 160;
        if (sel < 0 || sel >= (int)stack.size()) {
            AddLabel(f, Tr(L"Добавьте эффект кнопкой «Добавить».", L"Add an effect with the Add button."), x, y, lw + cw + 4, 24, DT_LEFT, false, true, 1);
        } else {
            const Effect& e = stack[sel];
            const EffectDef& d = GetEffectDef(e.type);
            AddLabel(f, EffectName(e), x, y, lw + cw, 24, DT_LEFT, true, false, 1);
            y += 32;
            if (d.count == 0) AddLabel(f, Tr(L"Без параметров.", L"No settings."), x, y, lw + cw, 24, DT_LEFT, false, true, 1);
            for (int i = 0; i < d.count; ++i, y += 32) {
                const EffectParamDef& p = d.params[i];
                int id = ID_Param + i;
                const wchar_t* name = Tr(p.ru, p.en);
                switch (p.kind) {
                case PK_Bool: AddCheck(f, id, x, y, lw + cw + 4, name, e.v[i] != 0, i); break;
                case PK_Color: AddLabel(f, name, x, y, lw, 24, DT_LEFT, false, false, 1);
                    AddColor(f, id, x + lw + 4, y, cw, Hex((uint32_t)e.v[i]), (uint32_t)e.v[i], i);
                    break;
                case PK_Choice:
                    AddLabel(f, name, x, y, lw, 24, DT_LEFT, false, false, 1);
                    AddChoice(f, id, x + lw + 4, y, cw, Split(Tr(p.choicesRu, p.choicesEn)), (int)e.v[i], i);
                    break;
                case PK_Text:
                    AddLabel(f, name, x, y, lw, 24, DT_LEFT, false, false, 1);
                    AddText(f, id, x + lw + 4, y, cw, e.text, i);
                    break;
                default:
                    AddLabel(f, name, x, y, lw, 24, DT_LEFT, false, false, 1);
                    AddNumber(f, id, x + lw + 4, y, cw, e.v[i], p.lo, p.hi, p.step, p.kind == PK_Float, i);
                    break;
                }
            }
        }
        // Keep the tab order: parameters after the list buttons, OK/Cancel last.
        HWND after = f.Find(ID_Reset)->hwnd;
        for (auto& c : f.ctls)
            if (c.param >= 0) {
                SetWindowPos(c.hwnd, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                after = c.hwnd;
            }
        InvalidateRect(f.hwnd, nullptr, FALSE);
    }

    void StartPreview() {
        auto p = prev;
        p->gen++;  // cancels a running pass
        {
            std::lock_guard<std::mutex> lk(p->mu);
            if (p->running) {
                p->pending = true;
                return;
            }
            p->running = true;
        }
        int gen = p->gen.load(), bw = box.right - box.left, bh = box.bottom - box.top;
        HWND h = f.hwnd;
        std::thread([p, gen, bw, bh, h, s = src, st = stack] {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Image r = ApplyEffects(*s, st, &p->gen, gen);
            Image t = r.empty() ? Image{} : ImgThumbnail(r, bw, bh);
            {
                std::lock_guard<std::mutex> lk(p->mu);
                if (!r.empty() && p->gen.load() == gen) {
                    p->thumb = std::move(t);
                    p->w = r.w;
                    p->h = r.h;
                    p->ready = true;
                }
                p->running = false;
            }
            if (SUCCEEDED(hr)) CoUninitialize();
            PostMessageW(h, WM_PREVIEW, 0, 0);
        }).detach();
    }

    void OnPreviewDone() {
        bool again;
        {
            std::lock_guard<std::mutex> lk(prev->mu);
            again = prev->pending;
            prev->pending = false;
        }
        if (again) StartPreview();
        RECT r{box.left, f.S(4), box.right, box.bottom};
        InvalidateRect(f.hwnd, &r, FALSE);
    }

    void Changed() {
        RefreshList();
        StartPreview();
    }

    void PaintPreview(HDC dc) {
        FrameRectColor(dc, {box.left - 1, box.top - 1, box.right + 1, box.bottom + 1}, kFrame);
        std::lock_guard<std::mutex> lk(prev->mu);
        HGDIOBJ old = SelectObject(dc, f.font);
        RECT info{box.left, f.S(12), box.right, f.S(36)};
        std::wstring size = prev->ready ? std::to_wstring(prev->w) + L" × " + std::to_wstring(prev->h) : L"…";
        if (prev->running) size = Tr(L"обработка…  ", L"processing…  ") + size;
        DrawText1(dc, size, info, DT_RIGHT, kDim);
        SelectObject(dc, old);
        if (!prev->ready || prev->thumb.empty()) return;
        const Image& t = prev->thumb;
        // Composite over a checkerboard so transparency (shadow, rounded corners) is visible.
        std::vector<uint32_t> px(t.px.size());
        int cell = f.S(8);
        for (int y = 0; y < t.h; ++y)
            for (int x = 0; x < t.w; ++x) {
                uint32_t p = t.px[(size_t)y * t.w + x], a = t.alpha ? p >> 24 : 255;
                uint32_t bg = ((x / cell + y / cell) % 2) ? 58 : 74;
                auto mix = [&](int s) { return ((p >> s & 255) * a + bg * (255 - a)) / 255; };
                px[(size_t)y * t.w + x] = mix(16) << 16 | mix(8) << 8 | mix(0);
            }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), t.w, -t.h, 1, 32, BI_RGB};
        int x0 = box.left + (box.right - box.left - t.w) / 2, y0 = box.top + (box.bottom - box.top - t.h) / 2;
        SetDIBitsToDevice(dc, x0, y0, t.w, t.h, 0, 0, 0, t.h, px.data(), &bi, DIB_RGB_COLORS);
    }

    void DrawItem(const DRAWITEMSTRUCT& d) {
        if ((int)d.itemID < 0 || (int)d.itemID >= (int)stack.size()) {
            FillRectColor(d.hDC, d.rcItem, kField);
            return;
        }
        const Effect& e = stack[d.itemID];
        bool selected = d.itemState & ODS_SELECTED;
        HDC dc = d.hDC;
        RECT r = d.rcItem;
        FillRectColor(dc, r, selected ? RGB(46, 66, 92) : kField);
        if (selected && (d.itemState & ODS_FOCUS)) FrameRectColor(dc, r, kAccent);
        int s = f.S(14), y = (r.top + r.bottom - s) / 2;
        RECT cb{r.left + f.S(6), y, r.left + f.S(6) + s, y + s};
        FillRectColor(dc, cb, e.enabled ? kAccent : RGB(40, 40, 42));
        FrameRectColor(dc, cb, e.enabled ? kAccent : RGB(110, 110, 114));
        if (e.enabled) {
            HPEN pen = CreatePen(PS_SOLID, std::max(2, f.S(2)), RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(dc, pen);
            POINT pts[3] = {{cb.left + s * 3 / 14, cb.top + s * 7 / 14}, {cb.left + s * 6 / 14, cb.top + s * 10 / 14},
                            {cb.left + s * 11 / 14, cb.top + s * 4 / 14}};
            Polyline(dc, pts, 3);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        HGDIOBJ old = SelectObject(dc, f.font);
        RECT nr{cb.right + f.S(8), r.top, r.right - f.S(6), r.bottom};
        std::wstring sum = EffectSummary(e);
        SIZE z{};
        GetTextExtentPoint32W(dc, sum.c_str(), (int)sum.size(), &z);
        int sumW = std::min((int)z.cx, (int)(nr.right - nr.left) / 2);
        RECT sr{nr.right - sumW, r.top, nr.right, r.bottom};
        nr.right -= sumW + f.S(6);
        DrawText1(dc, EffectName(e), nr, DT_LEFT, e.enabled ? kText : kDim);
        DrawText1(dc, sum, sr, DT_RIGHT, kDim);
        SelectObject(dc, old);
    }

    void AddMenu() {
        // ShareX groups: Adjustments, Filters, Drawings, Manipulations.
        struct Group { const wchar_t* ru; const wchar_t* en; std::vector<int> items; };
        Group groups[] = {
            {L"Коррекция", L"Adjustments", {E_Brightness, E_Contrast, E_Saturation, E_Hue, E_Gamma, E_Grayscale, E_Inverse, E_Sepia, E_Polaroid}},
            {L"Фильтры", L"Filters", {E_Blur, E_Sharpen, E_Pixelate, E_Shadow, E_Outline, E_Reflection}},
            {L"Рисование", L"Drawings", {E_Border, E_TextWatermark}},
            {L"Преобразования", L"Manipulations", {E_Canvas, E_Resize, E_Rotate, E_RoundedCorners}},
        };
        HMENU m = CreatePopupMenu();
        for (auto& g : groups) {
            HMENU sub = CreatePopupMenu();
            for (int t : g.items) {
                const EffectDef& d = GetEffectDef(t);
                AppendMenuW(sub, MF_STRING, t + 1, Tr(d.ru, d.en));
            }
            AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, Tr(g.ru, g.en));
        }
        Ctl* b = f.Find(ID_Add);
        POINT p{b->frame.left, b->frame.bottom};
        ClientToScreen(f.hwnd, &p);
        int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, f.hwnd, nullptr);
        DestroyMenu(m);
        if (id <= 0) return;
        int at = sel < 0 ? (int)stack.size() : sel + 1;
        stack.insert(stack.begin() + at, MakeEffect(id - 1));
        sel = at;
        BuildParams();
        Changed();
    }

    void Move(int dir) {
        int to = sel + dir;
        if (sel < 0 || to < 0 || to >= (int)stack.size()) return;
        std::swap(stack[sel], stack[to]);
        sel = to;
        Changed();
    }

    void Remove() {
        if (sel < 0 || sel >= (int)stack.size()) return;
        stack.erase(stack.begin() + sel);
        if (sel >= (int)stack.size()) sel = (int)stack.size() - 1;
        BuildParams();
        Changed();
    }

    void Toggle(int i) {
        if (i < 0 || i >= (int)stack.size()) return;
        stack[i].enabled = !stack[i].enabled;
        Changed();
    }

    bool Build(HWND owner) {
        f.W = 900;
        f.H = 560;
        if (!CreateForm(f, owner, Tr(L"Эффекты изображения", L"Image effects"))) return false;
        AddLabel(f, Tr(L"Эффекты (применяются сверху вниз)", L"Effects (applied top to bottom)"), 12, 12, 236, 24, DT_LEFT, true);
        AddList(f, ID_List, 12, 44, 236, 378);
        AddButton(f, ID_Add, 12, 430, 116, Tr(L"Добавить…", L"Add…"));
        AddButton(f, ID_Remove, 132, 430, 116, Tr(L"Удалить", L"Remove"));
        AddButton(f, ID_Up, 12, 464, 116, Tr(L"Выше", L"Move up"));
        AddButton(f, ID_Down, 132, 464, 116, Tr(L"Ниже", L"Move down"));
        AddButton(f, ID_Reset, 12, 498, 236, Tr(L"Очистить список", L"Clear all"));
        AddLabel(f, Tr(L"Предпросмотр", L"Preview"), 572, 12, 160, 24, DT_LEFT, true);
        box = f.R(572, 44, 316, 446);
        OkCancel(f);
        f.onPaint = [this](HDC dc) { PaintPreview(dc); };
        f.onDrawList = [this](const DRAWITEMSTRUCT& d) { DrawItem(d); };
        f.onPreview = [this] { OnPreviewDone(); };
        f.onCommand = [this](int id, int code) {
            if (id == ID_List && code == LBN_SELCHANGE) {
                int s = (int)SendMessageW(f.Find(ID_List)->hwnd, LB_GETCURSEL, 0, 0);
                if (s != sel) {
                    sel = s;
                    BuildParams();
                    RefreshList();
                }
            } else if (id == ID_List && code == LBN_DBLCLK) {
                Toggle(sel);
            } else if (code == BN_CLICKED) {
                if (id == ID_Add) AddMenu();
                else if (id == ID_Remove) Remove();
                else if (id == ID_Up) Move(-1);
                else if (id == ID_Down) Move(1);
                else if (id == ID_Reset && !stack.empty()) {
                    stack.clear();
                    sel = -1;
                    BuildParams();
                    Changed();
                }
            }
        };
        f.onListKey = [this](int vk) -> int {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (vk == VK_SPACE) { Toggle(sel); return -2; }
            if (vk == VK_DELETE) { Remove(); return -2; }
            if (ctrl && (vk == VK_UP || vk == VK_DOWN)) { Move(vk == VK_UP ? -1 : 1); return -2; }
            return -1;
        };
        f.onListClick = [this](int x, int item) {
            if (x > f.S(26) || item >= (int)stack.size()) return false;
            sel = item;
            Toggle(item);
            BuildParams();
            return true;
        };
        f.onChange = [this](Ctl& c) {
            if (c.param < 0 || sel < 0 || sel >= (int)stack.size()) return;
            Effect& e = stack[sel];
            switch (c.kind) {
            case K_Check: e.v[c.param] = c.checked; break;
            case K_Choice: e.v[c.param] = c.choice; break;
            case K_Color:
                e.v[c.param] = c.color;
                c.text = Hex(c.color);
                Redraw(c);
                break;
            case K_TextEdit: e.text = c.text; break;
            default: e.v[c.param] = c.Value(); break;
            }
            Changed();
        };
        if (!stack.empty()) sel = 0;
        BuildParams();
        RefreshList();
        StartPreview();
        return true;
    }
};

}  // namespace

// ---------- public ----------

void LoadImageEditorOptions(NewImageOptions& n, ImageSizeOptions& s, CanvasOptions& c) {
    std::wstring ini = Ini();
    auto hex = [&](const wchar_t* k, uint32_t def) {
        wchar_t b[32];
        GetPrivateProfileStringW(L"imageeditor", k, L"", b, 32, ini.c_str());
        return *b ? (uint32_t)wcstoul(b, nullptr, 16) : def;
    };
    n.w = std::clamp((int)GetPrivateProfileIntW(L"imageeditor", L"NewImageWidth", n.w, ini.c_str()), 1, 30000);
    n.h = std::clamp((int)GetPrivateProfileIntW(L"imageeditor", L"NewImageHeight", n.h, ini.c_str()), 1, 30000);
    n.transparent = GetPrivateProfileIntW(L"imageeditor", L"NewImageTransparent", n.transparent, ini.c_str()) != 0;
    n.color = hex(L"NewImageColor", n.color);
    c.color = hex(L"CanvasColor", c.color);
    s.resampling = std::clamp((int)GetPrivateProfileIntW(L"imageeditor", L"ResizeResampling", s.resampling, ini.c_str()), 0, RS_Count - 1);
    s.keepAspect = GetPrivateProfileIntW(L"imageeditor", L"ResizeKeepAspect", s.keepAspect, ini.c_str()) != 0;
}

void SaveImageEditorOptions(const NewImageOptions& n, const ImageSizeOptions& s, const CanvasOptions& c) {
    std::wstring ini = Ini();
    auto put = [&](const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"imageeditor", k, v.c_str(), ini.c_str()); };
    put(L"NewImageWidth", std::to_wstring(n.w));
    put(L"NewImageHeight", std::to_wstring(n.h));
    put(L"NewImageTransparent", n.transparent ? L"1" : L"0");
    put(L"NewImageColor", Hex(n.color));
    put(L"CanvasColor", Hex(c.color));
    put(L"ResizeResampling", std::to_wstring(s.resampling));
    put(L"ResizeKeepAspect", s.keepAspect ? L"1" : L"0");
}

namespace {
bool g_quietDialogs = false;
}

bool ShowNewImageDialog(HWND owner, NewImageOptions& o) {
    Form f;
    f.quiet = g_quietDialogs;
    f.W = 340;
    f.H = 214;
    if (!CreateForm(f, owner, Tr(L"Новое изображение", L"New image"))) return false;
    enum { W_ = 10, H_, T_, C_ };
    AddLabel(f, Tr(L"Ширина:", L"Width:"), 16, 16, 80);
    AddNumber(f, W_, 100, 16, 100, o.w, 1, 30000, 10, false);
    AddLabel(f, Tr(L"пикселей", L"pixels"), 208, 16, 100, 24, DT_LEFT, false, true);
    AddLabel(f, Tr(L"Высота:", L"Height:"), 16, 50, 80);
    AddNumber(f, H_, 100, 50, 100, o.h, 1, 30000, 10, false);
    AddLabel(f, Tr(L"пикселей", L"pixels"), 208, 50, 100, 24, DT_LEFT, false, true);
    AddLabel(f, Tr(L"Цвет фона", L"Background color"), 16, 90, 300, 24, DT_LEFT, true);
    AddCheck(f, T_, 16, 118, 140, Tr(L"Прозрачный", L"Transparent"), o.transparent);
    Ctl& col = AddColor(f, C_, 160, 118, 164, Tr(L"Изменить цвет…", L"Change color…"), o.color);
    EnableWindow(col.hwnd, !o.transparent);
    OkCancel(f);
    f.onChange = [&f](Ctl& c) {
        if (c.id == T_) EnableWindow(f.Find(C_)->hwnd, !c.checked);
        EnableWindow(f.Find(IDOK)->hwnd, f.Find(W_)->Value() > 0 && f.Find(H_)->Value() > 0);
    };
    bool ok = RunForm(f);
    if (ok) {
        o.w = (int)f.Find(W_)->Value();
        o.h = (int)f.Find(H_)->Value();
        o.transparent = f.Find(T_)->checked;
        o.color = f.Find(C_)->color;
    }
    DestroyForm(f);
    return ok;
}

bool ShowImageSizeDialog(HWND owner, int curW, int curH, ImageSizeOptions& o) {
    Form f;
    f.quiet = g_quietDialogs;
    f.W = 400;
    f.H = 214;
    if (!CreateForm(f, owner, Tr(L"Размер изображения", L"Image size"))) return false;
    enum { WP = 10, WR, HP, HR, KA, RS };
    curW = std::max(1, curW), curH = std::max(1, curH);
    AddLabel(f, Tr(L"Ширина:", L"Width:"), 16, 16, 100);
    AddNumber(f, WP, 124, 16, 90, curW, 1, 30000, 1, false);
    AddLabel(f, Tr(L"пикселей", L"pixels"), 220, 16, 70, 24, DT_LEFT, false, true);
    AddNumber(f, WR, 292, 16, 76, 100, .01, 10000, 1, true);
    AddLabel(f, L"%", 372, 16, 20, 24, DT_LEFT, false, true);
    AddLabel(f, Tr(L"Высота:", L"Height:"), 16, 50, 100);
    AddNumber(f, HP, 124, 50, 90, curH, 1, 30000, 1, false);
    AddLabel(f, Tr(L"пикселей", L"pixels"), 220, 50, 70, 24, DT_LEFT, false, true);
    AddNumber(f, HR, 292, 50, 76, 100, .01, 10000, 1, true);
    AddLabel(f, L"%", 372, 50, 20, 24, DT_LEFT, false, true);
    AddCheck(f, KA, 124, 84, 260, Tr(L"Сохранять пропорции", L"Maintain aspect ratio"), o.keepAspect);
    AddLabel(f, Tr(L"Масштабирование:", L"Resampling:"), 16, 120, 108);
    std::vector<std::wstring> modes;
    for (int i = 0; i < RS_Count; ++i) modes.push_back(ResamplingName(i));
    AddChoice(f, RS, 124, 120, 244, modes, o.resampling);
    OkCancel(f);
    auto set = [&f](int id, double v) {
        Ctl* c = f.Find(id);
        c->num = v;
        SetEditText(f, *c);
    };
    f.onChange = [&, curW, curH](Ctl& c) {
        if (c.id == RS) return;
        bool keep = f.Find(KA)->checked;
        double w = f.Find(WP)->Value(), h = f.Find(HP)->Value();
        if (c.id == WR) w = std::max(1.0, std::round(c.Value() * curW / 100));
        if (c.id == HR) h = std::max(1.0, std::round(c.Value() * curH / 100));
        bool widthLed = c.id == WP || c.id == WR || c.id == KA;
        if (keep) {
            if (widthLed) h = std::max(1.0, std::round(w * curH / curW));
            else w = std::max(1.0, std::round(h * curW / curH));
        }
        if (c.id != WP) set(WP, w);
        if (c.id != HP) set(HP, h);
        if (c.id != WR) set(WR, w * 100.0 / curW);
        if (c.id != HR) set(HR, h * 100.0 / curH);
    };
    bool ok = RunForm(f);
    if (ok) {
        o.w = (int)f.Find(WP)->Value();
        o.h = (int)f.Find(HP)->Value();
        o.keepAspect = f.Find(KA)->checked;
        o.resampling = f.Find(RS)->choice;
    }
    DestroyForm(f);
    return ok;
}

bool ShowCanvasSizeDialog(HWND owner, CanvasOptions& o) {
    Form f;
    f.quiet = g_quietDialogs;
    f.W = 340;
    f.H = 200;
    if (!CreateForm(f, owner, Tr(L"Размер полотна", L"Canvas size"))) return false;
    enum { L_ = 10, T_, R_, B_, C_ };
    AddLabel(f, Tr(L"Слева:", L"Left:"), 16, 16, 60);
    AddNumber(f, L_, 80, 16, 80, o.left, -30000, 30000, 1, false);
    AddLabel(f, Tr(L"Справа:", L"Right:"), 176, 16, 60);
    AddNumber(f, R_, 244, 16, 80, o.right, -30000, 30000, 1, false);
    AddLabel(f, Tr(L"Сверху:", L"Top:"), 16, 50, 60);
    AddNumber(f, T_, 80, 50, 80, o.top, -30000, 30000, 1, false);
    AddLabel(f, Tr(L"Снизу:", L"Bottom:"), 176, 50, 60);
    AddNumber(f, B_, 244, 50, 80, o.bottom, -30000, 30000, 1, false);
    AddColor(f, C_, 16, 90, 308, Tr(L"Цвет полотна…", L"Canvas color…"), o.color);
    AddLabel(f, Tr(L"Отрицательные значения обрезают изображение.", L"Negative values crop the image."), 16, 120, 308, 24, DT_LEFT, false, true);
    OkCancel(f);
    bool ok = RunForm(f);
    if (ok) {
        o.left = (int)f.Find(L_)->Value();
        o.top = (int)f.Find(T_)->Value();
        o.right = (int)f.Find(R_)->Value();
        o.bottom = (int)f.Find(B_)->Value();
        o.color = f.Find(C_)->color;
    }
    DestroyForm(f);
    return ok;
}

bool ShowImageEffectsDialog(HWND owner, ImagePtr src, std::vector<Effect>& stack) {
    if (!src || src->empty()) return false;
    auto d = std::make_unique<EffectsDlg>();
    d->f.quiet = g_quietDialogs;
    d->src = src;
    d->stack = stack;
    if (!d->Build(owner)) return false;
    if (d->f.quiet) {  // test: wait for the first preview, then grab the window
        DWORD t0 = GetTickCount();
        MSG m;
        while (GetTickCount() - t0 < 15000) {
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
            std::lock_guard<std::mutex> lk(d->prev->mu);
            if (d->prev->ready && !d->prev->running && !d->prev->pending) break;
            Sleep(10);
        }
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
    }
    bool ok = RunForm(d->f);
    d->prev->gen++;  // stop a pass still running
    if (ok) {
        stack = d->stack;
        SaveEffectStack(stack);
    }
    DestroyForm(d->f);
    return ok;
}

// --test-imageops in outdir: every operation and effect on `in`, the dialogs rendered off-screen,
// and the editor document test (undo, shapes following rotate/flip/resize/canvas).
bool ImageOpsTest(const std::wstring& in, const std::wstring& outDir) {
    Image src;
    if (!LoadImageFile(in, src)) return false;
    CreateDirectoryW(outDir.c_str(), nullptr);
    bool ok = true;
    auto save = [&](const std::wstring& name, const Image& im) {
        bool r = !im.empty() && EncodeImage(im, outDir + L"\\" + name + L".png", false);
        Log(L"test-imageops: %s %dx%d %s", name.c_str(), im.w, im.h, r ? L"ok" : L"FAILED");
        ok &= r;
    };
    LARGE_INTEGER fq, a, b;
    QueryPerformanceFrequency(&fq);
    auto ms = [&] { QueryPerformanceCounter(&b); double v = (b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart; a = b; return v; };
    QueryPerformanceCounter(&a);
    save(L"op_new", ImgNew(320, 200, 0xFF3366CC));
    for (int m = 0; m < RS_Count; ++m) save(L"op_resize50_mode" + std::to_wstring(m), ImgResize(src, src.w / 2, src.h / 2, m));
    save(L"op_canvas", ImgCanvas(src, 40, 20, 0, 60, 0xFFFF8800));
    save(L"op_canvas_transparent_crop", ImgCanvas(src, 30, -50, -80, 30, 0));
    save(L"op_rot90cw", ImgRotate90(src, 1));
    save(L"op_rot90ccw", ImgRotate90(src, 3));
    save(L"op_rot180", ImgRotate90(src, 2));
    save(L"op_fliph", ImgFlip(src, true));
    save(L"op_flipv", ImgFlip(src, false));
    Image framed = ImgCanvas(src, 37, 11, 23, 41, 0xFF00FF00);
    RECT ac = ImgAutoCropRect(framed, 0);
    bool acOk = ac.left == 37 && ac.top == 11 && ac.right == 37 + src.w && ac.bottom == 11 + src.h;
    Log(L"test-imageops: autocrop %d,%d,%d,%d expected 37,11,%d,%d %s", ac.left, ac.top, ac.right, ac.bottom, 37 + src.w, 11 + src.h,
        acOk ? L"ok" : L"(differs: the source has its own uniform border)");
    save(L"op_autocrop", ImgCrop(framed, ac));
    Log(L"test-imageops: basic ops %.0f ms", ms());
    for (int t = 0; t < E_Count; ++t) {
        Effect e = MakeEffect(t);
        if (t == E_Brightness) e.v[0] = .2;
        if (t == E_Hue) e.v[0] = 120;
        if (t == E_Contrast) e.v[0] = 1.4;
        if (t == E_Saturation) e.v[0] = 2;
        if (t == E_Gamma) e.v[0] = .6;
        if (t == E_Pixelate) e.v[1] = 1, e.v[2] = 0xFF202020;
        if (t == E_Border) e.v[1] = 8, e.v[2] = 1, e.v[3] = 0xFFFF0000;
        if (t == E_Shadow) e.v[1] = 20, e.v[4] = 8, e.v[5] = 8;
        if (t == E_Outline) e.v[0] = 4, e.v[2] = 0xFFFF0000;
        if (t == E_Reflection) e.v[4] = 1;
        if (t == E_Canvas) e.v[0] = e.v[1] = e.v[2] = e.v[3] = 30;
        if (t == E_Rotate) e.v[0] = 15;
        if (t == E_TextWatermark) e.text = L"Zipshot — водяной знак", e.v[5] = 18;
        QueryPerformanceCounter(&a);
        Image r = ApplyEffect(src, e);
        double took = ms();
        save(L"fx_" + std::wstring(GetEffectDef(t).key), r);
        Log(L"test-imageops: fx %s %.0f ms", GetEffectDef(t).key, took);
    }
    // ShareX-like preset: rounded corners + shadow + watermark; then the outline on the transparent result.
    std::vector<Effect> stack;
    stack.push_back(MakeEffect(E_RoundedCorners));
    stack.push_back(MakeEffect(E_Shadow));
    stack.push_back(MakeEffect(E_Outline));
    stack.back().v[0] = 2, stack.back().v[2] = 0xFF008AFF;
    stack.push_back(MakeEffect(E_TextWatermark));
    QueryPerformanceCounter(&a);
    save(L"fx_stack", ApplyEffects(src, stack));
    Log(L"test-imageops: stack %.0f ms", ms());

    // Effect stack round trip through zipshot.ini (restores the user's stack afterwards).
    std::vector<Effect> saved = LoadEffectStack();
    stack[3].text = L"a|b\\c\nd";
    SaveEffectStack(stack);
    std::vector<Effect> back = LoadEffectStack();
    bool rt = back.size() == stack.size();
    for (size_t i = 0; rt && i < back.size(); ++i) {
        rt = back[i].type == stack[i].type && back[i].text == stack[i].text && back[i].enabled == stack[i].enabled;
        for (int k = 0; rt && k < kMaxEffectParams; ++k) rt = std::abs(back[i].v[k] - stack[i].v[k]) < 1e-6;
    }
    SaveEffectStack(saved);
    Log(L"test-imageops: ini round trip %s", rt ? L"ok" : L"FAILED");
    ok &= rt;

    // Dialogs, rendered off-screen.
    g_quietDialogs = true;
    NewImageOptions n;
    ImageSizeOptions s;
    CanvasOptions c;
    g_shotPath = outDir + L"\\dlg_new.png";
    ShowNewImageDialog(nullptr, n);
    g_shotPath = outDir + L"\\dlg_size.png";
    ShowImageSizeDialog(nullptr, src.w, src.h, s);
    c.left = c.right = 10;
    g_shotPath = outDir + L"\\dlg_canvas.png";
    ShowCanvasSizeDialog(nullptr, c);
    g_shotPath = outDir + L"\\dlg_effects.png";
    stack[3].text = L"Zipshot";
    ShowImageEffectsDialog(nullptr, std::make_shared<Image>(src), stack);
    g_quietDialogs = false;

    bool ed = EditorImageOpsTest(in, outDir);
    Log(L"test-imageops: editor document test %s", ed ? L"ok" : L"FAILED");
    return ok && ed;
}
