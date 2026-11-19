#define wmain ImageProfileMain
#include "image_native.cpp"
#undef wmain

// Frozen literal LoadImageFile implementation from1fdf0a7; renamed only.
static bool OldWic(const std::wstring& path, Image& out) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom))) return false;
    UINT w, h;
    if (FAILED(conv->GetSize(&w, &h)) || !w || !h || (uint64_t)w * h > UINT_MAX / 4) return false;
    Image loaded;
    loaded.w = (int)w;
    loaded.h = (int)h;
    loaded.px.resize((size_t)w * h);
    if (FAILED(conv->CopyPixels(nullptr, w * 4, (UINT)(loaded.px.size() * 4), (BYTE*)loaded.px.data()))) return false;
    loaded.alpha = std::any_of(loaded.px.begin(), loaded.px.end(), [](uint32_t p) { return (p >> 24) != 255; });
    out = std::move(loaded);
    return true;
}
#ifndef ZIPSHOT_WIC_REFERENCE_ONLY
int wmain(int argc,wchar_t**argv){
    if(argc<4)return 2;
    const std::wstring folder=std::filesystem::absolute(argv[1]).wstring();std::filesystem::create_directories(folder);
    const int n=_wtoi(argv[2]);
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);SetProcessAffinityMask(GetCurrentProcess(),15);
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);QueryPerformanceFrequency(&bench::frequency);
    std::ofstream csv(std::filesystem::path(folder+L"\\wic-native.csv"));
    csv<<"file,engine,phase,trial,ms,decoded_sha256,width,height,alpha,exact\n";
    for(int argument=3;argument<argc;++argument){
        const std::wstring file=std::filesystem::absolute(argv[argument]).wstring();Image reference;
        if(!OldWic(file,reference))return 3;
        for(int trial=-1;trial<n;++trial)for(int order=0;order<2;++order){
            const int engine=(order+(trial>=0?trial%2:0))%2;Image decoded;
            const double begin=bench::Clock();const bool ok=engine?LoadImageFile(file,decoded):OldWic(file,decoded);const double ms=bench::Clock()-begin;
            if(!ok||decoded.w!=reference.w||decoded.h!=reference.h||decoded.alpha!=reference.alpha||decoded.px!=reference.px)return 4;
            csv<<std::filesystem::path(file).generic_string()<<','<<(engine?"wic-production-final":"wic-1fdf0a7-reference")<<','
               <<(trial<0?"first":"warm")<<','<<trial<<','<<ms<<','<<bench::Hash(decoded.px.data(),decoded.px.size()*4)<<','
               <<decoded.w<<','<<decoded.h<<','<<decoded.alpha<<",1\n";
        }
    }
    CoUninitialize();std::printf("PASS native WIC %d inputs n=%d alternating\n",argc-3,n);return 0;
}
#endif
