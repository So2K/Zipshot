// Isolated lossless candidates, pinned upstream sources kept under ignored build/.
#define wmain ImageProfileMain
#include "image_native.cpp"
#undef wmain
#include <intrin.h>
#include "../../build/perf/images/vendor/fpng/fpng.h"
#pragma warning(push)
#pragma warning(disable: 4244) // Unmodified upstream header uses small integer option fields.
#include "../../build/perf/images/vendor/fpnge/fpnge.h"
#pragma warning(pop)
extern "C" size_t FPNGEEncodeAvx2(size_t, size_t, const void*, size_t, size_t, size_t, void*, const FPNGEOptions*);

static bool Cpu(bool avx) {
    int r[4]{}; __cpuid(r, 1);
    if (!(r[2] & (1 << 19)) || !(r[2] & (1 << 1))) return false;
    if (!avx) return true;
    if (!(r[2] & (1 << 27)) || !(r[2] & (1 << 28)) || (_xgetbv(0) & 6) != 6) return false;
    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) && (r[1] & (1 << 8));
}
static void AddColorChunks(std::vector<BYTE>& png) {
    const BYTE chunks[] = {
        0,0,0,1,'s','R','G','B',0, 0xAE,0xCE,0x1C,0xE9,
        0,0,0,4,'g','A','M','A',0,0,0xB1,0x8F, 0x0B,0xFC,0x61,0x05
    };
    png.insert(png.begin() + 33, std::begin(chunks), std::end(chunks));
}
static bool Fast(const Image& image, int engine, std::vector<BYTE>& output) {
    const unsigned channels = image.alpha ? 4 : 3;
    std::vector<BYTE> packed;
    const BYTE* data = (const BYTE*)image.px.data();
    if (engine < 2 || !image.alpha) {
        packed.resize(image.px.size() * channels);
        for (size_t i = 0; i < image.px.size(); ++i) {
            const uint32_t p = image.px[i];
            packed[i * channels] = engine < 2 ? (BYTE)(p >> 16) : (BYTE)p;
            packed[i * channels + 1] = (BYTE)(p >> 8);
            packed[i * channels + 2] = engine < 2 ? (BYTE)p : (BYTE)(p >> 16);
            if (channels == 4) packed[i * 4 + 3] = (BYTE)(p >> 24);
        }
        data = packed.data();
    }
    if (engine < 2) {
        if (!fpng::fpng_encode_image_to_memory(data, image.w, image.h, channels, output,
                                              engine == 1 ? fpng::FPNG_ENCODE_SLOWER : 0)) return false;
        AddColorChunks(output); // Same sRGB intent/gAMA metadata as current WIC output.
        return true;
    }
    const char intent = 0;
    const unsigned char gamma[] = {0, 0, 0xB1, 0x8F};
    const FPNGEAdditionalChunk chunks[] = {{{'s','R','G','B'}, &intent, 1},
                                         {{'g','A','M','A'}, reinterpret_cast<const char*>(gamma), 4}};
    FPNGEOptions options{}; FPNGEFillOptions(&options, 4, FPNGE_CICP_NONE);
    options.channel_order = FPNGE_ORDER_BGR;
    options.num_additional_chunks = 2; options.additional_chunks = chunks;
    output.resize(FPNGEOutputAllocSize(1, channels, image.w, image.h));
    const size_t size = engine == 2 ? FPNGEEncode(1, channels, data, image.w, image.w * channels, image.h, output.data(), &options)
                                    : FPNGEEncodeAvx2(1, channels, data, image.w, image.w * channels, image.h, output.data(), &options);
    if (!size || size > output.size()) return false;
    output.resize(size);
    return true;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 6) return 2;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS); SetProcessAffinityMask(GetCurrentProcess(), 15);
    QueryPerformanceFrequency(&bench::frequency); CoInitializeEx(nullptr, COINIT_MULTITHREADED); fpng::fpng_init();
    const std::wstring folder = std::filesystem::absolute(argv[1]).wstring();
    const int n = _wtoi(argv[5]); Image image = bench::Fixture(_wtoi(argv[3]), _wtoi(argv[4]), argv[2]);
    std::filesystem::create_directories(folder);
    const auto sourceHash = bench::Hash(image.px.data(), image.px.size() * 4);
    std::ofstream csv(std::filesystem::path(folder + L"\\fastpng.csv"));
    csv << "engine,operation,phase,trial,ms,bytes,source_sha256,decoded_sha256,exact_pixels,alpha\n";
    const char* names[] = {"fpng-default", "fpng-slower", "fpnge-sse41-level4", "fpnge-avx2-level4"};
    for (int engine = 0; engine < 4; ++engine) {
        if (engine >= 2 && !Cpu(engine == 3)) continue;
        std::wstring file = folder + L"\\fast" + std::to_wstring(engine) + L".png";
        size_t encodedSize = 0;
        for (int trial = -1; trial < n; ++trial) {
            std::vector<BYTE> bytes;
            const double start = bench::Clock(); const bool ok = Fast(image, engine, bytes); const double ms = bench::Clock() - start;
            if (!ok) return 3;
            encodedSize = bytes.size();
            std::string decodedHash; bool exact = true;
            if (trial == -1) {
                bench::Write(file, bytes.data(), bytes.size());
                Image decoded; if (!LoadImageFile(file, decoded)) return 4;
                decodedHash = bench::Hash(decoded.px.data(), decoded.px.size() * 4);
                exact = image.w == decoded.w && image.h == decoded.h && image.alpha == decoded.alpha && image.px == decoded.px;
                if (!exact) return 5;
            }
            csv << names[engine] << ",png-encode-memory," << (trial == -1 ? "first" : "warm") << ',' << trial << ',' << ms << ','
                << bytes.size() << ',' << sourceHash << ',' << decodedHash << ',' << exact << ',' << image.alpha << '\n';
        }
        for (int decoder = 0; decoder < 1; ++decoder) for (int trial = -1; trial < n; ++trial) {
            Image decoded;
            const double start = bench::Clock(); const bool ok = LoadImageFile(file, decoded); const double ms = bench::Clock() - start;
            if (!ok || decoded.px != image.px) return 6;
            csv << names[engine] << ',' << (decoder ? "fpng-decode-file-bgra" : "wic-decode-file") << ','
                << (trial == -1 ? "first" : "warm") << ',' << trial << ',' << ms << ',' << encodedSize << ','
                << sourceHash << ',' << bench::Hash(decoded.px.data(), decoded.px.size() * 4) << ",1," << decoded.alpha << '\n';
        }
    }
    CoUninitialize(); std::printf("PASS fast PNG %ls %dx%d n=%d\n", argv[2], image.w, image.h, n);
    return 0;
}
