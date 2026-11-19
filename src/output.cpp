#include "common.h"
#include "png_limit_stream.h"
#include "sharex_import.h"
#include "encoded_png.h"
#include "png_encode_fast.h"
#include "pixel_alpha.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <psapi.h>
#include <mmsystem.h>
#include <wrl/client.h>
#include <random>
#include <algorithm>
#include <ctime>

using Microsoft::WRL::ComPtr;

static std::wstring g_saveDir;
static int g_jpegOverKB = 2048;
static int g_jpegQuality = 90;
static bool g_sound = true, g_soundCapture = true, g_soundDone = true, g_soundAction = true;
static std::wstring g_namePattern, g_namePatternWindow, g_subFolderPattern;
static std::wstring g_ffmpeg;

static std::wstring IniPath() { return ExeDir() + L"\\zipshot.ini"; }

static std::wstring IniStr(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[MAX_PATH * 2];
    GetPrivateProfileStringW(L"main", key, def, buf, (DWORD)std::size(buf), IniPath().c_str());
    return buf;
}

void LoadSettings() {
    std::wstring ini = IniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        EnsureUnicodeIni();
        // Use this user's Pictures folder; importing ShareX settings can override it later.
        WritePrivateProfileStringW(L"main", L"SaveDir", L"", ini.c_str());
        WritePrivateProfileStringW(L"main", L"JpegOverKB", L"2048", ini.c_str());
        WritePrivateProfileStringW(L"main", L"Sound", L"1", ini.c_str());
        WritePrivateProfileStringW(L"main", L"FFmpeg", L"", ini.c_str());
    }
    g_saveDir = IniStr(L"SaveDir", L"");
    if (g_saveDir.empty()) {
        wchar_t* pics = nullptr;
        SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics);
        g_saveDir = std::wstring(pics ? pics : L".") + L"\\Zipshot";
        CoTaskMemFree(pics);
    }
    g_jpegOverKB = _wtoi(IniStr(L"JpegOverKB", L"2048").c_str());
    g_jpegQuality = std::clamp(_wtoi(IniStr(L"JpegQuality", L"90").c_str()), 1, 100);
    g_sound = IniStr(L"Sound", L"1") != L"0";
    g_soundCapture = IniStr(L"SoundCapture", L"1") != L"0";
    g_soundDone = IniStr(L"SoundDone", L"1") != L"0";
    g_soundAction = IniStr(L"SoundAction", L"1") != L"0";
    // ShareX name patterns (UploadSettings.NameFormatPattern*, SaveImageSubFolderPattern).
    g_namePattern = IniString(L"main", L"NamePattern", L"%pn_%ra{10}");
    g_namePatternWindow = IniString(L"main", L"NamePatternWindow", L"%pn_%ra{10}");
    g_subFolderPattern = IniString(L"main", L"SubFolderPattern", L"%y-%mo");
    g_ffmpeg = IniStr(L"FFmpeg", L"");
}

bool GetSettingBool(const wchar_t* key, bool def) { return IniStr(key, def ? L"1" : L"0") != L"0"; }

void SetSettingBool(const wchar_t* key, bool value) {
    WritePrivateProfileStringW(L"main", key, value ? L"1" : L"0", IniPath().c_str());
}

std::wstring SaveDirRoot() { return g_saveDir; }

// Process name and title of the foreground window (empty when it is Zipshot itself or unknown).
static void ForegroundInfo(std::wstring& process, std::wstring& title) {
    process.clear();
    title.clear();
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (!pid || pid == GetCurrentProcessId()) return;
    wchar_t t[256];
    if (GetWindowTextW(fg, t, 256) > 0) title = t;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
        PathRemoveExtensionW(buf);
        process = PathFindFileNameW(buf);
    }
    CloseHandle(p);
}

// ShareX NameParser tokens: %pn %t %y %yy %mo %mon %mon2 %d %h %mi %s %ms %pm %w %unix %guid %cn %un
// %ra{n} (random alphanumeric) %rn{n} (random digits) %rx{n} (random hex). Unknown tokens stay as typed.
static std::wstring ExpandPattern(const std::wstring& pat, const std::wstring& process, const std::wstring& title) {
    static std::mt19937 rng{std::random_device{}()};
    static const wchar_t* months[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August",
                                      L"September", L"October", L"November", L"December"};
    static const wchar_t* days[] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
    SYSTEMTIME st;
    GetLocalTime(&st);
    const bool h12 = pat.find(L"%pm") != std::wstring::npos;
    auto num = [](int v, int width) {
        wchar_t b[16];
        swprintf_s(b, L"%0*d", width, v);
        return std::wstring(b);
    };
    std::wstring out;
    for (size_t i = 0; i < pat.size();) {
        if (pat[i] != L'%') { out += pat[i++]; continue; }
        auto at = [&](const wchar_t* tok) { return pat.compare(i, wcslen(tok), tok) == 0; };
        if (at(L"%ra{") || at(L"%rn{") || at(L"%rx{")) {
            size_t close = pat.find(L'}', i + 4);
            if (close != std::wstring::npos) {
                int n = std::clamp(_wtoi(pat.substr(i + 4, close - i - 4).c_str()), 1, 100);
                const wchar_t* al = pat[i + 2] == L'a' ? L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
                                    : pat[i + 2] == L'n' ? L"0123456789" : L"0123456789abcdef";
                size_t len = wcslen(al);
                for (int k = 0; k < n; ++k) out += al[rng() % len];
                i = close + 1;
                continue;
            }
        }
        std::wstring v;
        const wchar_t* tok = nullptr;
        if (at(tok = L"%unix")) v = std::to_wstring((long long)time(nullptr));
        else if (at(tok = L"%guid")) {
            GUID g{};
            CoCreateGuid(&g);
            wchar_t b[64];
            swprintf_s(b, L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                       g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
            v = b;
        } else if (at(tok = L"%mon2")) v = std::wstring(months[st.wMonth - 1]).substr(0, 3);
        else if (at(tok = L"%mon")) v = months[st.wMonth - 1];
        else if (at(tok = L"%mo")) v = num(st.wMonth, 2);
        else if (at(tok = L"%ms")) v = num(st.wMilliseconds, 3);
        else if (at(tok = L"%mi")) v = num(st.wMinute, 2);
        else if (at(tok = L"%yy")) v = num(st.wYear % 100, 2);
        else if (at(tok = L"%y")) v = num(st.wYear, 4);
        else if (at(tok = L"%pn")) v = process.empty() ? L"screen" : process;
        else if (at(tok = L"%pm")) v = st.wHour >= 12 ? L"PM" : L"AM";
        else if (at(tok = L"%cn")) {
            wchar_t b[MAX_COMPUTERNAME_LENGTH + 1];
            DWORD n = (DWORD)std::size(b);
            if (GetComputerNameW(b, &n)) v = b;
        } else if (at(tok = L"%un")) {
            wchar_t b[257];
            DWORD n = 257;
            if (GetUserNameW(b, &n)) v = b;
        } else if (at(tok = L"%h")) v = num(h12 ? (st.wHour % 12 == 0 ? 12 : st.wHour % 12) : st.wHour, 2);
        else if (at(tok = L"%d")) v = num(st.wDay, 2);
        else if (at(tok = L"%s")) v = num(st.wSecond, 2);
        else if (at(tok = L"%t")) v = title;
        else if (at(tok = L"%w")) v = days[st.wDayOfWeek];
        else tok = nullptr;
        if (tok) {
            out += v;
            i += wcslen(tok);
        } else {
            out += pat[i++];
        }
    }
    return out;
}

// Replaces characters Windows forbids in names; `allowDirs` keeps \ and / as folder separators.
static std::wstring CleanName(std::wstring s, bool allowDirs) {
    for (auto& c : s) {
        if (c < 32 || wcschr(L":*?\"<>|", c) || (!allowDirs && (c == L'\\' || c == L'/'))) c = L'_';
        else if (c == L'/') c = L'\\';
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.' || s.back() == L'\\')) s.pop_back();
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\\')) s.erase(0, 1);
    return s;
}

// <SaveDir>\<SubFolderPattern>\<NamePattern(Window)>.<ext>, defaults reproduce <SaveDir>\yyyy-MM\<proc>_<rand10>.
std::wstring NewOutputPath(const wchar_t* ext) {
    std::wstring process, title;
    ForegroundInfo(process, title);
    std::wstring dir = g_saveDir;
    std::wstring sub = CleanName(ExpandPattern(g_subFolderPattern, process, title), true);
    if (!sub.empty() && sub.find(L"..") == std::wstring::npos) dir += L"\\" + sub;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    // ShareX uses the active-window pattern whenever the capture knows a window or process name.
    bool windowKnown = !process.empty() || !title.empty();
    const std::wstring& pat = windowKnown && !g_namePatternWindow.empty() ? g_namePatternWindow : g_namePattern;
    std::wstring name = CleanName(ExpandPattern(pat, process, title), false);
    if (name.size() > 180) name.resize(180);
    if (name.empty()) name = CleanName(ExpandPattern(L"%pn_%ra{10}", process, title), false);
    std::wstring path = dir + L"\\" + name + L"." + ext;
    for (int n = 1; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 1000; ++n)
        path = dir + L"\\" + name + L" (" + std::to_wstring(n) + L")." + ext;
    return path;
}

static bool ValidImage(const Image& img) {
    return img.w > 0 && img.h > 0 && (uint64_t)img.w * img.h == img.px.size() &&
           img.px.size() <= UINT_MAX / 4;
}

static bool Encode(const Image& img, IStream* stream, bool jpeg) {
    if (!stream || !ValidImage(img)) return false;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICBitmap> bmp;
    bool alpha = img.alpha && !jpeg;
    if (FAILED(f->CreateBitmapFromMemory(img.w, img.h, alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppBGR, img.w * 4,
                                         (UINT)(img.px.size() * 4), (BYTE*)img.px.data(), &bmp)))
        return false;
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc))) return false;
    if (FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;
    if (jpeg) {
        PROPBAG2 opt{};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = g_jpegQuality / 100.0f;
        props->Write(1, &opt, &v);
    }
    if (FAILED(frame->Initialize(props.Get())) || FAILED(frame->SetSize(img.w, img.h))) return false;
    WICPixelFormatGUID fmt = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    ComPtr<IWICBitmapSource> src = bmp;
    if (fmt != (alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppBGR)) {
        ComPtr<IWICBitmapSource> conv;
        if (SUCCEEDED(WICConvertBitmapSource(fmt, bmp.Get(), &conv))) src = conv;
    }
    if (FAILED(frame->WriteSource(src.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(enc->Commit());
}

// Capture workers can race with one another after NewOutputPath. Claim the final extension atomically,
// including when the PNG size limit changes it to JPEG, so no existing capture is overwritten.
static bool WriteFileBytes(std::wstring& path, const void* data, size_t n) {
    if (n > MAXDWORD) return false;
    const std::wstring original = path;
    size_t dot = original.find_last_of(L'.');
    size_t slash = original.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) dot = original.size();
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 10000; ++i) {
        std::wstring candidate = i == 0 ? original : original.substr(0, dot) + L" (" + std::to_wstring(i) + L")" + original.substr(dot);
        h = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { path = std::move(candidate); break; }
        DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return false;
    }
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &wr, nullptr);
    CloseHandle(h);
    if (!ok || wr != n) DeleteFileW(path.c_str());
    return ok && wr == n;
}

bool EncodeImage(const Image& img, const std::wstring& path, bool jpeg) {
    if (!ValidImage(img)) return false;
    ComPtr<IStream> s;
    if (FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &s)))
        return false;
    if (!jpeg) {
        std::vector<uint8_t> png;
        if (TryEncodePngFast(img, png)) {
            ULONG written = 0;
            return SUCCEEDED(s->Write(png.data(), (ULONG)png.size(), &written)) && written == png.size();
        }
    }
    return Encode(img, s.Get(), jpeg);
}

bool SaveImageAuto(const Image& img, std::wstring& path) {
    if (img.alpha || g_jpegOverKB <= 0) {
        std::vector<uint8_t> png;
        if (TryEncodePngFast(img, png)) return WriteFileBytes(path, png.data(), png.size());
    }
    ComPtr<IStream> mem;
    mem.Attach(SHCreateMemStream(nullptr, 0));
    if (!mem) return false;
    ComPtr<PngLimitStream> limited;
    if (!img.alpha && g_jpegOverKB > 0)
        limited.Attach(new PngLimitStream(mem.Get(), (uint64_t)g_jpegOverKB * 1024));
    const bool pngOk = Encode(img, limited ? static_cast<IStream*>(limited.Get()) : mem.Get(), false);
    const bool thresholdExceeded = limited && limited->Exceeded();
    if (!pngOk && !thresholdExceeded) return false;
    STATSTG st{};
    if (FAILED(mem->Stat(&st, STATFLAG_NONAME))) return false;
    size_t size = (size_t)st.cbSize.QuadPart;
    if (thresholdExceeded || (!img.alpha && g_jpegOverKB > 0 && size > (size_t)g_jpegOverKB * 1024)) {
        path = path.substr(0, path.find_last_of(L'.')) + L".jpg";
        mem.Attach(SHCreateMemStream(nullptr, 0));
        if (!mem || !Encode(img, mem.Get(), true) || FAILED(mem->Stat(&st, STATFLAG_NONAME))) return false;
        size = (size_t)st.cbSize.QuadPart;
    }
    if (size > MAXDWORD) return false;
    std::vector<BYTE> buf(size);
    LARGE_INTEGER zero{};
    if (FAILED(mem->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    ULONG rd = 0;
    if (FAILED(mem->Read(buf.data(), (ULONG)size, &rd)) || rd != size) return false;
    return WriteFileBytes(path, buf.data(), size);
}

static bool StreamBytes(IStream* stream, std::vector<BYTE>& bytes) {
    STATSTG stat{};
    if (!stream || FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || !stat.cbSize.QuadPart || stat.cbSize.QuadPart > MAXDWORD)
        return false;
    bytes.resize((size_t)stat.cbSize.QuadPart);
    LARGE_INTEGER zero{};
    ULONG read = 0;
    return SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) &&
           SUCCEEDED(stream->Read(bytes.data(), (ULONG)bytes.size(), &read)) && read == bytes.size();
}

static bool PngBytes(const Image& image, std::vector<BYTE>& bytes) {
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    return stream && Encode(image, stream.Get(), false) && StreamBytes(stream.Get(), bytes);
}

static bool SelectedPngBytes(const Image& image, std::vector<BYTE>& bytes) {
    if (image.alpha && TryEncodePngFast(image, bytes)) return true;
    return PngBytes(image, bytes);
}

class PreparedPng {
    friend PreparedPngPtr PreparePngWic(std::shared_ptr<const Image>);
    friend PreparedPngPtr PreparePng(std::shared_ptr<const Image>);
    friend void CopyPreparedImageToClipboard(const PreparedPngPtr&, std::function<bool()>);
    friend bool SavePreparedImageAuto(const PreparedPngPtr&, std::wstring&);
    std::shared_ptr<const Image> image;
    std::vector<BYTE> png;
    int width = 0, height = 0;
    bool alpha = false;
    bool Valid() const {
        return image && ValidImage(*image) && image->w == width && image->h == height && image->alpha == alpha &&
               !png.empty() && png.size() <= MAXDWORD;
    }
};

PreparedPngPtr PreparePngWic(std::shared_ptr<const Image> image) {
    if (!image || !ValidImage(*image)) return {};
    auto prepared = std::shared_ptr<PreparedPng>(new PreparedPng);
    prepared->image = std::move(image);
    prepared->width = prepared->image->w;
    prepared->height = prepared->image->h;
    prepared->alpha = prepared->image->alpha;
    if (!PngBytes(*prepared->image, prepared->png)) return {};
    return prepared;
}

PreparedPngPtr PreparePng(std::shared_ptr<const Image> image) {
    if (!image || !ValidImage(*image)) return {};
    auto prepared = std::shared_ptr<PreparedPng>(new PreparedPng);
    prepared->image = std::move(image);
    prepared->width = prepared->image->w;
    prepared->height = prepared->image->h;
    prepared->alpha = prepared->image->alpha;
    if (!SelectedPngBytes(*prepared->image, prepared->png)) return {};
    return prepared;
}

bool SavePreparedImageAuto(const PreparedPngPtr& prepared, std::wstring& path) {
    if (!prepared || !prepared->Valid()) return false;
    const Image& image = *prepared->image;
    // WIC PNG size is unchanged, including the automatic-JPEG decision.
    if (!image.alpha && g_jpegOverKB > 0 && prepared->png.size() > (size_t)g_jpegOverKB * 1024) {
        path = path.substr(0, path.find_last_of(L'.')) + L".jpg";
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(nullptr, 0));
        std::vector<BYTE> jpeg;
        if (!stream || !Encode(image, stream.Get(), true) || !StreamBytes(stream.Get(), jpeg)) return false;
        return WriteFileBytes(path, jpeg.data(), jpeg.size());
    }
    return WriteFileBytes(path, prepared->png.data(), prepared->png.size());
}

bool LoadImageFile(const std::wstring& path, Image& out) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    WICPixelFormatGUID native{};
    frame->GetPixelFormat(&native);
    ComPtr<IWICBitmapSource> source;
    if (native == GUID_WICPixelFormat32bppBGRA) source = frame;
    else {
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(f->CreateFormatConverter(&conv))) return false;
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom)))
            return false;
        source = conv;
    }
    UINT w, h;
    if (FAILED(source->GetSize(&w, &h)) || !w || !h || (uint64_t)w * h > UINT_MAX / 4) return false;
    Image loaded;
    loaded.w = (int)w;
    loaded.h = (int)h;
    loaded.px.resize((size_t)w * h);
    if (FAILED(source->CopyPixels(nullptr, w * 4, (UINT)(loaded.px.size() * 4), (BYTE*)loaded.px.data()))) return false;
    // Image documents use straight BGRA; the editor premultiplies only when uploading to D2D.
    // Flattening here destroyed transparency when a saved PNG was reopened.
    // These native formats cannot contain alpha. Gray/indexed/other formats
    // still inspect decoded pixels, including palette and color-key cases.
    const bool opaque = native == GUID_WICPixelFormat24bppBGR || native == GUID_WICPixelFormat24bppRGB ||
                        native == GUID_WICPixelFormat32bppBGR;
    loaded.alpha = !opaque && PixelsHaveTransparency(loaded.px.data(), loaded.px.size());
    out = std::move(loaded);
    return true;
}

static bool OpenClipboardRetry() {
    for (int i = 0; i < 20; ++i) {
        if (OpenClipboard(g_main)) return true;
        Sleep(15);
    }
    return false;
}

static void CopyImageToClipboardBytes(const Image& img, const std::vector<BYTE>* png,
                                      const std::function<bool()>& allow = {}) {
    if (!ValidImage(img) || !OpenClipboardRetry()) return;
    struct CloseOnExit { ~CloseOnExit() { CloseClipboard(); } } close;
    if (allow) {
        try { if (!allow()) return; }
        catch (...) { return; }
    }
    EmptyClipboard();
    size_t rowBytes = (size_t)img.w * 4;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + rowBytes * img.h);
    if (g) {
        auto* bih = (BITMAPINFOHEADER*)GlobalLock(g);
        if (!bih) { GlobalFree(g); g = nullptr; }
        if (bih) {
        *bih = {};
        bih->biSize = sizeof(*bih);
        bih->biWidth = img.w;
        bih->biHeight = img.h; // bottom-up: the most compatible layout
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        bih->biCompression = BI_RGB;
        bih->biSizeImage = (DWORD)(rowBytes * img.h);
        BYTE* dst = (BYTE*)(bih + 1);
        for (int y = 0; y < img.h; ++y)
            memcpy(dst + rowBytes * (img.h - 1 - y), &img.px[(size_t)y * img.w], rowBytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_DIB, g)) GlobalFree(g);
        }
    }
    if (img.alpha && png && !png->empty() && png->size() <= MAXDWORD) {
        // DIBs lose transparency in most apps; browsers, Telegram and Discord read the "PNG" format.
            HGLOBAL pg = GlobalAlloc(GMEM_MOVEABLE, png->size());
            if (pg) {
                void* data = GlobalLock(pg);
                if (data) memcpy(data, png->data(), png->size());
                GlobalUnlock(pg);
                if (!data || !SetClipboardData(RegisterClipboardFormatW(L"PNG"), pg)) GlobalFree(pg);
            }
    }
}

void CopyImageToClipboard(const Image& img) {
    if (!ValidImage(img)) return;
    std::vector<BYTE> png;
    if (img.alpha) SelectedPngBytes(img, png);  // All encoding occurs before opening the clipboard.
    CopyImageToClipboardBytes(img, png.empty() ? nullptr : &png);
}

void CopyPreparedImageToClipboard(const PreparedPngPtr& prepared, std::function<bool()> allow) {
    if (!prepared || !prepared->Valid()) return;
    CopyImageToClipboardBytes(*prepared->image, &prepared->png, allow);
}

void CopyFileToClipboard(const std::wstring& path) {
    if (!OpenClipboardRetry()) return;
    EmptyClipboard();
    size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (g) {
        auto* df = (DROPFILES*)GlobalLock(g);
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        memcpy(df + 1, path.c_str(), path.size() * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_HDROP, g)) GlobalFree(g);
    }
    CloseClipboard();
}

void PlayUiSound(Sound s) {
    if (!g_sound) return;
    if ((s == S_Capture && !g_soundCapture) || (s == S_TaskDone && !g_soundDone) || (s == S_Action && !g_soundAction)) return;
    static const wchar_t* names[] = {L"CAPTURE", L"TASKDONE", L"ERRORSND", L"ACTIONDONE"};
    PlaySoundW(names[s], g_inst, SND_RESOURCE | SND_ASYNC | SND_NODEFAULT);
}

std::wstring FindFFmpeg() {
    if (!g_ffmpeg.empty() && GetFileAttributesW(g_ffmpeg.c_str()) != INVALID_FILE_ATTRIBUTES) return g_ffmpeg;
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr)) return g_ffmpeg = buf;
    wchar_t local[MAX_PATH];
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\WinGet\\Links\\ffmpeg.exe", local, MAX_PATH);
    if (GetFileAttributesW(local) != INVALID_FILE_ATTRIBUTES) return g_ffmpeg = local;
    std::wstring exeLocal = ExeDir() + L"\\ffmpeg.exe";
    if (GetFileAttributesW(exeLocal.c_str()) != INVALID_FILE_ATTRIBUTES) return g_ffmpeg = exeLocal;
    return L"";
}
