#define ZIPSHOT_WIC_REFERENCE_ONLY
#include "image_wic_native.cpp"

static bool OriginalConverterFastAlpha(const std::wstring& path,Image& out){
    ComPtr<IWICImagingFactory> f;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f))))return false;
    ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
    if(FAILED(f->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder))||FAILED(decoder->GetFrame(0,&frame)))return false;
    WICPixelFormatGUID native{};frame->GetPixelFormat(&native);
    ComPtr<IWICFormatConverter> converter;
    if(FAILED(f->CreateFormatConverter(&converter))||FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return false;
    UINT w=0,h=0;if(FAILED(converter->GetSize(&w,&h))||!w||!h||(uint64_t)w*h>UINT_MAX/4)return false;
    Image image;image.w=(int)w;image.h=(int)h;image.px.resize((size_t)w*h);
    if(FAILED(converter->CopyPixels(nullptr,w*4,(UINT)(image.px.size()*4),(BYTE*)image.px.data())))return false;
    const bool opaque=native==GUID_WICPixelFormat24bppBGR||native==GUID_WICPixelFormat24bppRGB||native==GUID_WICPixelFormat32bppBGR;
    image.alpha=!opaque&&PixelsHaveTransparency(image.px.data(),image.px.size());out=std::move(image);return true;
}
int wmain(int argc,wchar_t**argv){
    if(argc<4)return 2;
    const std::wstring folder=std::filesystem::absolute(argv[1]).wstring();std::filesystem::create_directories(folder);
    const int n=_wtoi(argv[2]);SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);SetProcessAffinityMask(GetCurrentProcess(),15);
    QueryPerformanceFrequency(&bench::frequency);CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    std::ofstream csv(std::filesystem::path(folder+L"\\followup.csv"));csv<<"file,engine,trial,ms,exact\n";
    for(int argument=3;argument<argc;++argument){
        const std::wstring file=std::filesystem::absolute(argv[argument]).wstring();Image reference;if(!OldWic(file,reference))return 3;
        for(int trial=-1;trial<n;++trial)for(int order=0;order<3;++order){
            int engine=(order+(trial>=0?trial:0))%3;Image decoded;const double start=bench::Clock();
            const bool ok=engine==0?OldWic(file,decoded):engine==1?LoadImageFile(file,decoded):OriginalConverterFastAlpha(file,decoded);
            const double ms=bench::Clock()-start;if(!ok||decoded.w!=reference.w||decoded.h!=reference.h||decoded.alpha!=reference.alpha||decoded.px!=reference.px)return 4;
            csv<<std::filesystem::path(file).generic_string()<<','<<(engine==0?"original":engine==1?"direct-bgra-simd":"original-converter-simd")<<','<<trial<<','<<ms<<",1\n";
        }
    }
    CoUninitialize();std::printf("PASS trusted WIC followup%d inputs n%d\n",argc-3,n);return 0;
}
