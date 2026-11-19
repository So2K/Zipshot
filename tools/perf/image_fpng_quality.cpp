#define wmain ImageProfileMain
#include "image_native.cpp"
#undef wmain
#include "../../src/png_encode_fast.h"
#include "../../third_party/fpng/fpng.h"
#include <array>
#include <map>
#include <thread>

static int assertions = 0, failures = 0;
static void Check(bool ok, const char* message) {
    ++assertions; std::printf("%s %s\n", ok ? "PASS" : "FAIL", message); if (!ok) ++failures;
}
static std::vector<BYTE> Read(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}
static bool Metadata(const std::vector<BYTE>& png, std::map<std::string, std::vector<BYTE>>& selected) {
    const auto u32 = [&](size_t at) { return (uint32_t(png[at]) << 24) | (uint32_t(png[at + 1]) << 16) |
                                            (uint32_t(png[at + 2]) << 8) | png[at + 3]; };
    if (png.size() < 8 || memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8)) return false;
    size_t at = 8; bool ended = false;
    while (at + 12 <= png.size()) {
        const size_t length = u32(at);
        if (length > png.size() - at - 12 || fpng::fpng_crc32(png.data() + at + 4, length + 4) != u32(at + 8 + length)) return false;
        const std::string type((const char*)png.data() + at + 4, 4);
        if (type == "IHDR" || type == "sRGB" || type == "gAMA" || type == "pHYs")
            selected[type] = {png.begin() + at + 8, png.begin() + at + 8 + length};
        at += length + 12;
        if (type == "IEND") { ended = true; break; }
    }
    return ended && at == png.size();
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const std::wstring root = std::filesystem::absolute(argv[1]).wstring();
    if (std::filesystem::exists(root)) return 2;
    std::filesystem::create_directories(root);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS); SetProcessAffinityMask(GetCurrentProcess(), 15);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::array<std::vector<BYTE>, 4> parallel;
    std::array<bool, 4> succeeded{};
    const Image simultaneous = bench::Fixture(64, 64, L"alpha");
    std::array<std::thread, 4> threads;
    for (size_t i = 0; i < threads.size(); ++i)
        threads[i] = std::thread([&, i] { succeeded[i] = TryEncodePngFast(simultaneous, parallel[i]); });
    for (auto& thread : threads) thread.join();
    Check(std::all_of(succeeded.begin(), succeeded.end(), [](bool b) { return b; }) &&
          parallel[0] == parallel[1] && parallel[0] == parallel[2] && parallel[0] == parallel[3],
          "concurrent first calls initialize CPU dispatch once and produce deterministic identical PNGs");
    const std::array<SIZE, 7> sizes{{{1,1},{2,1},{1,128},{128,1},{17,33},{257,129},{1920,1080}}};
    int caseNumber = 0;
    for (const SIZE size : sizes) for (const wchar_t* kind : {L"ui", L"alpha", L"alpha-ui", L"entropy"}) {
        const Image image = bench::Fixture(size.cx, size.cy, kind);
        std::vector<BYTE> fast;
        const bool encoded = TryEncodePngFast(image, fast);
        ComPtr<IStream> stream; stream.Attach(SHCreateMemStream(nullptr, 0));
        const bool baselineOk = Encode(image, stream.Get(), false);
        const auto baseline = bench::Bytes(stream.Get());
        const std::wstring stem = root + L"\\case" + std::to_wstring(caseNumber++);
        bench::Write(stem + L"-wic.png", baseline.data(), baseline.size());
        bench::Write(stem + L"-fast.png", fast.data(), fast.size());
        Image a, b;
        std::map<std::string, std::vector<BYTE>> am, bm;
        Check(encoded && baselineOk && LoadImageFile(stem + L"-wic.png", a) && LoadImageFile(stem + L"-fast.png", b) &&
              a.w == b.w && a.h == b.h && a.alpha == b.alpha && a.px == b.px &&
              Metadata(baseline, am) && Metadata(fast, bm) && am == bm,
              "fast PNG edge-size corpus preserves WIC pixels, alpha, dimensions, color/DPI metadata and every chunk CRC");
    }
    std::vector<BYTE> untouched{1,2,3};
    Check(!TryEncodePngFast(Image{2,2,false,{0}}, untouched) && untouched == std::vector<BYTE>{1,2,3},
          "invalid source fails without changing caller output buffer");
    Image opaque = bench::Fixture(128, 80, L"ui");
    ComPtr<IStream> wic; wic.Attach(SHCreateMemStream(nullptr, 0)); Encode(opaque, wic.Get(), false);
    const auto original = bench::Bytes(wic.Get());
    g_jpegOverKB = 2048;
    std::wstring automatic = root + L"\\automatic.png";
    Check(SaveImageAuto(opaque, automatic) && Read(automatic) == original,
          "opaque automatic-JPEG setting retains exact WIC PNG decision and bytes");
    g_jpegOverKB = 0;
    std::wstring forced = root + L"\\forced.png";
    Image decoded;
    Check(SaveImageAuto(opaque, forced) && LoadImageFile(forced, decoded) && decoded.px == opaque.px &&
          Read(forced) != original, "disabled automatic JPEG uses new lossless PNG without changing pixels");
    auto alpha = std::make_shared<Image>(bench::Fixture(64, 80, L"alpha"));
    auto prepared = PreparePng(alpha);
    CopyPreparedImageToClipboard(prepared);
    std::wstring cached = root + L"\\cached.png";
    const bool saved = SavePreparedImageAuto(prepared, cached);
    const auto bytes = Read(cached);
    bool sameClipboard = false;
    if (saved && clipboard_probe::handles.size() == 2) {
        auto png = clipboard_probe::handles[1].second;
        sameClipboard = bench::Hash(GlobalLock(png), bytes.size()) == bench::Hash(bytes.data(), bytes.size()); GlobalUnlock(png);
    }
    clipboard_probe::Clear();
    Check(saved && sameClipboard && LoadImageFile(cached, decoded) && decoded.px == alpha->px,
          "generic prepared packet shares one exact fast PNG between clipboard and file");
    std::printf("fpng runtime SSE4.1/PCLMUL: %s\n", fpng::fpng_cpu_supports_sse41() ? "yes" : "scalar fallback");
    CoUninitialize();
    std::printf("%d assertions, %d failures\n", assertions, failures);
    return failures ? 1 : 0;
}
