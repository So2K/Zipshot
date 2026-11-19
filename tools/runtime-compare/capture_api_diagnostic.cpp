// Static versus moving desktop diagnostic. NOT a full-application latency benchmark.
#if CAPTURE_DIAGNOSTIC_TRACING
#include "capture_diagnostic_impl.cpp"
#else
#include "../../src/capture.cpp"
#endif
#include <thread>
#include <shellapi.h>
#include <filesystem>
#include <atomic>
#include <fstream>
#include <cstdio>
#include <cstdarg>

static std::wstring prefix;
static bool moving=false;
static HWND scene=nullptr;
static std::atomic<int> paints{0};
void Log(const wchar_t* fmt,...) {
    if(prefix.empty())return;FILE* f=nullptr;_wfopen_s(&f,(prefix+L".log").c_str(),L"a, ccs=UTF-8");if(!f)return;
    va_list ap;va_start(ap,fmt);vfwprintf(f,fmt,ap);va_end(ap);fputwc(L'\n',f);fclose(f);
}
static LRESULT CALLBACK proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    static bool bright=false;
    if(message==WM_TIMER){bright=!bright;RECT tick{16,16,48,48};InvalidateRect(hwnd,&tick,FALSE);UpdateWindow(hwnd);return 0;}
    if(message==WM_PAINT){PAINTSTRUCT p{};HDC dc=BeginPaint(hwnd,&p);RECT r{};GetClientRect(hwnd,&r);
        HBRUSH bg=CreateSolidBrush(RGB(24,59,99));FillRect(dc,&r,bg);DeleteObject(bg);
        if(moving){RECT tick{16,16,48,48};HBRUSH b=CreateSolidBrush(bright?RGB(255,255,255):RGB(24,59,99));FillRect(dc,&tick,b);DeleteObject(b);}
        EndPaint(hwnd,&p);++paints;return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,message,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    wchar_t user[128]{};DWORD n=128;if(!GetUserNameW(user,&n)||_wcsicmp(user,L"WDAGUtilityAccount"))return 20;
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);if(argc!=5)return 2;
    moving=std::wstring(argv[1])==L"moving";int count=_wtoi(argv[2]),warmups=_wtoi(argv[3]);prefix=argv[4];
    wchar_t full_prefix[32768]{};GetFullPathNameW(prefix.c_str(),32768,full_prefix,nullptr);std::wstring normalized=full_prefix;
    std::replace(normalized.begin(),normalized.end(),L'\\',L'/');std::transform(normalized.begin(),normalized.end(),normalized.begin(),towlower);
    if(count<1||count>20||warmups<0||warmups>10||normalized.rfind(L"c:/zipshotlab/",0)!=0)return 3;
    LocalFree(argv);SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RECT r=VirtualScreen();WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=proc;wc.lpszClassName=L"ZipshotCaptureApiDiagnostic";RegisterClassW(&wc);
    scene=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"Static/moving API diagnostic fixture",WS_POPUP,r.left,r.top,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,nullptr);
    ShowWindow(scene,SW_SHOW);UpdateWindow(scene);if(moving)SetTimer(scene,1,16,nullptr);
    bool all=true;
    std::thread worker([&](){Sleep(600);LARGE_INTEGER f{};QueryPerformanceFrequency(&f);std::ofstream result(std::filesystem::path(prefix+L".json"),std::ios::binary);
        result<<"{\"mode\":\""<<(moving?"moving":"static")<<"\",\"instrumented\":"<<(CAPTURE_DIAGNOSTIC_TRACING?"true":"false")<<",\"scope\":\"CaptureRect API diagnostic only, no observer video or application hotkey path\",\"width\":"<<r.right-r.left<<",\"height\":"<<r.bottom-r.top<<",\"warmups\":"<<warmups<<",\"ms\":[";
        for(int i=-warmups;i<count;++i){LARGE_INTEGER a{},b{};Log(L"DIAGNOSTIC begin mode=%s sample=%d paints=%d",moving?L"moving":L"static",i,paints.load());QueryPerformanceCounter(&a);auto image=CaptureRect(r,false);QueryPerformanceCounter(&b);
            bool valid=!image.empty();if(valid)for(int j=1;j<=4;++j)valid=valid&&((image.px[(size_t)(image.h/2)*image.w+j*image.w/5]&0xffffff)==0x183b63);
            all=all&&valid;Log(L"DIAGNOSTIC end sample=%d ms=%.3f colour_valid=%d paints=%d",i,(b.QuadPart-a.QuadPart)*1000.0/f.QuadPart,valid,paints.load());
            if(i>=0)result<<(i?",":"")<<(b.QuadPart-a.QuadPart)*1000.0/f.QuadPart;Sleep(200);
        }
        result<<"],\"colour_checks_pass\":"<<(all?"true":"false")<<",\"paints\":"<<paints.load()<<"}\n";result.close();PostMessageW(scene,WM_CLOSE,0,0);
    });
    MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}worker.join();return all?0:1;
}
