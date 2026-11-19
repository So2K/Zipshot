// Final production API benchmark: raw fixture -> completed file, no UI/clipboard.
#define wmain ImageProfileMain
#include "image_native.cpp"
#undef wmain
#include "../../src/png_encode_fast.h"

static std::vector<BYTE> FileBytes(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()};
}
int wmain(int argc,wchar_t**argv){
    if(argc!=7)return 2;
    const std::wstring source=std::filesystem::absolute(argv[1]).wstring();
    const std::wstring folder=std::filesystem::absolute(argv[2]).wstring();
    const int width=_wtoi(argv[3]),height=_wtoi(argv[4]),n=_wtoi(argv[6]);
    const bool alpha=std::wstring(argv[5])==L"true";
    if(width<1||height<1||n<1||n>100)return 2;
    std::filesystem::create_directories(folder);
    Image image{width,height,alpha,std::vector<uint32_t>((size_t)width*height)};
    std::ifstream input(std::filesystem::path(source+L"\\fixture.bgra"),std::ios::binary);
    if(!input.read((char*)image.px.data(),image.px.size()*4))return 3;
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);SetProcessAffinityMask(GetCurrentProcess(),15);
    QueryPerformanceFrequency(&bench::frequency);CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    g_jpegOverKB=2048;g_jpegQuality=90;
    const auto sourceHash=bench::Hash(image.px.data(),image.px.size()*4);
    std::ofstream csv(std::filesystem::path(folder+L"\\production.csv"));
    csv<<"engine,operation,phase,trial,ms,bytes,format,source_sha256,encoded_sha256,decoded_sha256,exact_pixels,old_jpeg_exact\n";
    const std::wstring representative=folder+L"\\production.png";
    for(int operation=0;operation<4;++operation)for(int trial=-1;trial<n;++trial){
        std::wstring file=operation==2?folder+L"\\auto.png":representative;
        std::vector<BYTE> bytes;Image decoded;bool ok=false;
        const double start=bench::Clock();
        if(operation==0)ok=TryEncodePngFast(image,bytes);
        else if(operation==1)ok=EncodeImage(image,file,false);
        else if(operation==2)ok=SaveImageAuto(image,file);
        else ok=LoadImageFile(file,decoded);
        const double ms=bench::Clock()-start;
        if(!ok)return 4;
        if(operation==0)bench::Write(file,bytes.data(),bytes.size());
        else bytes=FileBytes(file);
        std::string encodedHash,decodedHash;bool exact=true,oldJpegExact=true;
        const bool jpeg=std::filesystem::path(file).extension()==L".jpg";
        if(trial<0){
            encodedHash=bench::Hash(bytes.data(),bytes.size());
            if(operation!=3&&!LoadImageFile(file,decoded))return 5;
            decodedHash=bench::Hash(decoded.px.data(),decoded.px.size()*4);
            exact=decoded.w==image.w&&decoded.h==image.h&&decoded.alpha==image.alpha&&decoded.px==image.px;
            if(jpeg)oldJpegExact=bytes==FileBytes(source+L"\\engine0.jpg");
            if((!jpeg&&!exact)||(jpeg&&!oldJpegExact))return 6;
        }
        csv<<"zipshot-production,"<<(operation==0?"png-encode-memory":operation==1?"png-encode-file-forced":operation==2?"save-auto-default":"png-decode-file")
           <<','<<(trial<0?"first":"warm")<<','<<trial<<','<<ms<<','<<bytes.size()<<','<<(jpeg?"JPEG":"PNG")<<','
           <<sourceHash<<','<<encodedHash<<','<<decodedHash<<','<<(!jpeg&&exact)<<','<<oldJpegExact<<'\n';
        if(operation==2)DeleteFileW(file.c_str());
    }
    CoUninitialize();std::printf("PASS production PNG/Auto %dx%d alpha=%d n=%d\n",width,height,alpha,n);return 0;
}
