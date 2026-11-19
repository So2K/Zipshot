// Actual WIC/SaveImageAuto, synthetic pixels and a stub clipboard only.
#define wmain ImageProfileMain
#include "../tools/perf/image_native.cpp"
#undef wmain
#include "../src/png_limit_stream.h"

int checks = 0, failures = 0;
void Check(bool good, const char* label) { ++checks; if (!good) ++failures; std::printf("%s %s\n", good ? "PASS" : "FAIL", label); }
std::vector<BYTE> ReadFileBytes(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    return std::vector<BYTE>(std::istreambuf_iterator<char>(file), {});
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto folder = std::filesystem::absolute(argv[1]);
    if (!std::filesystem::create_directories(folder)) return 3;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IStream> memory; memory.Attach(SHCreateMemStream(nullptr,0));
        ComPtr<PngLimitStream> limited; limited.Attach(new PngLimitStream(memory.Get(),32));
        BYTE bytes[33]{}; ULONG written = 0;
        Check(SUCCEEDED(limited->Write(bytes,32,&written)) && written==32 && !limited->Exceeded(), "exact byte threshold is allowed");
        Check(FAILED(limited->Write(bytes,1,&written)) && written==0 && limited->Exceeded(), "first byte above threshold is rejected without partial write");
        STATSTG stat{}; memory->Stat(&stat,STATFLAG_NONAME);
        Check(stat.cbSize.QuadPart==32, "threshold stop leaves only the completed prefix");
    }
    {
        ComPtr<IStream> memory; memory.Attach(SHCreateMemStream(nullptr,0));
        ComPtr<PngLimitStream> limited; limited.Attach(new PngLimitStream(memory.Get(),16));
        BYTE bytes[32]{}; ULONG written=0; LARGE_INTEGER zero{};
        limited->Write(bytes,8,&written); limited->Seek(zero,STREAM_SEEK_SET,nullptr);
        Check(SUCCEEDED(limited->Write(bytes,32,&written)) && !limited->AppendOnly() && !limited->Exceeded(), "a rewriting encoder disables early cutoff rather than guessing its final size");
    }
    {
        ComPtr<IStream> memory; memory.Attach(SHCreateMemStream(nullptr,0));
        ComPtr<PngLimitStream> limited; limited.Attach(new PngLimitStream(memory.Get(),16));
        ULARGE_INTEGER size{}; size.QuadPart=32;
        limited->SetSize(size); BYTE bytes[32]{};
        Check(SUCCEEDED(limited->Write(bytes,32,nullptr)) && !limited->AppendOnly() && !limited->Exceeded(), "preallocation uses the original full-size decision path");
    }

    int id=0;
    for (const wchar_t* kind : {L"ui",L"photo",L"entropy"}) {
        Image image=bench::Fixture(640,360,kind);
        ComPtr<IStream> full;full.Attach(SHCreateMemStream(nullptr,0));
        if(!Encode(image,full.Get(),false))return 4;
        const auto png=bench::Bytes(full.Get());
        for (uint64_t limit : {uint64_t(png.size()-1),uint64_t(png.size()),uint64_t(png.size()+1)}) {
            ComPtr<IStream> memory;memory.Attach(SHCreateMemStream(nullptr,0));
            ComPtr<PngLimitStream> bounded;bounded.Attach(new PngLimitStream(memory.Get(),limit));
            const bool ok=Encode(image,bounded.Get(),false);
            const bool crossing=png.size()>limit;
            Check(crossing ? !ok&&bounded->Exceeded() : ok&&!bounded->Exceeded(), "real PNG cutoff agrees with full encoder immediately below, at and above its final byte count");
            if(!crossing)Check(bench::Bytes(memory.Get())==png, "completed bounded PNG is byte-identical to unlimited WIC");
        }
        for(int kb : {1,32,2048}) {
            g_jpegOverKB=kb;g_jpegQuality=90;
            const bool jpeg=png.size()>(size_t)kb*1024;
            auto expected=png;
            if(jpeg){ComPtr<IStream> stream;stream.Attach(SHCreateMemStream(nullptr,0));if(!Encode(image,stream.Get(),true))return 5;expected=bench::Bytes(stream.Get());}
            std::wstring path=(folder/(L"case-"+std::to_wstring(id++)+L".png")).wstring();
            Check(SaveImageAuto(image,path), "production automatic save succeeds");
            Check(ReadFileBytes(path)==expected && path.ends_with(jpeg?L".jpg":L".png"), "production threshold choice and complete output bytes equal the previous full-PNG decision");
        }
    }
    Image alpha=bench::Fixture(63,39,L"alpha");g_jpegOverKB=1;
    std::wstring alphaPath=(folder/L"alpha.png").wstring();Image decoded;
    Check(SaveImageAuto(alpha,alphaPath)&&alphaPath.ends_with(L".png")&&LoadImageFile(alphaPath,decoded)&&decoded.px==alpha.px&&decoded.alpha, "alpha stays lossless PNG even when above automatic JPEG threshold");
    Image bad{2,2,false,{}};std::wstring badPath=(folder/L"invalid.png").wstring();
    Check(!SaveImageAuto(bad,badPath)&&!std::filesystem::exists(badPath), "invalid pixels are a codec failure, not a false JPEG size-limit result");
    CoUninitialize();std::printf("%d assertions, %d failures\n",checks,failures);return failures?1:0;
}
