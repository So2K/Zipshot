#include "clipboard_shelf.h"
#include "clipboard_dib.h"
#include <wincodec.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <filesystem>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace shelf {
namespace {
constexpr size_t kMaxBlob = 512ull * 1024 * 1024;
constexpr uint64_t kMaxPixels = 128ull * 1024 * 1024;

struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool valid() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h = INVALID_HANDLE_VALUE) : value(h) {}
    ~Handle() { close(); }
    void close() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE && value; }
};

std::wstring FullPath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!n) return {};
    std::wstring out(n, L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
    if (!written || written >= n) return {};
    out.resize(written);
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}
std::wstring Native(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}
std::wstring Leaf(const std::wstring& path) {
    return path.substr(path.find_last_of(L"\\/") + 1);
}
bool Supported(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    for (auto allowed : {L".png", L".jpg", L".jpeg", L".bmp", L".webp", L".tif", L".tiff", L".gif", L".mp4", L".webm", L".mkv", L".mov", L".avi"})
        if (!_wcsicmp(ext, allowed)) return true;
    return false;
}
void AddError(std::wstring& error, const std::wstring& message) {
    if (error.size() > 1800) return;
    if (!error.empty()) error += L"\n";
    error += message;
}
std::wstring Failure(const wchar_t* ru, const wchar_t* en, const std::wstring& path = {}) {
    std::wstring text = Tr(ru, en);
    if (!path.empty()) text += L" " + Leaf(path);
    return text;
}
bool ImageValid(const Image& image) {
    return image.w > 0 && image.h > 0 && (uint64_t)image.w * image.h == image.px.size() && image.px.size() <= UINT_MAX / 4;
}

// WIC BGRA is straight alpha. PBGRA would darken translucent pixels after PNG encoding.
bool Decode(const std::vector<BYTE>& bytes, Image& out) {
    if (bytes.empty() || bytes.size() > kMaxBlob || bytes.size() > UINT_MAX) return false;
    ComScope com;
    if (!com.valid()) return false;
    ComPtr<IStream> stream; stream.Attach(SHCreateMemStream(bytes.data(), (UINT)bytes.size()));
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (!stream || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || !w || !h || w > INT_MAX || h > INT_MAX || (uint64_t)w * h > kMaxPixels || (uint64_t)w * h > UINT_MAX / 4) return false;
    Image image;
    image.w = (int)w; image.h = (int)h; image.px.resize((size_t)w * h);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, (UINT)image.px.size() * 4, (BYTE*)image.px.data()))) return false;
    image.alpha = std::any_of(image.px.begin(), image.px.end(), [](uint32_t p) { return (p >> 24) != 255; });
    out = std::move(image);
    return true;
}
bool DecodeDib(const std::vector<BYTE>& bytes, Image& out) {
    if (bytes.size() < sizeof(BITMAPINFOHEADER)) return false;
    BITMAPINFOHEADER header; std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.biWidth <= 0 || !header.biHeight || header.biHeight == LONG_MIN || header.biPlanes != 1 ||
        (header.biBitCount != 1 && header.biBitCount != 4 && header.biBitCount != 8 && header.biBitCount != 16 && header.biBitCount != 24 && header.biBitCount != 32) ||
        (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS)) return false;
    if (header.biBitCount <= 8 && header.biClrUsed > (1u << header.biBitCount)) return false;
    std::vector<BYTE> file; WORD bitCount = 0;
    if (!shapes_ext::detail::WrapClipboardDib(bytes.data(), bytes.size(), file, bitCount)) return false;
    BITMAPFILEHEADER fh; std::memcpy(&fh, file.data(), sizeof(fh));
    uint64_t stride = (((uint64_t)header.biWidth * bitCount + 31) / 32) * 4;
    uint64_t height = header.biHeight < 0 ? -(int64_t)header.biHeight : header.biHeight;
    if ((uint64_t)header.biWidth * height > kMaxPixels || stride * height > file.size() - fh.bfOffBits) return false;
    DWORD alphaMask = 0;
    if (header.biSize >= 56 && bytes.size() >= 56) std::memcpy(&alphaMask, bytes.data() + 52, sizeof(alphaMask));
    if (alphaMask && header.biSize == 56) {
        // WIC treats the older V3 alpha header as BGR. Promote only its metadata to V5.
        BITMAPV5HEADER v5{}; std::memcpy(&v5, bytes.data(), 56); v5.bV5Size = sizeof(v5); v5.bV5CSType = LCS_sRGB;
        std::vector<BYTE> normalized(sizeof(v5) + bytes.size() - 56);
        std::memcpy(normalized.data(), &v5, sizeof(v5));
        std::memcpy(normalized.data() + sizeof(v5), bytes.data() + 56, bytes.size() - 56);
        if (!shapes_ext::detail::WrapClipboardDib(normalized.data(), normalized.size(), file, bitCount)) return false;
    }
    if (!Decode(file, out)) return false;
    if (!alphaMask) { for (auto& pixel : out.px) pixel |= 0xFF000000u; out.alpha = false; }
    // An explicit V4/V5 alpha mask is meaningful even when every alpha byte is zero.
    return true;
}
bool Snapshot(UINT format, std::vector<BYTE>& out) {
    if (!IsClipboardFormatAvailable(format)) return false;
    HANDLE data = GetClipboardData(format);
    size_t size = data ? GlobalSize(data) : 0;
    if (!size || size > kMaxBlob) return false;
    void* memory = GlobalLock(data);
    if (!memory) return false;
    try { out.assign((BYTE*)memory, (BYTE*)memory + size); }
    catch (...) { GlobalUnlock(data); throw; }
    GlobalUnlock(data);
    return true;
}
bool ValidDrop(HANDLE data) {
    size_t size = data ? GlobalSize(data) : 0;
    if (size < sizeof(DROPFILES) || size > kMaxBlob) return false;
    const BYTE* bytes = (const BYTE*)GlobalLock(data);
    if (!bytes) return false;
    DROPFILES header; std::memcpy(&header, bytes, sizeof(header));
    bool terminated = false;
    if (header.pFiles >= sizeof(header) && header.pFiles < size) {
        if (header.fWide && header.pFiles % sizeof(wchar_t) == 0) {
            wchar_t previous = 1;
            for (size_t p = header.pFiles; p + sizeof(wchar_t) <= size; p += sizeof(wchar_t)) {
                wchar_t ch; std::memcpy(&ch, bytes + p, sizeof(ch));
                if (!ch && !previous) { terminated = true; break; }
                previous = ch;
            }
        } else if (!header.fWide) {
            for (size_t p = header.pFiles; p + 1 < size; ++p) if (!bytes[p] && !bytes[p + 1]) { terminated = true; break; }
        }
    }
    GlobalUnlock(data);
    return terminated;
}

// The only cleanup target is the exact temporary file identity created by this operation.
struct Temporary {
    std::wstring path;
    Handle guard;
    BY_HANDLE_FILE_INFORMATION identity{};
    bool published = false;
    ~Temporary() {
        guard.close();
        if (published || path.empty()) return;
        Handle file(CreateFileW(Native(path).c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        BY_HANDLE_FILE_INFORMATION current{};
        if (file && GetFileInformationByHandle(file.value, &current) && current.dwVolumeSerialNumber == identity.dwVolumeSerialNumber &&
            current.nFileIndexHigh == identity.nFileIndexHigh && current.nFileIndexLow == identity.nFileIndexLow) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
};
bool Reserve(const std::wstring& dir, Temporary& temp) {
    static std::atomic<unsigned long long> serial{0};
    for (int i = 0; i < 100; ++i) {
        temp.path = dir + L"\\.zipshot-shelf-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial) + L".tmp";
        temp.guard.value = CreateFileW(Native(temp.path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (temp.guard) return GetFileInformationByHandle(temp.guard.value, &temp.identity) != FALSE;
        DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) { temp.path.clear(); return false; }
    }
    temp.path.clear(); return false;
}
std::wstring SafeName(std::wstring name) {
    for (auto& ch : name) if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) ch = L'_';
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
    std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);
    if (stem.empty()) stem = L"Clipboard";
    if (stem.size() > 140) stem.resize(140);
    if (!stem.empty() && stem.back() >= 0xD800 && stem.back() <= 0xDBFF) stem.pop_back();
    std::wstring device = stem.substr(0, stem.find(L'.'));
    if (!_wcsicmp(device.c_str(), L"CON") || !_wcsicmp(device.c_str(), L"PRN") || !_wcsicmp(device.c_str(), L"AUX") || !_wcsicmp(device.c_str(), L"NUL") ||
        (device.size() == 4 && (!_wcsnicmp(device.c_str(), L"COM", 3) || !_wcsnicmp(device.c_str(), L"LPT", 3)) && device[3] >= L'1' && device[3] <= L'9')) stem.insert(stem.begin(), L'_');
    return stem + ext;
}
bool Publish(Temporary& temp, const std::wstring& dir, const std::wstring& name, std::wstring& out) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) dot = name.size();
    temp.guard.close();
    for (int n = 0; n < 10000; ++n) {
        std::wstring suffix = n ? L" (" + std::to_wstring(n) + L")" : L"";
        std::wstring path = dir + L"\\" + name.substr(0, dot) + suffix + name.substr(dot);
        if (MoveFileExW(Native(temp.path).c_str(), Native(path).c_str(), MOVEFILE_WRITE_THROUGH)) {
            temp.published = true; out = std::move(path); return true;
        }
        DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) return false;
    }
    return false;
}
bool CopyBytes(HANDLE source, const std::wstring& path) {
    Handle target(CreateFileW(Native(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!target) return false;
    std::array<BYTE, 65536> buffer;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(source, buffer.data(), (DWORD)buffer.size(), &read, nullptr)) return false;
        if (!read) break;
        DWORD offset = 0;
        while (offset < read) {
            DWORD written = 0;
            if (!WriteFile(target.value, buffer.data() + offset, read - offset, &written, nullptr) || !written) return false;
            offset += written;
        }
    }
    return FlushFileBuffers(target.value) != FALSE;
}
}  // namespace

std::wstring Directory() {
    std::wstring root = SaveDirRoot();
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    return root.empty() ? L"" : root + L"\\Clipboard";
}
bool IsShelfPath(const std::wstring& path) {
    if (path.empty() || Directory().empty()) return false;
    std::wstring file = FullPath(path), dir = FullPath(Directory());
    if (file.empty() || dir.empty() || file.size() < dir.size()) return false;
    if (CompareStringOrdinal(file.data(), (int)dir.size(), dir.data(), (int)dir.size(), TRUE) != CSTR_EQUAL) return false;
    return file.size() == dir.size() || file[dir.size()] == L'\\';
}

bool ReadClipboard(HWND owner, Payload& out, std::wstring& error) {
    out = {}; error.clear();
    try {
        std::array<std::vector<BYTE>, 3> images;
        {
            bool opened = false;
            for (int i = 0; i < 5 && !opened; ++i) { opened = OpenClipboard(owner) != FALSE; if (!opened) Sleep(10); }
            if (!opened) { error = Tr(L"Буфер обмена занят. Повторите вставку.", L"Clipboard is busy. Try pasting again."); return false; }
            struct Close { ~Close() { CloseClipboard(); } } close;
            if (IsClipboardFormatAvailable(CF_HDROP)) {
                HDROP drop = (HDROP)GetClipboardData(CF_HDROP);
                if (!ValidDrop(drop)) { error = Tr(L"Не удалось прочитать список файлов буфера обмена.", L"Could not read the clipboard file list."); return false; }
                UINT count = drop ? DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) : 0;
                if (count > 4096) { error = Tr(L"Слишком много файлов в буфере обмена.", L"Too many clipboard files."); return false; }
                for (UINT i = 0; i < count; ++i) {
                    UINT n = DragQueryFileW(drop, i, nullptr, 0);
                    if (!n || n > 32767) continue;
                    std::wstring file((size_t)n + 1, L'\0');
                    if (DragQueryFileW(drop, i, file.data(), n + 1) != n) continue;
                    file.resize(n);
                    if (Supported(file)) out.files.push_back(std::move(file));
                    else AddError(error, Failure(L"Неподдерживаемый файл:", L"Unsupported file:", file));
                }
                if (!out.files.empty()) return true;
                if (count) { if (error.empty()) error = Tr(L"Не удалось прочитать файлы буфера обмена.", L"Could not read the clipboard files."); return false; }
            }
            UINT png = RegisterClipboardFormatW(L"PNG");
            if (png) Snapshot(png, images[0]);
            Snapshot(CF_DIBV5, images[1]); Snapshot(CF_DIB, images[2]);
        }  // Close before WIC decoding or any file IO.
        if (Decode(images[0], out.image) || DecodeDib(images[1], out.image) || DecodeDib(images[2], out.image)) return true;
        error = Tr(L"В буфере обмена нет поддерживаемого изображения или файла.", L"Clipboard contains no supported image or file.");
    } catch (...) { out = {}; error = Tr(L"Не удалось прочитать изображение из буфера обмена.", L"Could not read the clipboard image."); }
    return false;
}

SaveResult Store(Payload data) {
    SaveResult result;
    if (data.files.empty() && !ImageValid(data.image)) { result.error = Tr(L"Нет поддерживаемого изображения или файла.", L"No supported image or file."); return result; }
    try {
        std::wstring dir = Directory().empty() ? L"" : FullPath(Directory());
        if (dir.empty()) { result.error = Tr(L"Папка сохранения не настроена.", L"Save directory is not configured."); return result; }
        std::error_code code;
        std::filesystem::create_directories(std::filesystem::path(Native(dir)), code);
        if (code || !std::filesystem::is_directory(std::filesystem::path(Native(dir)), code)) { result.error = Tr(L"Не удалось создать папку буфера обмена.", L"Could not create the Clipboard folder."); return result; }
        if (!data.files.empty()) {
            for (const auto& file : data.files) {
                std::wstring source = FullPath(file);
                if (!Supported(file)) { AddError(result.error, Failure(L"Неподдерживаемый файл:", L"Unsupported file:", file)); continue; }
                DWORD attrs = source.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(Native(source).c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) { AddError(result.error, Failure(L"Не удалось прочитать файл:", L"Could not read file:", file)); continue; }
                Handle input(CreateFileW(Native(source).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
                LARGE_INTEGER size{};
                if (!input || GetFileType(input.value) != FILE_TYPE_DISK || !GetFileSizeEx(input.value, &size) || size.QuadPart <= 0) { AddError(result.error, Failure(L"Файл пуст или занят:", L"File is empty or in use:", file)); continue; }
                Temporary temp;
                std::wstring saved;
                if (!Reserve(dir, temp) || !CopyBytes(input.value, temp.path) || !Publish(temp, dir, SafeName(Leaf(source)), saved)) { AddError(result.error, Failure(L"Не удалось сохранить файл:", L"Could not save file:", file)); continue; }
                result.paths.push_back(std::move(saved));
            }
        } else {
            ComScope com;
            Temporary temp;
            SYSTEMTIME now; GetLocalTime(&now);
            wchar_t name[80]; swprintf_s(name, L"Clipboard_%04u%02u%02u_%02u%02u%02u_%03u.png", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
            std::wstring saved;
            if (!com.valid() || !Reserve(dir, temp) || !EncodeImage(data.image, Native(temp.path), false) || !Publish(temp, dir, name, saved)) result.error = Tr(L"Не удалось сохранить изображение PNG.", L"Could not save the PNG image.");
            else result.paths.push_back(std::move(saved));
        }
    } catch (...) { AddError(result.error, Tr(L"Не удалось завершить сохранение в буфер.", L"Could not finish saving to the shelf.")); }
    return result;
}
}  // namespace shelf
