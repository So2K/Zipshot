#include "sharex_import.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <initializer_list>

// ShareX settings import: reads ApplicationConfig.json / HotkeysConfig.json (data only) and writes zipshot.ini.

namespace {

// ---------------------------------------------------------------- JSON

struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::wstring s;
    std::vector<J> a;
    std::vector<std::pair<std::wstring, J>> o;

    const J& operator[](const wchar_t* k) const {
        static const J null;
        if (t == Obj)
            for (auto& kv : o)
                if (kv.first == k) return kv.second;
        return null;
    }
    bool has() const { return t != Null; }
};

class Parser {
public:
    explicit Parser(const std::wstring& text) : s_(text) {}
    bool Parse(J& out) {
        if (!Value(out, 0)) return false;
        Ws();
        return i_ == s_.size();
    }

private:
    const std::wstring& s_;
    size_t i_ = 0;

    void Ws() {
        while (i_ < s_.size()) {
            wchar_t c = s_[i_];
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') ++i_;
            else if (c == L'/' && i_ + 1 < s_.size() && s_[i_ + 1] == L'/') {  // tolerate comments
                while (i_ < s_.size() && s_[i_] != L'\n') ++i_;
            } else break;
        }
    }
    bool Lit(const wchar_t* w) {
        size_t n = wcslen(w);
        if (s_.compare(i_, n, w) != 0) return false;
        i_ += n;
        return true;
    }
    static int Hex(wchar_t c) {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    }
    bool String(std::wstring& out) {
        if (i_ >= s_.size() || s_[i_] != L'"') return false;
        ++i_;
        while (i_ < s_.size()) {
            wchar_t c = s_[i_++];
            if (c == L'"') return true;
            if (c != L'\\') { out += c; continue; }
            if (i_ >= s_.size()) return false;
            wchar_t e = s_[i_++];
            switch (e) {
            case L'"': out += L'"'; break;
            case L'\\': out += L'\\'; break;
            case L'/': out += L'/'; break;
            case L'b': out += L'\b'; break;
            case L'f': out += L'\f'; break;
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            case L'u': {
                if (i_ + 4 > s_.size()) return false;
                int v = 0;
                for (int k = 0; k < 4; ++k) {
                    int h = Hex(s_[i_++]);
                    if (h < 0) return false;
                    v = v * 16 + h;
                }
                out += (wchar_t)v;  // UTF-16 surrogates pass through as-is
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Value(J& v, int depth) {
        if (depth > 256) return false;
        Ws();
        if (i_ >= s_.size()) return false;
        wchar_t c = s_[i_];
        if (c == L'{') {
            ++i_;
            v.t = J::Obj;
            Ws();
            if (i_ < s_.size() && s_[i_] == L'}') { ++i_; return true; }
            for (;;) {
                Ws();
                std::wstring k;
                if (!String(k)) return false;
                Ws();
                if (i_ >= s_.size() || s_[i_] != L':') return false;
                ++i_;
                J child;
                if (!Value(child, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(child));
                Ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == L',') { ++i_; continue; }
                if (s_[i_] == L'}') { ++i_; return true; }
                return false;
            }
        }
        if (c == L'[') {
            ++i_;
            v.t = J::Arr;
            Ws();
            if (i_ < s_.size() && s_[i_] == L']') { ++i_; return true; }
            for (;;) {
                J child;
                if (!Value(child, depth + 1)) return false;
                v.a.push_back(std::move(child));
                Ws();
                if (i_ >= s_.size()) return false;
                if (s_[i_] == L',') { ++i_; continue; }
                if (s_[i_] == L']') { ++i_; return true; }
                return false;
            }
        }
        if (c == L'"') { v.t = J::Str; return String(v.s); }
        if (Lit(L"true")) { v.t = J::Bool; v.b = true; return true; }
        if (Lit(L"false")) { v.t = J::Bool; v.b = false; return true; }
        if (Lit(L"null")) { v.t = J::Null; return true; }
        size_t start = i_;
        if (c == L'-' || c == L'+') ++i_;
        while (i_ < s_.size() && (iswdigit(s_[i_]) || s_[i_] == L'.' || s_[i_] == L'e' || s_[i_] == L'E' || s_[i_] == L'-' || s_[i_] == L'+'))
            ++i_;
        if (i_ == start) return false;
        std::wstring num = s_.substr(start, i_ - start);
        wchar_t* end = nullptr;
        v.n = wcstod(num.c_str(), &end);
        if (!end || *end) return false;
        v.t = J::Num;
        return true;
    }
};

bool ReadJsonFile(const std::wstring& path, J& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    if (size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) { CloseHandle(h); return false; }
    std::string bytes((size_t)size.QuadPart, '\0');
    DWORD rd = 0;
    BOOL ok = ReadFile(h, bytes.data(), (DWORD)bytes.size(), &rd, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    bytes.resize(rd);
    size_t off = bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF ? 3 : 0;
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + off, (int)(bytes.size() - off), nullptr, 0);
    std::wstring text(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data() + off, (int)(bytes.size() - off), text.data(), n);
    return Parser(text).Parse(out);
}

// ---------------------------------------------------------------- colours

struct NamedColor { const wchar_t* name; uint32_t rgb; };
const NamedColor kWebColors[] = {
    {L"AliceBlue", 0xF0F8FF}, {L"AntiqueWhite", 0xFAEBD7}, {L"Aqua", 0x00FFFF}, {L"Aquamarine", 0x7FFFD4}, {L"Azure", 0xF0FFFF},
    {L"Beige", 0xF5F5DC}, {L"Bisque", 0xFFE4C4}, {L"Black", 0x000000}, {L"BlanchedAlmond", 0xFFEBCD}, {L"Blue", 0x0000FF},
    {L"BlueViolet", 0x8A2BE2}, {L"Brown", 0xA52A2A}, {L"BurlyWood", 0xDEB887}, {L"CadetBlue", 0x5F9EA0}, {L"Chartreuse", 0x7FFF00},
    {L"Chocolate", 0xD2691E}, {L"Coral", 0xFF7F50}, {L"CornflowerBlue", 0x6495ED}, {L"Cornsilk", 0xFFF8DC}, {L"Crimson", 0xDC143C},
    {L"Cyan", 0x00FFFF}, {L"DarkBlue", 0x00008B}, {L"DarkCyan", 0x008B8B}, {L"DarkGoldenrod", 0xB8860B}, {L"DarkGray", 0xA9A9A9},
    {L"DarkGreen", 0x006400}, {L"DarkKhaki", 0xBDB76B}, {L"DarkMagenta", 0x8B008B}, {L"DarkOliveGreen", 0x556B2F},
    {L"DarkOrange", 0xFF8C00}, {L"DarkOrchid", 0x9932CC}, {L"DarkRed", 0x8B0000}, {L"DarkSalmon", 0xE9967A},
    {L"DarkSeaGreen", 0x8FBC8B}, {L"DarkSlateBlue", 0x483D8B}, {L"DarkSlateGray", 0x2F4F4F}, {L"DarkTurquoise", 0x00CED1},
    {L"DarkViolet", 0x9400D3}, {L"DeepPink", 0xFF1493}, {L"DeepSkyBlue", 0x00BFFF}, {L"DimGray", 0x696969}, {L"DodgerBlue", 0x1E90FF},
    {L"Firebrick", 0xB22222}, {L"FloralWhite", 0xFFFAF0}, {L"ForestGreen", 0x228B22}, {L"Fuchsia", 0xFF00FF}, {L"Gainsboro", 0xDCDCDC},
    {L"GhostWhite", 0xF8F8FF}, {L"Gold", 0xFFD700}, {L"Goldenrod", 0xDAA520}, {L"Gray", 0x808080}, {L"Green", 0x008000},
    {L"GreenYellow", 0xADFF2F}, {L"Honeydew", 0xF0FFF0}, {L"HotPink", 0xFF69B4}, {L"IndianRed", 0xCD5C5C}, {L"Indigo", 0x4B0082},
    {L"Ivory", 0xFFFFF0}, {L"Khaki", 0xF0E68C}, {L"Lavender", 0xE6E6FA}, {L"LavenderBlush", 0xFFF0F5}, {L"LawnGreen", 0x7CFC00},
    {L"LemonChiffon", 0xFFFACD}, {L"LightBlue", 0xADD8E6}, {L"LightCoral", 0xF08080}, {L"LightCyan", 0xE0FFFF},
    {L"LightGoldenrodYellow", 0xFAFAD2}, {L"LightGray", 0xD3D3D3}, {L"LightGreen", 0x90EE90}, {L"LightPink", 0xFFB6C1},
    {L"LightSalmon", 0xFFA07A}, {L"LightSeaGreen", 0x20B2AA}, {L"LightSkyBlue", 0x87CEFA}, {L"LightSlateGray", 0x778899},
    {L"LightSteelBlue", 0xB0C4DE}, {L"LightYellow", 0xFFFFE0}, {L"Lime", 0x00FF00}, {L"LimeGreen", 0x32CD32}, {L"Linen", 0xFAF0E6},
    {L"Magenta", 0xFF00FF}, {L"Maroon", 0x800000}, {L"MediumAquamarine", 0x66CDAA}, {L"MediumBlue", 0x0000CD},
    {L"MediumOrchid", 0xBA55D3}, {L"MediumPurple", 0x9370DB}, {L"MediumSeaGreen", 0x3CB371}, {L"MediumSlateBlue", 0x7B68EE},
    {L"MediumSpringGreen", 0x00FA9A}, {L"MediumTurquoise", 0x48D1CC}, {L"MediumVioletRed", 0xC71585}, {L"MidnightBlue", 0x191970},
    {L"MintCream", 0xF5FFFA}, {L"MistyRose", 0xFFE4E1}, {L"Moccasin", 0xFFE4B5}, {L"NavajoWhite", 0xFFDEAD}, {L"Navy", 0x000080},
    {L"OldLace", 0xFDF5E6}, {L"Olive", 0x808000}, {L"OliveDrab", 0x6B8E23}, {L"Orange", 0xFFA500}, {L"OrangeRed", 0xFF4500},
    {L"Orchid", 0xDA70D6}, {L"PaleGoldenrod", 0xEEE8AA}, {L"PaleGreen", 0x98FB98}, {L"PaleTurquoise", 0xAFEEEE},
    {L"PaleVioletRed", 0xDB7093}, {L"PapayaWhip", 0xFFEFD5}, {L"PeachPuff", 0xFFDAB9}, {L"Peru", 0xCD853F}, {L"Pink", 0xFFC0CB},
    {L"Plum", 0xDDA0DD}, {L"PowderBlue", 0xB0E0E6}, {L"Purple", 0x800080}, {L"RebeccaPurple", 0x663399}, {L"Red", 0xFF0000},
    {L"RosyBrown", 0xBC8F8F}, {L"RoyalBlue", 0x4169E1}, {L"SaddleBrown", 0x8B4513}, {L"Salmon", 0xFA8072}, {L"SandyBrown", 0xF4A460},
    {L"SeaGreen", 0x2E8B57}, {L"SeaShell", 0xFFF5EE}, {L"Sienna", 0xA0522D}, {L"Silver", 0xC0C0C0}, {L"SkyBlue", 0x87CEEB},
    {L"SlateBlue", 0x6A5ACD}, {L"SlateGray", 0x708090}, {L"Snow", 0xFFFAFA}, {L"SpringGreen", 0x00FF7F}, {L"SteelBlue", 0x4682B4},
    {L"Tan", 0xD2B48C}, {L"Teal", 0x008080}, {L"Thistle", 0xD8BFD8}, {L"Tomato", 0xFF6347}, {L"Turquoise", 0x40E0D0},
    {L"Violet", 0xEE82EE}, {L"Wheat", 0xF5DEB3}, {L"White", 0xFFFFFF}, {L"WhiteSmoke", 0xF5F5F5}, {L"Yellow", 0xFFFF00},
    {L"YellowGreen", 0x9ACD32},
};
struct SysColor { const wchar_t* name; int index; };
const SysColor kSysColors[] = {
    {L"ActiveBorder", COLOR_ACTIVEBORDER}, {L"ActiveCaption", COLOR_ACTIVECAPTION}, {L"ActiveCaptionText", COLOR_CAPTIONTEXT},
    {L"AppWorkspace", COLOR_APPWORKSPACE}, {L"ButtonFace", COLOR_BTNFACE}, {L"ButtonHighlight", COLOR_BTNHIGHLIGHT},
    {L"ButtonShadow", COLOR_BTNSHADOW}, {L"Control", COLOR_BTNFACE}, {L"ControlDark", COLOR_BTNSHADOW},
    {L"ControlDarkDark", COLOR_3DDKSHADOW}, {L"ControlLight", COLOR_3DLIGHT}, {L"ControlLightLight", COLOR_BTNHIGHLIGHT},
    {L"ControlText", COLOR_BTNTEXT}, {L"Desktop", COLOR_DESKTOP}, {L"GradientActiveCaption", COLOR_GRADIENTACTIVECAPTION},
    {L"GradientInactiveCaption", COLOR_GRADIENTINACTIVECAPTION}, {L"GrayText", COLOR_GRAYTEXT}, {L"Highlight", COLOR_HIGHLIGHT},
    {L"HighlightText", COLOR_HIGHLIGHTTEXT}, {L"HotTrack", COLOR_HOTLIGHT}, {L"InactiveBorder", COLOR_INACTIVEBORDER},
    {L"InactiveCaption", COLOR_INACTIVECAPTION}, {L"InactiveCaptionText", COLOR_INACTIVECAPTIONTEXT}, {L"Info", COLOR_INFOBK},
    {L"InfoText", COLOR_INFOTEXT}, {L"Menu", COLOR_MENU}, {L"MenuBar", COLOR_MENUBAR}, {L"MenuHighlight", COLOR_MENUHILIGHT},
    {L"MenuText", COLOR_MENUTEXT}, {L"ScrollBar", COLOR_SCROLLBAR}, {L"Window", COLOR_WINDOW}, {L"WindowFrame", COLOR_WINDOWFRAME},
    {L"WindowText", COLOR_WINDOWTEXT},
};

std::wstring Trim(std::wstring s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> v;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        v.push_back(Trim(s.substr(start, p == std::wstring::npos ? std::wstring::npos : p - start)));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    return v;
}

bool IsInt(const std::wstring& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (!iswdigit(s[i]) && !(i == 0 && s[i] == L'-')) return false;
    return true;
}

// ShareX serialises System.Drawing.Color as "R, G, B", "A, R, G, B" or a known colour name. Returns AARRGGBB.
bool ParseColor(const std::wstring& raw, uint32_t& argb) {
    std::wstring s = Trim(raw);
    if (s.empty()) return false;
    auto clamp = [](int v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    std::vector<std::wstring> parts = Split(s, L',');
    if (parts.size() == 3 || parts.size() == 4) {
        for (auto& p : parts)
            if (!IsInt(p)) return false;
        uint32_t c[4];
        for (size_t i = 0; i < parts.size(); ++i) c[i] = clamp(_wtoi(parts[i].c_str()));
        argb = parts.size() == 3 ? 0xFF000000u | c[0] << 16 | c[1] << 8 | c[2] : c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3];
        return true;
    }
    if (s[0] == L'#' && (s.size() == 7 || s.size() == 9)) {
        uint32_t v = wcstoul(s.c_str() + 1, nullptr, 16);
        argb = s.size() == 7 ? 0xFF000000u | v : v;
        return true;
    }
    if (IsInt(s)) {  // Color.ToArgb()
        argb = (uint32_t)_wtoi64(s.c_str());
        return true;
    }
    if (_wcsicmp(s.c_str(), L"Transparent") == 0 || _wcsicmp(s.c_str(), L"Empty") == 0) {
        argb = 0x00FFFFFF;
        return true;
    }
    for (auto& c : kWebColors)
        if (_wcsicmp(s.c_str(), c.name) == 0) { argb = 0xFF000000u | c.rgb; return true; }
    for (auto& c : kSysColors)
        if (_wcsicmp(s.c_str(), c.name) == 0) {
            COLORREF cr = GetSysColor(c.index);
            argb = 0xFF000000u | (uint32_t)GetRValue(cr) << 16 | (uint32_t)GetGValue(cr) << 8 | GetBValue(cr);
            return true;
        }
    return false;
}

// ---------------------------------------------------------------- keys (.NET Keys names == VK codes)

struct KeyName { const wchar_t* name; UINT vk; };
const KeyName kKeyNames[] = {
    {L"PrintScreen", VK_SNAPSHOT}, {L"Snapshot", VK_SNAPSHOT}, {L"Pause", VK_PAUSE}, {L"Scroll", VK_SCROLL},
    {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE}, {L"Home", VK_HOME}, {L"End", VK_END}, {L"PageUp", VK_PRIOR},
    {L"Prior", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Next", VK_NEXT}, {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"Left", VK_LEFT},
    {L"Right", VK_RIGHT}, {L"Space", VK_SPACE}, {L"Enter", VK_RETURN}, {L"Return", VK_RETURN}, {L"Escape", VK_ESCAPE},
    {L"Tab", VK_TAB}, {L"Back", VK_BACK}, {L"Capital", VK_CAPITAL}, {L"CapsLock", VK_CAPITAL}, {L"NumLock", VK_NUMLOCK},
    {L"Apps", VK_APPS}, {L"Multiply", VK_MULTIPLY}, {L"Add", VK_ADD}, {L"Subtract", VK_SUBTRACT}, {L"Decimal", VK_DECIMAL},
    {L"Divide", VK_DIVIDE}, {L"Separator", VK_SEPARATOR}, {L"Clear", VK_CLEAR}, {L"Help", VK_HELP}, {L"Sleep", VK_SLEEP},
    {L"Oemtilde", VK_OEM_3}, {L"Oem3", VK_OEM_3}, {L"OemMinus", VK_OEM_MINUS}, {L"Oemplus", VK_OEM_PLUS},
    {L"OemOpenBrackets", VK_OEM_4}, {L"Oem4", VK_OEM_4}, {L"OemCloseBrackets", VK_OEM_6}, {L"Oem6", VK_OEM_6},
    {L"OemPipe", VK_OEM_5}, {L"Oem5", VK_OEM_5}, {L"OemSemicolon", VK_OEM_1}, {L"Oem1", VK_OEM_1}, {L"OemQuotes", VK_OEM_7},
    {L"Oem7", VK_OEM_7}, {L"Oemcomma", VK_OEM_COMMA}, {L"OemPeriod", VK_OEM_PERIOD}, {L"OemQuestion", VK_OEM_2},
    {L"Oem2", VK_OEM_2}, {L"OemBackslash", VK_OEM_102}, {L"Oem102", VK_OEM_102}, {L"Oem8", VK_OEM_8},
    {L"BrowserBack", VK_BROWSER_BACK}, {L"BrowserForward", VK_BROWSER_FORWARD}, {L"BrowserRefresh", VK_BROWSER_REFRESH},
    {L"BrowserStop", VK_BROWSER_STOP}, {L"BrowserSearch", VK_BROWSER_SEARCH}, {L"BrowserFavorites", VK_BROWSER_FAVORITES},
    {L"BrowserHome", VK_BROWSER_HOME}, {L"VolumeMute", VK_VOLUME_MUTE}, {L"VolumeDown", VK_VOLUME_DOWN},
    {L"VolumeUp", VK_VOLUME_UP}, {L"MediaNextTrack", VK_MEDIA_NEXT_TRACK}, {L"MediaPreviousTrack", VK_MEDIA_PREV_TRACK},
    {L"MediaStop", VK_MEDIA_STOP}, {L"MediaPlayPause", VK_MEDIA_PLAY_PAUSE}, {L"LaunchMail", VK_LAUNCH_MAIL},
    {L"SelectMedia", VK_LAUNCH_MEDIA_SELECT}, {L"LaunchApplication1", VK_LAUNCH_APP1}, {L"LaunchApplication2", VK_LAUNCH_APP2},
};

std::wstring KeyNameOf(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z')) return std::wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return L"D" + std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"NumPad" + std::to_wstring(vk - VK_NUMPAD0);
    for (auto& k : kKeyNames)
        if (k.vk == vk) return k.name;
    wchar_t b[16];
    swprintf_s(b, L"VK%u", vk);
    return b;
}

bool VkOf(const std::wstring& n, UINT& vk) {
    if (n.size() == 1 && iswalpha(n[0])) { vk = (UINT)towupper(n[0]); return vk >= 'A' && vk <= 'Z'; }
    if (n.size() == 1 && iswdigit(n[0])) { vk = n[0]; return true; }
    if (n.size() == 2 && (n[0] == L'D' || n[0] == L'd') && iswdigit(n[1])) { vk = n[1]; return true; }
    if (n.size() >= 2 && (n[0] == L'F' || n[0] == L'f') && IsInt(n.substr(1))) {
        int f = _wtoi(n.c_str() + 1);
        if (f >= 1 && f <= 24) { vk = VK_F1 + f - 1; return true; }
    }
    if (_wcsnicmp(n.c_str(), L"NumPad", 6) == 0 && n.size() == 7 && iswdigit(n[6])) { vk = VK_NUMPAD0 + (n[6] - L'0'); return true; }
    if (_wcsnicmp(n.c_str(), L"VK", 2) == 0 && IsInt(n.substr(2))) {
        vk = (UINT)_wtoi(n.c_str() + 2);
        return vk > 0 && vk < 256;
    }
    for (auto& k : kKeyNames)
        if (_wcsicmp(n.c_str(), k.name) == 0) { vk = k.vk; return true; }
    return false;
}

// "PrintScreen, Shift, Control" (+ HotkeyInfo.Win) -> mods/vk. Returns false for "None" or unknown keys.
bool ParseNetHotkey(const std::wstring& s, bool win, UINT& mods, UINT& vk) {
    mods = win ? MOD_WIN : 0;
    vk = 0;
    for (auto& p : Split(s, L',')) {
        if (p.empty() || _wcsicmp(p.c_str(), L"None") == 0) continue;
        if (_wcsicmp(p.c_str(), L"Control") == 0) mods |= MOD_CONTROL;
        else if (_wcsicmp(p.c_str(), L"Shift") == 0) mods |= MOD_SHIFT;
        else if (_wcsicmp(p.c_str(), L"Alt") == 0) mods |= MOD_ALT;
        else if (!VkOf(p, vk)) return false;
    }
    return vk != 0;
}

// ---------------------------------------------------------------- ini writing

struct IniWriter {
    std::wstring path;
    int written = 0;
    void Str(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
        WritePrivateProfileStringW(sec, key, v.c_str(), path.c_str());
        ++written;
    }
    void Int(const wchar_t* sec, const wchar_t* key, long long v) { Str(sec, key, std::to_wstring(v)); }
    void Bool(const wchar_t* sec, const wchar_t* key, bool v) { Str(sec, key, v ? L"1" : L"0"); }
    void Num(const wchar_t* sec, const wchar_t* key, double v) {
        wchar_t b[32];
        swprintf_s(b, L"%g", v);
        Str(sec, key, b);
    }
};

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring Expand(const std::wstring& s) {
    wchar_t buf[2048];
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 2048);
    return n && n <= 2048 ? std::wstring(buf) : s;
}

// Folders that may hold ShareX's ApplicationConfig.json, most specific first.
std::vector<std::wstring> CandidateDirs() {
    std::vector<std::wstring> v;
    const std::wstring portable = L"D:\\Programs\\ShareX";
    // PersonalPath.cfg next to ShareX.exe redirects the settings folder.
    HANDLE h = CreateFileW((portable + L"\\PersonalPath.cfg").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        char buf[1024]{};
        DWORD rd = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
        CloseHandle(h);
        size_t off = rd >= 3 && (unsigned char)buf[0] == 0xEF ? 3 : 0;
        wchar_t w[1024]{};
        MultiByteToWideChar(CP_UTF8, 0, buf + off, (int)(rd - off), w, 1023);
        std::wstring p = Trim(Expand(w));
        if (!p.empty()) {
            if (PathIsRelativeW(p.c_str())) p = portable + L"\\" + p;
            v.push_back(p);
        }
    }
    v.push_back(portable + L"\\ShareX");
    wchar_t* docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) v.push_back(std::wstring(docs) + L"\\ShareX");
    CoTaskMemFree(docs);
    v.push_back(Expand(L"%USERPROFILE%\\Documents\\ShareX"));
    return v;
}

int ToolIndex(const std::wstring& name) {
    static const wchar_t* tools[] = {L"RegionRectangle", L"RegionEllipse", L"RegionFreehand", L"ToolSelect", L"DrawingRectangle",
                                     L"DrawingEllipse", L"DrawingFreehand", L"DrawingFreehandArrow", L"DrawingLine", L"DrawingArrow",
                                     L"DrawingTextOutline", L"DrawingTextBackground", L"DrawingStep", L"DrawingMagnify",
                                     L"DrawingSmartEraser", L"EffectBlur", L"EffectPixelate", L"EffectHighlight", L"ToolCrop",
                                     // appended in editor.cpp's Tool order
                                     L"DrawingSpeechBalloon", L"DrawingImage", L"DrawingImageScreen", L"DrawingSticker",
                                     L"DrawingCursor", L"ToolCutOut"};
    for (int i = 0; i < (int)std::size(tools); ++i)
        if (name == tools[i]) return i;
    return -1;
}

bool HasFlag(const std::wstring& flags, const wchar_t* f) {
    for (auto& p : Split(flags, L','))
        if (p == f) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------- shared helpers

std::wstring ZipshotIni() { return ExeDir() + L"\\zipshot.ini"; }

int IniInt(const wchar_t* section, const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(section, key, def, ZipshotIni().c_str()); }

std::wstring IniString(const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(section, key, def, buf, (DWORD)std::size(buf), ZipshotIni().c_str());
    return buf;
}

void EnsureUnicodeIni() {
    std::wstring ini = ZipshotIni();
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const BYTE bom[2] = {0xFF, 0xFE};
    DWORD wr;
    WriteFile(h, bom, 2, &wr, nullptr);
    CloseHandle(h);
}

bool ParseHotkeyText(const std::wstring& text, UINT& mods, UINT& vk) {
    mods = 0;
    vk = 0;
    std::wstring s = Trim(text);
    if (s.empty() || _wcsicmp(s.c_str(), L"None") == 0) return false;
    for (auto& p : Split(s, L'+')) {
        if (p.empty()) continue;
        if (_wcsicmp(p.c_str(), L"Ctrl") == 0 || _wcsicmp(p.c_str(), L"Control") == 0) mods |= MOD_CONTROL;
        else if (_wcsicmp(p.c_str(), L"Shift") == 0) mods |= MOD_SHIFT;
        else if (_wcsicmp(p.c_str(), L"Alt") == 0) mods |= MOD_ALT;
        else if (_wcsicmp(p.c_str(), L"Win") == 0) mods |= MOD_WIN;
        else if (!VkOf(p, vk)) return false;
    }
    return vk != 0;
}

std::wstring HotkeyIniText(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl+";
    if (mods & MOD_SHIFT) s += L"Shift+";
    if (mods & MOD_ALT) s += L"Alt+";
    if (mods & MOD_WIN) s += L"Win+";
    return s + KeyNameOf(vk);
}

// ShareX HotkeyInfo.ToString(): "Ctrl + Shift + Alt + Win + Print Screen", digits without the D, names split on case.
std::wstring HotkeyDisplayText(UINT mods, UINT vk) {
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_SHIFT) s += L"Shift + ";
    if (mods & MOD_ALT) s += L"Alt + ";
    if (mods & MOD_WIN) s += L"Win + ";
    if (vk >= '0' && vk <= '9') return s + (wchar_t)vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return s + L"Numpad " + std::to_wstring(vk - VK_NUMPAD0);
    std::wstring n = KeyNameOf(vk), spaced;
    for (size_t i = 0; i < n.size(); ++i) {
        if (i > 0 && iswupper(n[i]) && iswlower(n[i - 1])) spaced += L' ';
        spaced += n[i];
    }
    return s + spaced;
}

// ---------------------------------------------------------------- import

bool ImportShareXSettings(std::wstring& report) {
    report.clear();
    std::wstring dir;
    J app, hk;
    bool haveApp = false, haveHk = false;
    for (auto& d : CandidateDirs()) {
        bool a = FileExists(d + L"\\ApplicationConfig.json"), h = FileExists(d + L"\\HotkeysConfig.json");
        if (!a && !h) continue;
        dir = d;
        haveApp = a && ReadJsonFile(d + L"\\ApplicationConfig.json", app);
        haveHk = h && ReadJsonFile(d + L"\\HotkeysConfig.json", hk);
        break;
    }
    if (dir.empty()) {
        report = Tr(L"Настройки ShareX не найдены (D:\\Programs\\ShareX\\ShareX, Документы\\ShareX).",
                    L"ShareX settings not found (D:\\Programs\\ShareX\\ShareX, Documents\\ShareX).");
        return false;
    }
    if (!haveApp && !haveHk) {
        report = std::wstring(Tr(L"Не удалось прочитать настройки ShareX в ", L"Could not read ShareX settings in ")) + dir;
        return false;
    }

    std::wstring ini = ZipshotIni();
    if (FileExists(ini)) CopyFileW(ini.c_str(), (ini + L".bak").c_str(), FALSE);
    EnsureUnicodeIni();
    IniWriter w{ini};
    std::wstring done, skipped;
    auto note = [](std::wstring& list, const std::wstring& s) {
        if (!list.empty()) list += L"\n";
        list += L"  " + s;
    };
    report = std::wstring(Tr(L"Источник: ", L"Source: ")) + dir + L"\n";

    // ---- hotkeys
    if (haveHk) {
        struct JobMap { const wchar_t* job; const wchar_t* key; };
        static const JobMap jobs[] = {{L"RectangleRegion", L"Region"}, {L"PrintScreen", L"Full"}, {L"ActiveWindow", L"Window"},
                                      {L"ActiveMonitor", L"ActiveMonitor"}, {L"ScreenRecorder", L"Video"}, {L"ScreenRecorderGIF", L"Gif"},
                                      {L"LastRegion", L"LastRegion"}};
        std::vector<std::pair<std::wstring, std::wstring>> out;  // ini key -> "a; b"
        std::wstring hkDone;
        for (const J& e : hk[L"Hotkeys"].a) {
            std::wstring job = e[L"TaskSettings"][L"Job"].s;
            std::wstring text = e[L"HotkeyInfo"][L"Hotkey"].s;
            bool win = e[L"HotkeyInfo"][L"Win"].b;
            const wchar_t* key = nullptr;
            for (auto& j : jobs)
                if (job == j.job) key = j.key;
            UINT mods, vk;
            bool valid = ParseNetHotkey(text, win, mods, vk);
            if (!key) {
                note(skipped, std::wstring(Tr(L"хоткей ", L"hotkey ")) + job + L" (" + (valid ? HotkeyDisplayText(mods, vk) : text) + L")");
                continue;
            }
            auto it = out.begin();
            while (it != out.end() && it->first != key) ++it;
            if (it == out.end()) { out.emplace_back(key, L""); it = out.end() - 1; }
            if (valid) {
                if (!it->second.empty()) it->second += L"; ";
                it->second += HotkeyIniText(mods, vk);
                if (!hkDone.empty()) hkDone += L", ";
                hkDone += std::wstring(key) + L"=" + HotkeyDisplayText(mods, vk);
            }
        }
        WritePrivateProfileStringW(L"hotkeys", nullptr, nullptr, ini.c_str());  // replace the whole section
        for (auto& kv : out) w.Str(L"hotkeys", kv.first.c_str(), kv.second);
        if (out.empty()) w.Str(L"hotkeys", L"Region", L"");  // keep the section: ShareX had none
        note(done, std::wstring(Tr(L"Хоткеи: ", L"Hotkeys: ")) + (hkDone.empty() ? Tr(L"нет", L"none") : hkDone));
    }

    if (haveApp) {
        const J& ts = app[L"DefaultTaskSettings"];
        // ---- after capture
        if (ts[L"AfterCaptureJob"].t == J::Str) {
            std::wstring f = ts[L"AfterCaptureJob"].s;
            w.Bool(L"main", L"AfterCopy", HasFlag(f, L"CopyImageToClipboard"));
            w.Bool(L"main", L"AfterSave", HasFlag(f, L"SaveImageToFile") || HasFlag(f, L"SaveImageToFileWithDialog"));
            w.Bool(L"main", L"AfterEditor", HasFlag(f, L"AnnotateImage"));
            note(done, std::wstring(Tr(L"После захвата: ", L"After capture: ")) + f);
            for (auto& p : Split(f, L','))
                if (!p.empty() && p != L"CopyImageToClipboard" && p != L"SaveImageToFile" && p != L"SaveImageToFileWithDialog" &&
                    p != L"AnnotateImage")
                    note(skipped, std::wstring(Tr(L"после захвата: ", L"after capture: ")) + p);
        }
        const J& gs = ts[L"GeneralSettings"];
        if (gs.has()) {
            if (gs[L"ShowToastNotificationAfterTaskCompleted"].t == J::Bool)
                w.Bool(L"main", L"AfterToast", gs[L"ShowToastNotificationAfterTaskCompleted"].b);
            if (gs[L"ToastWindowDuration"].t == J::Num) w.Num(L"main", L"ToastSeconds", gs[L"ToastWindowDuration"].n);
            if (gs[L"ToastWindowFadeDuration"].t == J::Num) w.Num(L"main", L"ToastFadeSeconds", gs[L"ToastWindowFadeDuration"].n);
            if (gs[L"PlaySoundAfterCapture"].t == J::Bool) w.Bool(L"main", L"SoundCapture", gs[L"PlaySoundAfterCapture"].b);
            if (gs[L"PlaySoundAfterUpload"].t == J::Bool) w.Bool(L"main", L"SoundDone", gs[L"PlaySoundAfterUpload"].b);
            if (gs[L"PlaySoundAfterAction"].t == J::Bool) w.Bool(L"main", L"SoundAction", gs[L"PlaySoundAfterAction"].b);
            note(done, Tr(L"Уведомления и звуки", L"Notifications and sounds"));
        }

        // ---- image files
        const J& is = ts[L"ImageSettings"];
        if (is.has()) {
            std::wstring fmt = is[L"ImageFormat"].s;
            if (fmt == L"JPEG") w.Int(L"main", L"JpegOverKB", 1);  // any PNG over 1 KB becomes JPEG: always JPEG
            else if (is[L"ImageAutoUseJPEG"].t == J::Bool)
                w.Int(L"main", L"JpegOverKB", is[L"ImageAutoUseJPEG"].b ? (long long)is[L"ImageAutoUseJPEGSize"].n : 0);
            if (is[L"ImageJPEGQuality"].t == J::Num) w.Int(L"main", L"JpegQuality", (long long)is[L"ImageJPEGQuality"].n);
            if (!fmt.empty() && fmt != L"PNG" && fmt != L"JPEG") note(skipped, std::wstring(Tr(L"формат ", L"image format ")) + fmt);
            note(done, Tr(L"Формат изображений (PNG/JPEG)", L"Image format (PNG/JPEG)"));
        }

        // ---- capture
        const J& cs = ts[L"CaptureSettings"];
        if (cs.has()) {
            if (cs[L"ShowCursor"].t == J::Bool) w.Bool(L"main", L"ShowCursor", cs[L"ShowCursor"].b);
            if (cs[L"ScreenRecordFPS"].t == J::Num) w.Int(L"record", L"VideoFps", (long long)cs[L"ScreenRecordFPS"].n);
            if (cs[L"GIFFPS"].t == J::Num) w.Int(L"record", L"GifFps", (long long)cs[L"GIFFPS"].n);
            if (cs[L"ScreenRecordShowCursor"].t == J::Bool) w.Bool(L"record", L"ShowCursor", cs[L"ScreenRecordShowCursor"].b);
            const J& ff = cs[L"FFmpegOptions"];
            if (ff.has()) {
                if (ff[L"x264_CRF"].t == J::Num) w.Int(L"record", L"X264Crf", (long long)ff[L"x264_CRF"].n);
                if (ff[L"x264_Preset"].t == J::Str) w.Str(L"record", L"X264Preset", ff[L"x264_Preset"].s);
                if (ff[L"GIFDither"].t == J::Str) w.Str(L"record", L"GifDither", ff[L"GIFDither"].s);
                if (ff[L"GIFStatsMode"].t == J::Str) w.Str(L"record", L"GifStatsMode", ff[L"GIFStatsMode"].s);
                if (ff[L"GIFBayerScale"].t == J::Num) w.Int(L"record", L"GifBayerScale", (long long)ff[L"GIFBayerScale"].n);
                if (ff[L"OverrideCLIPath"].b && !ff[L"CLIPath"].s.empty() && FileExists(Expand(ff[L"CLIPath"].s)))
                    w.Str(L"main", L"FFmpeg", Expand(ff[L"CLIPath"].s));
                // Audio: a dshow device name ("" = none); AAC/Opus/MP3 map to ours, Vorbis (not in MP4) to AAC.
                if (ff[L"AudioSource"].t == J::Str) w.Str(L"record", L"AudioSource", ff[L"AudioSource"].s);
                if (ff[L"AudioCodec"].t == J::Str) {
                    std::wstring ac = ff[L"AudioCodec"].s;
                    const wchar_t* ours = ac == L"libopus" ? L"opus" : ac == L"libmp3lame" ? L"mp3" : L"aac";
                    w.Str(L"record", L"AudioCodec", ours);
                    const J& br = ac == L"libopus" ? ff[L"Opus_Bitrate"] : ff[L"AAC_Bitrate"];
                    if (br.t == J::Num) w.Int(L"record", L"AudioBitrate", (long long)br.n);
                    if (ac == L"libvorbis") note(skipped, std::wstring(L"AudioCodec = ") + ac + L" (AAC)");
                }
            }
            if (cs[L"ScreenRecordStartDelay"].t == J::Num) w.Num(L"record", L"StartDelay", cs[L"ScreenRecordStartDelay"].n);
            if (cs[L"ScreenRecordAutoStart"].t == J::Bool) w.Bool(L"record", L"AutoStart", cs[L"ScreenRecordAutoStart"].b);
            note(done, Tr(L"Захват и запись экрана (курсор, FPS, x264, GIF)", L"Capture and screen recording (cursor, FPS, x264, GIF)"));

            // ---- region capture surface + annotation defaults
            const J& so = cs[L"SurfaceOptions"];
            if (so.has()) {
                auto flag = [&](const wchar_t* k) {
                    if (so[k].t == J::Bool) w.Bool(L"region", k, so[k].b);
                };
                auto num = [&](const J& v, const wchar_t* sec, const wchar_t* k) {
                    if (v.t == J::Num) w.Int(sec, k, (long long)std::lround(v.n));
                };
                flag(L"ShowInfo"); flag(L"ShowMagnifier"); flag(L"UseSquareMagnifier"); flag(L"UseDimming"); flag(L"DetectWindows");
                flag(L"QuickCrop"); flag(L"ShowCrosshair"); flag(L"EnableAnimations"); flag(L"IsFixedSize"); flag(L"ShowFPS");
                flag(L"MenuLocked"); flag(L"RememberMenuState"); flag(L"SwitchToDrawingToolAfterSelection");
                flag(L"SwitchToSelectionToolAfterDrawing"); flag(L"UseLightResizeNodes");
                if (so[L"FixedSize"].t == J::Str) {  // System.Drawing.Size: "250, 250"
                    auto p = Split(so[L"FixedSize"].s, L',');
                    if (p.size() == 2 && IsInt(p[0]) && IsInt(p[1])) w.Str(L"region", L"FixedSize", p[0] + L"," + p[1]);
                }
                for (const wchar_t* k : {L"RegionCaptureActionX1Click", L"RegionCaptureActionX2Click"}) {
                    if (so[k].t != J::Str) continue;
                    static const wchar_t* known[] = {L"None", L"CancelCapture", L"RemoveShapeCancelCapture", L"RemoveShape",
                                                     L"SwapToolType", L"CaptureFullscreen", L"CaptureActiveMonitor", L"CaptureLastRegion"};
                    bool ok = false;
                    for (auto a : known) ok |= so[k].s == a;
                    if (ok) w.Str(L"region", k, so[k].s);
                    else note(skipped, std::wstring(k) + L" = " + so[k].s);
                }
                num(so[L"MagnifierPixelCount"], L"region", L"MagnifierPixelCount");
                num(so[L"MagnifierPixelSize"], L"region", L"MagnifierPixelSize");
                num(so[L"BackgroundDimStrength"], L"region", L"BackgroundDimStrength");

                const J& an = so[L"AnnotationOptions"];
                auto color = [&](const J& v, const wchar_t* k) {
                    if (v.t != J::Str) return;
                    uint32_t c;
                    if (!ParseColor(v.s, c)) { note(skipped, std::wstring(k) + L" = " + v.s); return; }
                    wchar_t b[16];
                    swprintf_s(b, L"%08X", c);
                    w.Str(L"annotation", k, b);
                };
                if (an.has()) {
                    color(an[L"BorderColor"], L"BorderColor");
                    color(an[L"FillColor"], L"FillColor");
                    num(an[L"BorderSize"], L"annotation", L"BorderSize");
                    color(an[L"TextOutlineBorderColor"], L"TextOutlineBorderColor");
                    color(an[L"TextOutlineOptions"][L"Color"], L"TextOutlineColor");
                    num(an[L"TextOutlineOptions"][L"Size"], L"annotation", L"TextOutlineSize");
                    num(an[L"TextOutlineBorderSize"], L"annotation", L"TextOutlineBorderSize");
                    color(an[L"TextOptions"][L"Color"], L"TextColor");
                    color(an[L"TextFillColor"], L"TextFillColor");
                    num(an[L"TextOptions"][L"Size"], L"annotation", L"TextSize");
                    color(an[L"StepFillColor"], L"StepFillColor");
                    num(an[L"StepFontSize"], L"annotation", L"StepFontSize");
                    num(an[L"MagnifyStrength"], L"annotation", L"MagnifyStrength");
                    num(an[L"BlurRadius"], L"annotation", L"BlurRadius");
                    num(an[L"PixelateSize"], L"annotation", L"PixelateSize");
                    color(an[L"HighlightColor"], L"HighlightColor");
                    // Shape/tool options (editor.cpp Opts): enums are stored as their index.
                    auto enm = [&](const J& v, const wchar_t* k, std::initializer_list<const wchar_t*> names) {
                        if (v.t != J::Str) return;
                        int i = 0;
                        for (const wchar_t* n : names) {
                            if (v.s == n) { w.Int(L"annotation", k, i); return; }
                            ++i;
                        }
                    };
                    auto flagA = [&](const J& v, const wchar_t* k) {
                        if (v.t == J::Bool) w.Int(L"annotation", k, v.b ? 1 : 0);
                    };
                    num(an[L"RegionCornerRadius"], L"annotation", L"RegionCornerRadius");
                    enm(an[L"BorderStyle"], L"BorderStyle", {L"Solid", L"Dash", L"Dot", L"DashDot", L"DashDotDot"});
                    num(an[L"DrawingCornerRadius"], L"annotation", L"DrawingCornerRadius");
                    flagA(an[L"Shadow"], L"Shadow");
                    color(an[L"ShadowColor"], L"ShadowColor");
                    num(an[L"LineCenterPointCount"], L"annotation", L"LineCenterPointCount");
                    enm(an[L"ArrowHeadDirection"], L"ArrowHeadDirection", {L"End", L"Start", L"Both"});
                    for (auto [json, pre] : {std::pair{L"TextOutlineOptions", L"TextOutline"}, std::pair{L"TextOptions", L"Text"}}) {
                        const J& to = an[json];
                        std::wstring p = pre;
                        if (to[L"Font"].t == J::Str && !to[L"Font"].s.empty()) w.Str(L"annotation", (p + L"Font").c_str(), to[L"Font"].s);
                        flagA(to[L"Bold"], (p + L"Bold").c_str());
                        flagA(to[L"Italic"], (p + L"Italic").c_str());
                        flagA(to[L"Underline"], (p + L"Underline").c_str());
                        flagA(to[L"EnterKeyNewLine"], (p + L"EnterKeyNewLine").c_str());
                        enm(to[L"AlignmentHorizontal"], (p + L"AlignmentHorizontal").c_str(), {L"Near", L"Center", L"Far"});
                        enm(to[L"AlignmentVertical"], (p + L"AlignmentVertical").c_str(), {L"Near", L"Center", L"Far"});
                    }
                    enm(an[L"StepType"], L"StepType",
                        {L"Numbers", L"LettersUppercase", L"LettersLowercase", L"RomanNumeralsUppercase", L"RomanNumeralsLowercase"});
                    num(an[L"StickerSize"], L"annotation", L"StickerSize");
                    enm(an[L"CutOutEffectType"], L"CutOutEffectType", {L"None", L"ZigZag", L"TornEdge", L"Wave"});
                    num(an[L"CutOutEffectSize"], L"annotation", L"CutOutEffectSize");
                    color(an[L"CutOutBackgroundColor"], L"CutOutBackgroundColor");
                }
                auto tool = [&](const wchar_t* k) {
                    if (so[k].t != J::Str) return;
                    int i = ToolIndex(so[k].s);
                    if (i >= 0) w.Int(L"annotation", k, i);
                    else note(skipped, std::wstring(k) + L" = " + so[k].s);
                };
                tool(L"LastRegionTool"); tool(L"LastAnnotationTool"); tool(L"LastEditorTool");
                note(done, Tr(L"Захват области и аннотации (цвета, размеры, инструменты)",
                              L"Region capture and annotation (colours, sizes, tools)"));
            }
        }

        // ---- file names and folder
        const J& us = ts[L"UploadSettings"];
        if (us[L"NameFormatPattern"].t == J::Str) w.Str(L"main", L"NamePattern", us[L"NameFormatPattern"].s);
        if (us[L"NameFormatPatternActiveWindow"].t == J::Str) w.Str(L"main", L"NamePatternWindow", us[L"NameFormatPatternActiveWindow"].s);
        if (app[L"SaveImageSubFolderPattern"].t == J::Str) w.Str(L"main", L"SubFolderPattern", app[L"SaveImageSubFolderPattern"].s);
        std::wstring save;
        if (app[L"UseCustomScreenshotsPath"].b && !Trim(app[L"CustomScreenshotsPath"].s).empty()) {
            save = Expand(Trim(app[L"CustomScreenshotsPath"].s));
            if (PathIsRelativeW(save.c_str())) save = dir + L"\\" + save;
        } else {
            save = dir + L"\\Screenshots";
        }
        w.Str(L"main", L"SaveDir", save);
        note(done, std::wstring(Tr(L"Имена файлов и папка: ", L"File names and folder: ")) + save);

        // Per-hotkey task overrides are not modelled: Zipshot has one set of after-capture tasks.
        for (const J& e : hk[L"Hotkeys"].a)
            if (e[L"TaskSettings"][L"UseDefaultAfterCaptureJob"].t == J::Bool && !e[L"TaskSettings"][L"UseDefaultAfterCaptureJob"].b)
                note(skipped, std::wstring(Tr(L"свои задачи хоткея ", L"per-hotkey tasks of ")) + e[L"TaskSettings"][L"Job"].s);
    }

    report += std::wstring(Tr(L"Импортировано:\n", L"Imported:\n")) + done;
    if (!skipped.empty()) report += std::wstring(Tr(L"\nНе поддерживается:\n", L"\nNot supported:\n")) + skipped;
    Log(L"sharex import from %s: %d keys", dir.c_str(), w.written);
    return true;
}
