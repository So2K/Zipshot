// Isolated image-codec benchmark. Includes current production output.cpp.
// Synthetic pixels only; no desktop, input, clipboard, foreground or settings.
#include "../../src/common.h"
#include "../../src/sharex_import.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HICON g_icon = nullptr;
std::wstring ExeDir() { return L"."; }
void EnsureUnicodeIni() {}
std::wstring IniString(const wchar_t*, const wchar_t*, const wchar_t* fallback) { return fallback; }
void Log(const wchar_t*, ...) {}
namespace clipboard_probe {
std::vector<std::pair<UINT, HGLOBAL>> handles;
LARGE_INTEGER opened{}, closed{};
int opens = 0, empties = 0, closes = 0;
bool isOpen = false;
BOOL WINAPI Open(HWND) { ++opens; isOpen = true; QueryPerformanceCounter(&opened); return TRUE; }
BOOL WINAPI Empty() { ++empties; return TRUE; }
BOOL WINAPI Close() { ++closes; isOpen = false; QueryPerformanceCounter(&closed); return TRUE; }
HANDLE WINAPI Set(UINT format, HANDLE handle) { handles.push_back({format, (HGLOBAL)handle}); return handle; }
UINT WINAPI Register(LPCWSTR) { return 0xC123; }
void Clear() { for (auto x : handles) GlobalFree(x.second); handles.clear(); }
}  // namespace clipboard_probe
#define OpenClipboard clipboard_probe::Open
#define EmptyClipboard clipboard_probe::Empty
#define CloseClipboard clipboard_probe::Close
#define SetClipboardData clipboard_probe::Set
#define RegisterClipboardFormatW clipboard_probe::Register
#include "../../src/output.cpp"
#undef OpenClipboard
#undef EmptyClipboard
#undef CloseClipboard
#undef SetClipboardData
#undef RegisterClipboardFormatW

namespace bench {
LARGE_INTEGER frequency{};
double Clock() { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return q.QuadPart * 1000.0 / frequency.QuadPart; }
std::string Hash(const void* bytes, size_t count) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    BCryptHashData(hash, (PUCHAR)bytes, static_cast<ULONG>(count), 0);
    unsigned char digest[32]{};
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg, 0);
    const char* digits = "0123456789abcdef";
    std::string result;
    for (auto x : digest) { result += digits[x >> 4]; result += digits[x & 15]; }
    return result;
}
Image Fixture(int w, int h, const std::wstring& kind) {
    Image image{w, h, kind == L"alpha" || kind == L"alpha-ui", std::vector<uint32_t>((size_t)w * h)};
    uint32_t random = 0x97241A3B;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        uint32_t rgb = 0x27282B;
        if (kind == L"entropy") rgb = random & 0xFFFFFF;
        else if (kind == L"photo") {
            const int noise = (random & 31) - 15;
            const auto c = [&](int v) { return (uint32_t)std::clamp(v + noise, 0, 255); };
            rgb = (c(x * 255 / w) << 16) | (c(y * 255 / h) << 8) | c((x + y) * 255 / (w + h));
        } else if (kind == L"alpha") rgb = random & 0xFFFFFF;
        else if ((x / 80 + y / 60) % 7 == 0) rgb = 0x3A8DFF;
        else if (y % 60 < 2 || x % 240 < 2) rgb = 0x3D3E42;
        const uint32_t alpha = image.alpha ? (uint32_t)((x * 255LL) / std::max(1, w - 1)) : 255;
        image.px[(size_t)y * w + x] = (alpha << 24) | rgb;
    }
    if (kind == L"ui" || kind == L"text" || kind == L"alpha-ui") {
        BITMAPINFO info{}; info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        memcpy(bits, image.px.data(), image.px.size() * 4);
        HDC dc = CreateCompatibleDC(nullptr); HGDIOBJ previous = SelectObject(dc, bitmap);
        HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(231, 233, 234));
        const wchar_t* label = L"Zipshot synthetic fixture — Поиск по имени — 0123456789  PNG/JPEG text";
        for (int y = 12; y < h; y += kind == L"text" ? 25 : 75) TextOutW(dc, 16, y, label, (int)wcslen(label));
        GdiFlush(); memcpy(image.px.data(), bits, image.px.size() * 4);
        for (size_t i = 0; i < image.px.size(); ++i) {
            const uint32_t alpha = image.alpha ? (uint32_t)((i % w) * 255ULL / std::max(1, w - 1)) : 255;
            image.px[i] = (image.px[i] & 0xFFFFFF) | (alpha << 24);
        }
        SelectObject(dc, oldFont); SelectObject(dc, previous); DeleteObject(font); DeleteObject(bitmap); DeleteDC(dc);
    }
    return image;
}

bool Direct(const Image& image, IStream* stream, bool jpeg, bool cached) {
    static ComPtr<IWICImagingFactory> shared;
    ComPtr<IWICImagingFactory> factory;
    if (cached) {
        if (!shared && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shared)))) return false;
        factory = shared;
    } else if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, &properties))) return false;
    if (jpeg) {
        PROPBAG2 option{}; option.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT quality{}; quality.vt = VT_R4; quality.fltVal = .9f;
        if (FAILED(properties->Write(1, &option, &quality))) return false;
    }
    if (FAILED(frame->Initialize(properties.Get())) || FAILED(frame->SetSize(image.w, image.h))) return false;
    const bool alpha = image.alpha && !jpeg;
    WICPixelFormatGUID format = alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    std::vector<BYTE> packed;
    BYTE* data = (BYTE*)image.px.data(); UINT stride = image.w * 4;
    if (!alpha) {
        stride = image.w * 3; packed.resize((size_t)stride * image.h);
        for (size_t i = 0; i < image.px.size(); ++i) {
            packed[i * 3] = (BYTE)image.px[i]; packed[i * 3 + 1] = (BYTE)(image.px[i] >> 8); packed[i * 3 + 2] = (BYTE)(image.px[i] >> 16);
        }
        data = packed.data();
    }
    return SUCCEEDED(frame->WritePixels(image.h, stride, stride * image.h, data)) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

std::vector<BYTE> Bytes(IStream* stream) {
    STATSTG stat{}; stream->Stat(&stat, STATFLAG_NONAME);
    std::vector<BYTE> bytes((size_t)stat.cbSize.QuadPart);
    LARGE_INTEGER zero{}; stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0; stream->Read(bytes.data(), (ULONG)bytes.size(), &read);
    return read == bytes.size() ? bytes : std::vector<BYTE>{};
}
void Write(const std::wstring& path, const void* data, size_t bytes) {
    std::ofstream file(std::filesystem::path(path), std::ios::binary);
    file.write((const char*)data, static_cast<std::streamsize>(bytes));
}

bool ReuseAlpha(const Image& image, std::wstring& path) {
    ComPtr<IStream> memory; memory.Attach(SHCreateMemStream(nullptr, 0));
    if (!Encode(image, memory.Get(), false)) return false;
    const auto png = Bytes(memory.Get());  // Immutable per-capture bytes, no global cache.
    if (!clipboard_probe::Open(nullptr)) return false;
    clipboard_probe::Empty();
    const size_t rowBytes = (size_t)image.w * 4;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + image.px.size() * 4);
    auto* header = (BITMAPINFOHEADER*)GlobalLock(dib);
    *header = {};
    header->biSize = sizeof(*header); header->biWidth = image.w; header->biHeight = image.h;
    header->biPlanes = 1; header->biBitCount = 32; header->biCompression = BI_RGB;
    header->biSizeImage = (DWORD)(rowBytes * image.h);
    BYTE* target = (BYTE*)(header + 1);
    for (int y = 0; y < image.h; ++y)
        memcpy(target + rowBytes * (image.h - 1 - y), &image.px[(size_t)y * image.w], rowBytes);
    GlobalUnlock(dib); clipboard_probe::Set(CF_DIB, dib);
    HGLOBAL clipPng = GlobalAlloc(GMEM_MOVEABLE, png.size());
    memcpy(GlobalLock(clipPng), png.data(), png.size()); GlobalUnlock(clipPng);
    clipboard_probe::Set(0xC123, clipPng); clipboard_probe::Close();
    return WriteFileBytes(path, png.data(), png.size());
}
}  // namespace bench

int wmain(int argc, wchar_t** argv) {
    if (argc != 6 && argc != 7) return 2;
    const bool codecsOnly = argc == 7 && std::wstring(argv[6]) == L"codecs";
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetProcessAffinityMask(GetCurrentProcess(), 15);
    QueryPerformanceFrequency(&bench::frequency);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring folder = std::filesystem::absolute(argv[1]).wstring(), kind = argv[2];
    const int width = _wtoi(argv[3]), height = _wtoi(argv[4]), n = _wtoi(argv[5]);
    if (width < 1 || height < 1 || n < 1 || n > 100) return 2;
    std::filesystem::create_directories(folder);
    Image image = bench::Fixture(width, height, kind);
    const std::string sourceHash = bench::Hash(image.px.data(), image.px.size() * 4);
    bench::Write(folder + L"\\fixture.bgra", image.px.data(), image.px.size() * 4);
    std::ofstream csv(std::filesystem::path(folder + L"\\native.csv"));
    csv << "engine,operation,phase,trial,ms,bytes,source_sha256,encoded_sha256,decoded_sha256,alpha\n";
    for (bool jpeg : {false, true}) {
        if (jpeg && image.alpha) continue;
        std::vector<BYTE> baseline;
        for (int engine = 0; engine < (codecsOnly ? 1 : 3); ++engine) {
            for (int trial = -1; trial < n; ++trial) {
                ComPtr<IStream> stream;
                const double start = bench::Clock();
                stream.Attach(SHCreateMemStream(nullptr, 0));
                const bool ok = engine == 0 ? Encode(image, stream.Get(), jpeg) : bench::Direct(image, stream.Get(), jpeg, engine == 2);
                const double elapsed = bench::Clock() - start;
                if (!ok) return 3;
                auto bytes = bench::Bytes(stream.Get());
                std::string encodedHash, decodedHash;
                if (trial == -1) {
                    const std::wstring file = folder + L"\\engine" + std::to_wstring(engine) + (jpeg ? L".jpg" : L".png");
                    bench::Write(file, bytes.data(), bytes.size());
                    Image decoded; if (!LoadImageFile(file, decoded)) return 4;
                    encodedHash = bench::Hash(bytes.data(), bytes.size());
                    decodedHash = bench::Hash(decoded.px.data(), decoded.px.size() * 4);
                    if (engine == 0) baseline = bytes;
                }
                csv << (engine == 0 ? "zipshot-current" : engine == 1 ? "candidate-writepixels" : "candidate-writepixels-factory")
                    << ',' << (jpeg ? "jpeg-encode-memory" : "png-encode-memory") << ',' << (trial == -1 ? "first" : "warm") << ','
                    << trial << ',' << elapsed << ',' << bytes.size() << ',' << sourceHash << ',' << encodedHash << ',' << decodedHash << ',' << image.alpha << '\n';
            }
        }
        const std::wstring input = folder + (jpeg ? L"\\engine0.jpg" : L"\\engine0.png");
        for (int trial = -1; trial < n; ++trial) {
            Image decoded;
            const double start = bench::Clock(); const bool ok = LoadImageFile(input, decoded); const double elapsed = bench::Clock() - start;
            if (!ok) return 4;
            csv << "zipshot-current," << (jpeg ? "jpeg-decode-file" : "png-decode-file") << ',' << (trial == -1 ? "first" : "warm") << ','
                << trial << ',' << elapsed << ',' << baseline.size() << ',' << sourceHash << ",," << bench::Hash(decoded.px.data(), decoded.px.size() * 4) << ',' << decoded.alpha << '\n';
        }
    }
    for (int trial = -1; trial < n; ++trial) {
        g_jpegOverKB = 2048; g_jpegQuality = 90;
        std::wstring path = folder + L"\\save-auto.png";
        const double start = bench::Clock(); const bool ok = SaveImageAuto(image, path); const double elapsed = bench::Clock() - start;
        if (!ok) return 5;
        const auto bytes = std::filesystem::file_size(path);
        csv << "zipshot-current,save-auto-default," << (trial == -1 ? "first" : "warm") << ',' << trial << ',' << elapsed << ',' << bytes << ',' << sourceHash << ",,," << image.alpha << '\n';
        DeleteFileW(path.c_str());
    }
    if (image.alpha && !codecsOnly) {
        std::ofstream clip(std::filesystem::path(folder + L"\\clipboard.csv"));
        clip << "engine,phase,trial,ms,clipboard_lock_ms,bytes,png_sha256,dib_sha256,disk_sha256\n";
        const auto sharedImage = std::make_shared<Image>(image);
        for (int engine = 0; engine < 3; ++engine) for (int trial = -1; trial < n; ++trial) {
            std::wstring path = folder + L"\\clipboard-save.png";
            const double start = bench::Clock();
            bool ok = true;
            if (engine == 0) { CopyImageToClipboard(image); ok = SaveImageAuto(image, path); }
            else if (engine == 1) ok = bench::ReuseAlpha(image, path);
            else {
                auto prepared = PreparePngWic(sharedImage);
                if (!prepared) return 6;
                CopyPreparedImageToClipboard(prepared);
                ok = SavePreparedImageAuto(prepared, path);
            }
            const double elapsed = bench::Clock() - start;
            if (!ok || clipboard_probe::handles.size() != 2) return 6;
            const size_t size = (size_t)std::filesystem::file_size(path);
            std::ifstream file(std::filesystem::path(path), std::ios::binary);
            std::vector<char> disk((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const auto png = clipboard_probe::handles[1].second, dib = clipboard_probe::handles[0].second;
            const std::string pngHash = bench::Hash(GlobalLock(png), size);
            const std::string dibHash = bench::Hash(GlobalLock(dib), sizeof(BITMAPINFOHEADER) + image.px.size() * 4);
            GlobalUnlock(png); GlobalUnlock(dib);
            const double lockMs = (clipboard_probe::closed.QuadPart - clipboard_probe::opened.QuadPart) * 1000.0 / bench::frequency.QuadPart;
            clip << (engine == 0 ? "zipshot-legacy-dual-encode" : engine == 1 ? "candidate-reuse-alpha-png" : "production-wic-prepared") << ','
                 << (trial == -1 ? "first" : "warm") << ',' << trial << ',' << elapsed << ',' << lockMs << ',' << size << ','
                 << pngHash << ',' << dibHash << ',' << bench::Hash(disk.data(), disk.size()) << '\n';
            file.close(); clipboard_probe::Clear(); DeleteFileW(path.c_str());
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
    std::printf("PASS %dx%d %ls n=%d\n", width, height, kind.c_str(), n);
    return 0;
}
