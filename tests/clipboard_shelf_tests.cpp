// No system clipboard calls. ReadClipboard is compiled against the owned in-memory fake below.
#include "../src/common.h"
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <cstdio>
#include <map>
#include <set>
#include <thread>
#include <fstream>

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HICON g_icon = nullptr;
static std::wstring testRoot;
std::wstring ExeDir() { return testRoot; }
void Log(const wchar_t*, ...) {}
bool IsEnglish() { return true; }
void EnsureUnicodeIni() {}
std::wstring IniString(const wchar_t*, const wchar_t*, const wchar_t* value) { return value; }

#define SaveDirRoot ProductionSaveDirRoot
#define EncodeImage ProductionEncodeImage
#include "../src/output.cpp"
#undef SaveDirRoot
#undef EncodeImage
std::wstring SaveDirRoot() { return testRoot; }
static bool failEncode = false;
bool EncodeImage(const Image& image, const std::wstring& path, bool jpeg) {
    if (!failEncode) return ProductionEncodeImage(image, path, jpeg);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) { DWORD written; WriteFile(file, "bad", 3, &written, nullptr); CloseHandle(file); }
    return false;
}

static std::map<UINT, HGLOBAL> fakeClipboard;
static int fakeOpens = 0, fakeCloses = 0;
static BOOL FakeOpenClipboard(HWND) { ++fakeOpens; return TRUE; }
static BOOL FakeCloseClipboard() { ++fakeCloses; return TRUE; }
static BOOL FakeIsClipboardFormatAvailable(UINT format) { return fakeClipboard.count(format) != 0; }
static HANDLE FakeGetClipboardData(UINT format) { auto it = fakeClipboard.find(format); return it == fakeClipboard.end() ? nullptr : it->second; }
static UINT FakeRegisterClipboardFormatW(const wchar_t*) { return 0xC123; }
#define OpenClipboard FakeOpenClipboard
#define CloseClipboard FakeCloseClipboard
#define IsClipboardFormatAvailable FakeIsClipboardFormatAvailable
#define GetClipboardData FakeGetClipboardData
#define RegisterClipboardFormatW FakeRegisterClipboardFormatW
#include "../src/clipboard_shelf.cpp"
#undef OpenClipboard
#undef CloseClipboard
#undef IsClipboardFormatAvailable
#undef GetClipboardData
#undef RegisterClipboardFormatW

static int failures = 0;
static void Check(bool pass, const char* message) { std::printf("%s %s\n", pass ? "PASS" : "FAIL", message); if (!pass) ++failures; }
static std::vector<BYTE> Bytes(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static void Write(const std::wstring& path, const std::vector<BYTE>& bytes) {
    std::ofstream output(std::filesystem::path(path), std::ios::binary); output.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}
static void SetFake(UINT format, const std::vector<BYTE>& bytes) {
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    void* memory = GlobalLock(data); memcpy(memory, bytes.data(), bytes.size()); GlobalUnlock(data); fakeClipboard[format] = data;
}
static void ClearFake() { for (auto item : fakeClipboard) GlobalFree(item.second); fakeClipboard.clear(); }
static size_t TmpCount() {
    size_t count = 0;
    for (const auto& item : std::filesystem::directory_iterator(shelf::Directory())) if (item.path().extension() == L".tmp") ++count;
    return count;
}
static UINT GifFrames(const std::wstring& path) {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory; Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    UINT count = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) decoder->GetFrameCount(&count);
    return count;
}
static std::vector<BYTE> Dib(bool v5, bool alpha, int height = -1) {
    size_t header = v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
    std::vector<BYTE> bytes(header + 8, 0);
    BITMAPINFOHEADER info{}; info.biSize = (DWORD)header; info.biWidth = 2; info.biHeight = height; info.biPlanes = 1; info.biBitCount = 32;
    info.biCompression = v5 ? BI_BITFIELDS : BI_RGB;
    memcpy(bytes.data(), &info, sizeof(info));
    if (v5) {
        auto* extended = (BITMAPV5HEADER*)bytes.data(); extended->bV5RedMask = 0xFF0000; extended->bV5GreenMask = 0xFF00; extended->bV5BlueMask = 0xFF;
        extended->bV5AlphaMask = 0xFF000000; extended->bV5CSType = LCS_sRGB;
    }
    uint32_t pixels[] = {alpha ? 0x80112233u : 0x00112233u, 0x00445566u};
    memcpy(bytes.data() + header, pixels, sizeof(pixels)); return bytes;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring(argv[1]) == L"--verify-persistence") {
        testRoot = argv[2]; CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Image loaded; bool ok = shelf::IsShelfPath(argv[3]) && LoadImageFile(argv[3], loaded) && loaded.alpha && loaded.px == std::vector<uint32_t>({0x00112233u, 0x80123456u, 0xFFFFFFFFu, 0xFF123456u});
        CoUninitialize(); return ok ? 0 : 1;
    }
    if (argc != 2) return 2;
    testRoot = std::filesystem::absolute(argv[1]).wstring();
    std::filesystem::create_directories(testRoot);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Check(shelf::Store({}).paths.empty(), "empty payload rejected");
    Image malformed{2, 2, true, {0xFFFFFFFFu}};
    Check(shelf::Store({malformed, {}}).paths.empty(), "invalid image buffer rejected before encoding");
    Image transparent{2, 2, true, {0x00112233u, 0x80123456u, 0xFFFFFFFFu, 0xFF123456u}};
    auto saved = shelf::Store({transparent, {}});
    Check(saved.paths.size() == 1 && saved.error.empty(), "bitmap stored as a shelf PNG without caller COM setup assumptions");
    Image loaded;
    Check(saved.paths.size() == 1 && LoadImageFile(saved.paths[0], loaded) && loaded.alpha && loaded.px == transparent.px, "PNG roundtrip preserves straight alpha and exact pixels");
    Check(saved.paths.size() == 1 && shelf::IsShelfPath(saved.paths[0]) && !shelf::IsShelfPath(testRoot + L"\\ClipboardOther\\x.png") && !shelf::IsShelfPath(shelf::Directory()+L"\\..\\outside.png"), "shelf path classification honors case-independent directory boundaries");

    const std::vector<BYTE> gif = {'G','I','F','8','9','a',1,0,1,0,0x80,0,0,0,0,0,255,255,255,
        0x21,0xF9,4,0,5,0,0,0,0x2C,0,0,0,0,1,0,1,0,0,2,2,0x44,1,0,
        0x21,0xF9,4,0,5,0,0,0,0x2C,0,0,0,0,1,0,1,0,0,2,2,0x4C,1,0,0x3B};
    auto original = testRoot + L"\\animated.gif"; Write(original, gif);
    HANDLE oldTime = CreateFileW(original.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    SYSTEMTIME past{}; past.wYear=2001; past.wMonth=1; past.wDay=2; FILETIME pastTime{}; SystemTimeToFileTime(&past,&pastTime); SetFileTime(oldTime,nullptr,nullptr,&pastTime); CloseHandle(oldTime);
    auto one = shelf::Store({{}, {original}}); auto two = shelf::Store({{}, {original}});
    Check(one.paths.size()==1 && two.paths.size()==1 && one.paths[0]!=two.paths[0] && Bytes(one.paths[0])==gif && Bytes(two.paths[0])==gif, "duplicate filenames get distinct byte-identical copies");
    Check(one.paths.size()==1 && GifFrames(one.paths[0])==2, "animated GIF keeps both frames");
    std::filesystem::create_directories(testRoot+L"\\second");
    auto otherSource=testRoot+L"\\second\\animated.gif";auto otherGif=gif;otherGif[17]=0;otherGif[18]=0;Write(otherSource,otherGif);
    auto different=shelf::Store({{}, {otherSource}});
    Check(different.paths.size()==1 && different.paths[0]!=one.paths[0] && Bytes(different.paths[0])==otherGif && Bytes(one.paths[0])==gif, "same basename with different bytes never overwrites an existing stored file");
    WIN32_FILE_ATTRIBUTE_DATA srcTime{},dstTime{}; GetFileAttributesExW(original.c_str(),GetFileExInfoStandard,&srcTime); GetFileAttributesExW(one.paths[0].c_str(),GetFileExInfoStandard,&dstTime);
    Check(Bytes(original)==gif && CompareFileTime(&srcTime.ftLastWriteTime,&pastTime)==0 && CompareFileTime(&dstTime.ftLastWriteTime,&pastTime)>0, "source bytes/time unchanged; pasted copy sorts as newly added");
    auto movie = testRoot + L"\\video.mp4"; std::vector<BYTE> videoBytes={'f','t','y','p',0,1,2,3,4,5,6}; Write(movie,videoBytes);
    auto movieCopy=shelf::Store({{}, {movie}});
    Check(movieCopy.paths.size()==1 && Bytes(movieCopy.paths[0])==videoBytes && Bytes(movie)==videoBytes, "video bytes copied without decoding or rewriting source");
    auto text=testRoot+L"\\unsupported.txt";Write(text,{'t','e','x','t'});
    auto partial=shelf::Store({{}, {original,text,testRoot+L"\\missing.png"}});
    Check(partial.paths.size()==1 && !partial.error.empty() && Bytes(partial.paths[0])==gif, "partial errors retain successful files and explain unsupported/missing inputs");
    auto foreign=shelf::Directory()+L"\\.zipshot-shelf-foreign.tmp";Write(foreign,{'k','e','e','p'});
    size_t before=TmpCount();failEncode=true;auto failed=shelf::Store({transparent,{}});failEncode=false;
    Check(failed.paths.empty() && !failed.error.empty() && TmpCount()==before && Bytes(foreign)==std::vector<BYTE>({'k','e','e','p'}), "failed encoder removes only its own new staging file");

    constexpr int count=8;std::thread workers[count];shelf::SaveResult copies[count];
    for(int i=0;i<count;++i)workers[i]=std::thread([&,i]{copies[i]=shelf::Store({{}, {original}});});
    for(auto& worker:workers)worker.join();std::set<std::wstring> unique;bool all=true;
    for(auto& item:copies){all &= item.paths.size()==1 && item.error.empty();if(item.paths.size()==1){unique.insert(item.paths[0]);all &= Bytes(item.paths[0])==gif;}}
    Check(all && unique.size()==count, "concurrent stores publish unique completed files without overwrite");
    std::wstring backgroundPath; std::thread pngWorker([&]{auto imageResult=shelf::Store({transparent,{}});if(imageResult.paths.size()==1)backgroundPath=imageResult.paths[0];});pngWorker.join();
    Check(!backgroundPath.empty() && LoadImageFile(backgroundPath,loaded) && loaded.px==transparent.px, "PNG storage initializes COM on an uninitialized worker thread");

    shelf::Payload clip;std::wstring error;
    Check(!shelf::ReadClipboard(nullptr,clip,error) && !error.empty() && fakeOpens==fakeCloses, "unsupported fake clipboard rejected and closed");
    SetFake(0xC123,Bytes(saved.paths[0]));
    Check(shelf::ReadClipboard(nullptr,clip,error) && clip.image.px==transparent.px && clip.image.alpha, "registered PNG fake decodes straight BGRA without host clipboard access");ClearFake();
    SetFake(CF_DIBV5,Dib(true,true));
    Check(shelf::ReadClipboard(nullptr,clip,error) && clip.image.alpha && clip.image.px[0]==0x80112233u && clip.image.px[1]==0x00445566u, "DIBV5 preserves declared partial and fully transparent pixels");ClearFake();
    auto v4=Dib(true,true);v4.erase(v4.begin()+108,v4.begin()+124);((BITMAPINFOHEADER*)v4.data())->biSize=108;SetFake(CF_DIB,v4);
    Check(shelf::ReadClipboard(nullptr,clip,error) && clip.image.alpha && clip.image.px[0]==0x80112233u && clip.image.px[1]==0x00445566u, "DIBV4 declared alpha survives straight BGRA decoding");ClearFake();
    auto v3=Dib(true,true);v3.erase(v3.begin()+56,v3.begin()+124);((BITMAPINFOHEADER*)v3.data())->biSize=56;SetFake(CF_DIB,v3);
    Check(shelf::ReadClipboard(nullptr,clip,error) && clip.image.alpha && clip.image.px[0]==0x80112233u && clip.image.px[1]==0x00445566u, "older V3 alpha metadata is normalized without losing transparency");ClearFake();
    SetFake(CF_DIBV5,Dib(true,false));
    Check(shelf::ReadClipboard(nullptr,clip,error) && clip.image.alpha && (clip.image.px[0]>>24)==0 && (clip.image.px[1]>>24)==0, "fully transparent DIBV5 is not flattened to opaque");ClearFake();
    SetFake(CF_DIB,Dib(false,false));
    Check(shelf::ReadClipboard(nullptr,clip,error) && !clip.image.alpha && clip.image.px[0]==0xFF112233u, "ordinary DIB unused alpha is treated as opaque");ClearFake();
    auto truncated=Dib(true,true);truncated.pop_back();SetFake(CF_DIBV5,truncated);
    Check(!shelf::ReadClipboard(nullptr,clip,error) && clip.image.empty(), "truncated DIB pixels rejected");ClearFake();

    std::vector<BYTE> drop(sizeof(DROPFILES)+(original.size()+2)*sizeof(wchar_t),0);auto* header=(DROPFILES*)drop.data();header->pFiles=sizeof(DROPFILES);header->fWide=TRUE;memcpy(drop.data()+sizeof(DROPFILES),original.c_str(),original.size()*sizeof(wchar_t));
    SetFake(CF_HDROP,drop);SetFake(0xC123,Bytes(saved.paths[0]));
    Check(shelf::ReadClipboard(nullptr,clip,error) && clip.files==std::vector<std::wstring>{original} && clip.image.empty(), "file-drop wins over a PNG preview so animation is preserved");ClearFake();
    auto brokenDrop=drop;((DROPFILES*)brokenDrop.data())->pFiles=(DWORD)brokenDrop.size()+4;SetFake(CF_HDROP,brokenDrop);
    Check(!shelf::ReadClipboard(nullptr,clip,error) && clip.files.empty(), "malformed dropped-file bounds rejected before shell parsing");ClearFake();
    Check(fakeOpens==fakeCloses, "every fake clipboard open is closed");

    wchar_t self[32768];GetModuleFileNameW(nullptr,self,(DWORD)std::size(self));
    std::wstring command=L"\""+std::wstring(self)+L"\" --verify-persistence \""+testRoot+L"\" \""+saved.paths[0]+L"\"";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    bool started=CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE;
    DWORD code=1;if(started){WaitForSingleObject(process.hProcess,15000);GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);CloseHandle(process.hThread);}
    Check(started && code==0, "saved PNG remains readable in a fresh process after restart");
    CoUninitialize();return failures?1:0;
}
