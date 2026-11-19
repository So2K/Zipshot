#define wmain ImageProfileMain
#include "image_native.cpp"
#undef wmain
#include "../../src/png_limit_stream.h"
#include <chrono>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    const std::wstring dir = std::filesystem::absolute(argv[1]).wstring();
    if (!std::filesystem::create_directories(dir)) return 3;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    SetProcessAffinityMask(GetCurrentProcess(), 15);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); QueryPerformanceFrequency(&bench::frequency);
    std::ofstream csv(std::filesystem::path(dir + L"\\threshold.csv"));
    csv << "case,trial,engine,ms,bytes,exceeded,append,writes,seeks,resizes,encoded_sha256\n";
    for (const wchar_t* kind : {L"ui", L"photo", L"entropy"}) {
        Image image = bench::Fixture(3840,2160,kind);
        std::string label;
        for (const wchar_t* c=kind;*c;++c) label+=(char)*c;
        for (int trial=-1; trial<9; ++trial) for (int slot=0;slot<2;++slot) {
            const int engine = trial >= 0 && trial % 2 ? 1-slot : slot;
            ComPtr<IStream> memory; memory.Attach(SHCreateMemStream(nullptr,0));
            ComPtr<PngLimitStream> limit;
            if(engine) limit.Attach(new PngLimitStream(memory.Get(),2048*1024));
            const double start=bench::Clock();
            bool ok=Encode(image,engine?static_cast<IStream*>(limit.Get()):memory.Get(),false);
            bool useJpeg=engine?limit->Exceeded():false;
            if(ok&&!useJpeg){STATSTG stat{};if(FAILED(memory->Stat(&stat,STATFLAG_NONAME)))return 4;useJpeg=stat.cbSize.QuadPart>2048*1024;}
            if(!ok&&!useJpeg)return 5;
            if(useJpeg){memory.Attach(SHCreateMemStream(nullptr,0));if(!Encode(image,memory.Get(),true))return 6;}
            const double elapsed=bench::Clock()-start;
            const auto bytes=bench::Bytes(memory.Get());
            csv << label << ',' << trial << ',' << engine << ',' << elapsed << ',' << bytes.size() << ',' << useJpeg << ','
                << (!limit||limit->AppendOnly()) << ',' << (limit?limit->Writes():0) << ',' << (limit?limit->Seeks():0) << ',' << (limit?limit->Resizes():0) << ',' << bench::Hash(bytes.data(),bytes.size()) << '\n';
        }
    }
    CoUninitialize(); std::printf("Threshold profile complete\n"); return 0;
}
