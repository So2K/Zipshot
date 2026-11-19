// Guest-only actual HWND editor. Production renderer/event handlers are included unchanged.
// This helper records handler receipt and paint CPU timing, NOT first-visible latency.
#include "../../src/editor.cpp"
#include <cstdio>
#include <shlobj.h>

namespace {
struct EventSample { UINT message; WPARAM w; LPARAM l; LONGLONG begin,end; };
std::vector<EventSample> events;
WNDPROC originalProc=nullptr;
HWND ownedWindow=nullptr;
std::wstring outputPrefix;
LONGLONG clockFrequency=0;
ULONGLONG stopAt=0;
bool finishing=false;

bool IsApprovedGuest() {
    wchar_t user[256]={}, opt[8]={}; DWORD length=256,session=0;
    if(!GetUserNameW(user,&length)||_wcsicmp(user,L"WDAGUtilityAccount")!=0) return false;
    if(!ProcessIdToSessionId(GetCurrentProcessId(),&session)||session==0) return false;
    if(!GetEnvironmentVariableW(L"ZIPSHOT_RUNTIME_ISOLATED",opt,8)||wcscmp(opt,L"1")!=0) return false;
    DWORD attributes=GetFileAttributesW(L"C:\\ZipshotLab\\input\\.isolation-approved.json");
    return attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY);
}

void WriteState(const wchar_t* suffix) {
    auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(ownedWindow,GWLP_USERDATA));
    if(!e) return;
    POINT origin{}; ClientToScreen(ownedWindow,&origin);
    RECT client{},window{};GetClientRect(ownedWindow,&client);GetWindowRect(ownedWindow,&window);
    FILE* file=nullptr;
    if(_wfopen_s(&file,(outputPrefix+suffix).c_str(),L"wb")||!file) return;
    DWORD session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);
    std::fprintf(file,"{\"engine\":\"Zipshot actual HWND editor\",\"scope\":\"guest_window_state_not_visual_latency\",\"pid\":%lu,\"session\":%lu,\"hwnd\":\"%p\",\"qpc_frequency\":%lld,\"window\":[%ld,%ld,%ld,%ld],\"client_origin\":[%ld,%ld],\"client_size\":[%ld,%ld],\"image_size\":[%d,%d],\"image_to_client\":{\"scale\":%.9f,\"off_x\":%.6f,\"off_y\":%.6f},\"shape_count\":%zu,\"undo_count\":%zu,\"redo_count\":%zu,\"hwnd_render_target_created\":%s}\n",GetCurrentProcessId(),session,ownedWindow,clockFrequency,window.left,window.top,window.right,window.bottom,origin.x,origin.y,client.right,client.bottom,e->img->w,e->img->h,e->scale,e->offX,e->offY,e->shapes.size(),e->undo.size(),e->redo.size(),e->rt?"true":"false");
    std::fclose(file);
}

bool Snapshot() {
    auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(ownedWindow,GWLP_USERDATA));
    if(!e) return false;
    Image document=e->Export();
    bool documentOk=!document.empty()&&EncodeImage(document,outputPrefix+L"-snapshot.png",false);
    bool foreground=GetForegroundWindow()==ownedWindow;
    RECT rect{};GetWindowRect(ownedWindow,&rect);
    bool visibleAtSamples=true;
    for(POINT point:std::vector<POINT>{{rect.left+(rect.right-rect.left)/2,rect.top+60},{rect.left+(rect.right-rect.left)/4,rect.top+(rect.bottom-rect.top)/2},{rect.left+(rect.right-rect.left)*3/4,rect.top+(rect.bottom-rect.top)/2}}){HWND hit=WindowFromPoint(point);visibleAtSamples&=hit==ownedWindow||IsChild(ownedWindow,hit);}
    Image visible;
    // Read actual displayed pixels only in the approved disposable guest, never from the host.
    if(visibleAtSamples&&IsWindowVisible(ownedWindow)&&!IsIconic(ownedWindow))visible=CaptureRect(rect,false);
    bool windowOk=!visible.empty()&&EncodeImage(visible,outputPrefix+L"-window.png",false);
    size_t nonuniform=0;
    if(!visible.px.empty())for(uint32_t pixel:visible.px)nonuniform+=pixel!=visible.px[0];
    FILE* file=nullptr;
    if(!_wfopen_s(&file,(outputPrefix+L"-snapshot.json").c_str(),L"wb")&&file){
        std::fprintf(file,"{\"document_method\":\"Production Editor::Export\",\"document_ok\":%s,\"width\":%d,\"height\":%d,\"annotation_count\":%zu,\"foreground_matches_owned_window\":%s,\"owned_window_visible_at_samples\":%s,\"window_capture_method\":\"Actual guest CaptureRect on visible owned window\",\"window_capture_ok\":%s,\"window_nonuniform_pixels\":%zu}\n",documentOk?"true":"false",document.w,document.h,e->shapes.size(),foreground?"true":"false",visibleAtSamples?"true":"false",windowOk?"true":"false",nonuniform);
        std::fclose(file);
    }
    return documentOk&&windowOk;
}

void Finish() {
    if(finishing) return;finishing=true;
    WriteState(L"-final.json");
    FILE* file=nullptr;
    if(!_wfopen_s(&file,(outputPrefix+L"-events.json").c_str(),L"wb")&&file) {
        std::fprintf(file,"{\"scope\":\"Input handler receipt and WM_PAINT CPU duration only. No DwmFlush, forced repaint or inferred visible-frame timestamp.\",\"qpc_frequency\":%lld,\"events\":[",clockFrequency);
        for(size_t i=0;i<events.size();++i) {
            const auto& s=events[i];
            std::fprintf(file,"%s{\"message\":%u,\"wparam\":%llu,\"lparam\":%lld,\"qpc_begin\":%lld,\"qpc_end\":%lld}",i?",":"",s.message,(unsigned long long)s.w,(long long)s.l,s.begin,s.end);
        }
        std::fputs("]}\n",file);std::fclose(file);
    }
    if(auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(ownedWindow,GWLP_USERDATA))) e->dirty=false;
    DestroyWindow(ownedWindow);PostQuitMessage(0);
}

LRESULT CALLBACK ObserveProc(HWND h,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_APP+300){WriteState(L"-state.json");return 1;}
    if(message==WM_APP+301){Finish();return 1;}
    if(message==WM_APP+302){return Snapshot()?1:0;}
    bool observed=message==WM_PAINT||message==WM_MOUSEMOVE||message==WM_LBUTTONDOWN||message==WM_LBUTTONUP||message==WM_MOUSEWHEEL||message==WM_KEYDOWN||message==WM_KEYUP;
    LARGE_INTEGER a{},b{};if(observed) QueryPerformanceCounter(&a);
    LRESULT result=CallWindowProcW(originalProc,h,message,w,l);
    if(observed) {QueryPerformanceCounter(&b);events.push_back({message,w,l,a.QuadPart,b.QuadPart});}
    return result;
}

void CALLBACK StopTimer(HWND,UINT,UINT_PTR,DWORD) {if(GetTickCount64()>=stopAt) Finish();}
LRESULT CALLBACK Dispatcher(HWND h,UINT m,WPARAM w,LPARAM l) {
    if(m==WM_APP+2){auto* fn=reinterpret_cast<std::function<void()>*>(l);(*fn)();delete fn;return 0;}
    return DefWindowProcW(h,m,w,l);
}
}

int wmain(int argc,wchar_t** argv) {
    // Refuse before OleInitialize, editor/window creation or any input-related action on the host.
    if(!IsApprovedGuest()){std::fputs("Refused: approved Windows Sandbox guest identity, interactive session, marker and opt-in are required.\n",stderr);return 20;}
    if(argc!=6){std::fputs("fixture output-prefix width height lifetime-ms\n",stderr);return 2;}
    int width=_wtoi(argv[3]),height=_wtoi(argv[4]),lifetime=_wtoi(argv[5]);
    if(width<640||height<480||lifetime<1000||lifetime>300000)return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);g_inst=GetModuleHandleW(nullptr);g_icon=LoadAppIcon(32);
    WNDCLASSW cls{};cls.hInstance=g_inst;cls.lpfnWndProc=Dispatcher;cls.lpszClassName=L"ZipshotEditorBenchmarkDispatcher";RegisterClassW(&cls);
    g_main=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,L"",WS_POPUP,0,0,0,0,nullptr,nullptr,g_inst,nullptr);
    auto image=std::make_shared<Image>();if(!LoadImageFile(argv[1],*image))return 3;
    outputPrefix=argv[2];size_t slash=outputPrefix.find_last_of(L"\\/");
    if(slash!=std::wstring::npos)SHCreateDirectoryExW(nullptr,outputPrefix.substr(0,slash).c_str(),nullptr);
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);clockFrequency=frequency.QuadPart;
    events.reserve(20000);g_quietEditor=false;OpenEditor(image,L"Runtime comparison fixture.png");ownedWindow=OwnEditorWindow();if(!ownedWindow)return 4;
    originalProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(ownedWindow,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ObserveProc)));
    auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(ownedWindow,GWLP_USERDATA));
    e->o.border=Rgb(255,0,0);e->o.borderSize=4;e->o.shadow=0;e->o.blur=15;e->tool=T_Select;
    SetWindowPos(ownedWindow,HWND_TOP,40,40,width,height,SWP_SHOWWINDOW);e->LayoutAll();
    // Initial guest-only focus preparation. No input is simulated and measured sequences start later.
    HWND prior=GetForegroundWindow();DWORD priorThread=prior?GetWindowThreadProcessId(prior,nullptr):0;
    bool attached=priorThread&&priorThread!=GetCurrentThreadId()&&AttachThreadInput(GetCurrentThreadId(),priorThread,TRUE);
    BringWindowToTop(ownedWindow);SetForegroundWindow(ownedWindow);SetActiveWindow(ownedWindow);SetFocus(ownedWindow);
    if(attached)AttachThreadInput(GetCurrentThreadId(),priorThread,FALSE);
    InvalidateRect(ownedWindow,nullptr,FALSE);UpdateWindow(ownedWindow); // Initial readiness only, before measured input.
    WriteState(L"-ready.json");stopAt=GetTickCount64()+lifetime;SetTimer(ownedWindow,0xE11B,100,StopTimer);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    DestroyWindow(g_main);OleUninitialize();return 0;
}
