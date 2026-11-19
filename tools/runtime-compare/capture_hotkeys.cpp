// Genuine full-app hotkey path observer. Refuses all screen/input access outside the Sandbox guest.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <set>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <map>
#include <cstdio>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
static long long tick() { LARGE_INTEGER q{}; QueryPerformanceCounter(&q); return q.QuadPart; }
static std::string utf8(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
    std::string r(n,0); WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),r.data(),n,nullptr,nullptr); return r;
}
static std::string quote(const std::wstring& s) {
    std::string r="\""; for(unsigned char c:utf8(s)) { if(c=='\\'||c=='\"') r+='\\'; if(c>=32) r+=c; } return r+'\"';
}
static bool lab_path(const std::wstring& path) {
    wchar_t full[32768]{}; if(!GetFullPathNameW(path.c_str(),32768,full,nullptr)) return false;
    std::wstring p=full; std::transform(p.begin(),p.end(),p.begin(),towlower);
    return p.rfind(L"c:\\zipshotlab\\",0)==0;
}
static bool image_path(const fs::path& p) {
    auto e=p.extension().wstring(); std::transform(e.begin(),e.end(),e.begin(),towlower);
    return e==L".png"||e==L".jpg"||e==L".jpeg"||e==L".bmp";
}
static std::set<std::wstring> images(const fs::path& root) {
    std::set<std::wstring> result; std::error_code error;
    for(fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied,error),end;it!=end;it.increment(error)) {
        if(error) { error.clear(); continue; }
        if(it->is_regular_file(error)&&image_path(it->path())) result.insert(it->path().wstring());
    }
    return result;
}
static bool belongs_to(DWORD pid,DWORD target) {
    if(pid==target)return true;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE)return false;
    std::map<DWORD,DWORD> parents;PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(Process32FirstW(snapshot,&entry))do{parents[entry.th32ProcessID]=entry.th32ParentProcessID;}while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    for(int depth=0;pid&&depth<16;++depth){auto it=parents.find(pid);if(it==parents.end()||it->second==pid)break;pid=it->second;if(pid==target)return true;}
    return false;
}
static std::set<HWND> visible_windows(DWORD pid) {
    struct State {DWORD pid;std::set<HWND> windows;} state{pid,{}};
    EnumWindows([](HWND hwnd,LPARAM lp)->BOOL {auto* s=(State*)lp;DWORD owner=0;GetWindowThreadProcessId(hwnd,&owner);
        RECT r{};if(IsWindowVisible(hwnd)&&GetWindowRect(hwnd,&r)&&r.right-r.left>32&&r.bottom-r.top>32&&belongs_to(owner,s->pid))s->windows.insert(hwnd);return TRUE;},(LPARAM)&state);
    return state.windows;
}
struct Observer {
    int x,y,w,h; HDC screen=nullptr,mem=nullptr; HBITMAP bitmap=nullptr; HGDIOBJ old=nullptr; uint32_t* pixels=nullptr;
    Observer(int ax,int ay,int aw,int ah):x(ax),y(ay),w(aw),h(ah) {
        BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h;
        bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
        screen=GetDC(nullptr);mem=CreateCompatibleDC(screen);
        bitmap=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,(void**)&pixels,nullptr,0);
        if(bitmap)old=SelectObject(mem,bitmap);
    }
    ~Observer(){if(old)SelectObject(mem,old);if(bitmap)DeleteObject(bitmap);if(mem)DeleteDC(mem);if(screen)ReleaseDC(nullptr,screen);}
    bool grab(){return pixels&&BitBlt(mem,0,0,w,h,screen,x,y,SRCCOPY|CAPTUREBLT)&&GdiFlush();}
    int changes(const std::vector<uint32_t>& base) const {
        int count=0;
        for(size_t i=0;i<base.size();++i) {
            uint32_t a=base[i],b=pixels[i];
            if(abs((int)(a&255)-(int)(b&255))>=8 || abs((int)((a>>8)&255)-(int)((b>>8)&255))>=8 || abs((int)((a>>16)&255)-(int)((b>>16)&255))>=8)
                if(++count>=16)break;
        }
        return count;
    }
};
static bool save_full_frame(const fs::path& path,RECT desktop) {
    Observer frame(desktop.left,desktop.top,desktop.right-desktop.left,desktop.bottom-desktop.top);
    if(!frame.grab())return false;
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+(DWORD)((size_t)frame.w*frame.h*4);
    info.biSize=sizeof(info);info.biWidth=frame.w;info.biHeight=-frame.h;info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    std::ofstream stream(path,std::ios::binary);stream.write((char*)&file,sizeof(file));stream.write((char*)&info,sizeof(info));
    stream.write((char*)frame.pixels,(std::streamsize)frame.w*frame.h*4);return stream.good();
}
static UINT key(bool region,WORD vk=VK_SNAPSHOT) {
    INPUT k[4]{}; for(auto& a:k)a.type=INPUT_KEYBOARD;
    if(region){k[0].ki.wVk=k[3].ki.wVk=VK_CONTROL;k[1].ki.wVk=k[2].ki.wVk=vk;k[2].ki.dwFlags=k[3].ki.dwFlags=KEYEVENTF_KEYUP;}
    else{k[0].ki.wVk=k[1].ki.wVk=vk;k[1].ki.dwFlags=KEYEVENTF_KEYUP;}
    return SendInput(region?4:2,k,sizeof(INPUT));
}
static bool clipboard_image(DWORD sequence,int& width,int& height) {
    if(GetClipboardSequenceNumber()==sequence||!OpenClipboard(nullptr)) return false;
    bool ok=false;
    for(UINT format:{CF_DIBV5,CF_DIB}) {
        HGLOBAL h=GetClipboardData(format); if(!h)continue;
        auto* b=(BITMAPINFOHEADER*)GlobalLock(h);
        if(b&&GlobalSize(h)>=sizeof(BITMAPINFOHEADER)&&b->biSize>=sizeof(BITMAPINFOHEADER)&&b->biWidth>0&&b->biHeight!=0) {
            width=b->biWidth;height=abs(b->biHeight);ok=true;
        }
        if(b)GlobalUnlock(h);if(ok)break;
    }
    if(!ok)if(HBITMAP h=(HBITMAP)GetClipboardData(CF_BITMAP)) {BITMAP b{};if(GetObjectW(h,sizeof(b),&b)){width=b.bmWidth;height=b.bmHeight;ok=width>0&&height>0;}}
    CloseClipboard();return ok;
}
static bool completed_image(const std::wstring& path,IWICImagingFactory* wic,UINT& w,UINT& h) {
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};bool complete=GetFileSizeEx(f,&size)&&size.QuadPart>8;CloseHandle(f);if(!complete)return false;
    ComPtr<IWICBitmapDecoder> decoder;ComPtr<IWICBitmapFrameDecode> frame;
    return SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder))&&
           SUCCEEDED(decoder->GetFrame(0,&frame))&&SUCCEEDED(frame->GetSize(&w,&h))&&w>0&&h>0;
}
int wmain(int argc,wchar_t** argv) {
    wchar_t username[128]{};DWORD user_size=128;
    if(!GetUserNameW(username,&user_size)||_wcsicmp(username,L"WDAGUtilityAccount"))return 20;
    if(argc<10||argc>12)return 2;
    DWORD pid=wcstoul(argv[1],nullptr,10);bool region=std::wstring(argv[2])==L"region";
    if(!region&&std::wstring(argv[2])!=L"full")return 2;
    int count=_wtoi(argv[3]),x=_wtoi(argv[4]),y=_wtoi(argv[5]),w=_wtoi(argv[6]),h=_wtoi(argv[7]),warmups=argc>=11?_wtoi(argv[10]):3;
    bool snapshots=argc==12&&std::wstring(argv[11])==L"snapshots";
    std::wstring save=argv[8],destination=argv[9];
    if(count<1||count>100||warmups<0||warmups>20||w<64||h<64||w>4096||h>2160||!lab_path(save)||!lab_path(destination))return 3;
    DWORD session=0,target_session=0;ProcessIdToSessionId(GetCurrentProcessId(),&session);ProcessIdToSessionId(pid,&target_session);
    if(session!=target_session)return 4;
    HANDLE target=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);if(!target)return 5;
    wchar_t path[32768]{};DWORD path_size=32768;QueryFullProcessImageNameW(target,0,path,&path_size);
    auto name=fs::path(path).filename().wstring();std::transform(name.begin(),name.end(),name.begin(),towlower);
    if(name!=L"zipshot.exe"&&name!=L"sharex.exe"){CloseHandle(target);return 6;}
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
    desktop.right=desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);desktop.bottom=desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if(x<desktop.left||y<desktop.top||x+w>desktop.right||y+h>desktop.bottom)return 7;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);ComPtr<IWICImagingFactory> wic;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic))))return 8;
    fs::create_directories(save);fs::create_directories(fs::path(destination).parent_path());
    Observer observer(x,y,w,h);if(!observer.grab())return 9;
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    long long anchor_before=tick();FILETIME utc{};GetSystemTimePreciseAsFileTime(&utc);long long anchor_after=tick();
    ULARGE_INTEGER utc_i{};utc_i.LowPart=utc.dwLowDateTime;utc_i.HighPart=utc.dwHighDateTime;
    std::ofstream json(fs::path(destination),std::ios::binary);
    json<<"{\"schema_version\":1,\"scenario\":\""<<(region?"region":"full")<<"\",\"target_pid\":"<<pid<<",\"target_executable\":"<<quote(name)
        <<",\"guest_session_id\":"<<session<<",\"qpc_frequency\":"<<frequency.QuadPart<<",\"anchor_qpc\":"<<(anchor_before+anchor_after)/2
        <<",\"anchor_unix_100ns\":"<<utc_i.QuadPart-116444736000000000ULL<<",\"desktop\":{\"x\":"<<desktop.left<<",\"y\":"<<desktop.top<<",\"width\":"<<desktop.right-desktop.left<<",\"height\":"<<desktop.bottom-desktop.top<<"}"
        <<",\"observer_roi\":{\"x\":"<<x<<",\"y\":"<<y<<",\"width\":"<<w<<",\"height\":"<<h<<"},\"input_method\":\"Guest SendInput OS keyboard events; not hardware keystrokes\",\"visual_method\":\"Persistent GDI composited-desktop ROI readback; >=16 pixels with >=8 channel difference\",\"scope\":\"Full application global hotkey path; no product capture test/CLI API is invoked\",\"trials\":[\n";
    bool all=true;int output_index=0;
    for(int i=-warmups;i<count;++i) {
        if(WaitForSingleObject(target,0)!=WAIT_TIMEOUT){all=false;break;}
        Sleep(1200); // Identical inter-trial settling; outside all measured intervals.
        if(!observer.grab()){all=false;break;}
        std::vector<uint32_t> baseline(observer.pixels,observer.pixels+(size_t)w*h);
        auto known=images(save);std::set<std::wstring> pending;
        auto initial_windows=visible_windows(pid);
        DWORD foreground_before=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground_before);
        DWORD clip_sequence=GetClipboardSequenceNumber();int clip_w=0,clip_h=0;UINT file_w=0,file_h=0;
        long long sent=tick();UINT inserted=key(region);long long send_return=tick();
        long long visible=0,previous_frame=sent,baseline_last=sent,clip=0,file=0,last_scan=0,first_file_ready=0;
        std::wstring filename;std::ostringstream frames;int frame_count=0;double max_gap=0;
        DWORD foreground_visible=0;bool target_window_evidence=false;
        while((tick()-sent)*1000.0/frequency.QuadPart<8000) {
            long long before=tick();if(!observer.grab()){all=false;break;}long long after=tick();
            int changed=observer.changes(baseline);++frame_count;
            if(frame_count>1)frames<<',';
            frames<<"["<<before<<','<<after<<','<<changed<<"]";
            max_gap=std::max(max_gap,(after-previous_frame)*1000.0/frequency.QuadPart);previous_frame=after;
            if(!visible){if(changed>=16){visible=after;GetWindowThreadProcessId(GetForegroundWindow(),&foreground_visible);
                target_window_evidence=belongs_to(foreground_visible,pid);
                if(!target_window_evidence)for(auto hwnd:visible_windows(pid))if(!initial_windows.count(hwnd)){target_window_evidence=true;break;}
            }else baseline_last=before;}
            if(!region) {
                if(!clip&&clipboard_image(clip_sequence,clip_w,clip_h))clip=tick();
                if(!file) {
                    if(!last_scan||(after-last_scan)*1000.0/frequency.QuadPart>=5) {
                        for(auto& candidate:images(save))if(!known.count(candidate))pending.insert(candidate);
                        last_scan=after;
                    }
                    for(auto& candidate:pending)if(completed_image(candidate,wic.Get(),file_w,file_h)){file=tick();filename=candidate;first_file_ready=file;break;}
                }
                if(clip&&file&&(visible||(after-first_file_ready)*1000.0/frequency.QuadPart>=500))break;
            } else if(visible)break;
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            Sleep(2);
        }
        // Endpoint timestamps are already fixed. This full-desktop evidence capture
        // and disk write occur outside the measured interval, before region cancel.
        std::wstring endpoint_frame;
        long long evidence_start=0,evidence_end=0,escape_sent=0;
        if(snapshots&&visible) {
            evidence_start=tick();
            auto frame_path=fs::path(destination).parent_path()/(L"capture-endpoint-"+std::to_wstring(i)+L".bmp");
            if(save_full_frame(frame_path,desktop))endpoint_frame=frame_path.wstring();
            evidence_end=tick();
        }
        if(region){Sleep(snapshots?1500:400);escape_sent=tick();key(false,VK_ESCAPE);Sleep(300);}
        bool pass=inserted==(region?4U:2U)&&(region?(visible!=0&&target_window_evidence):(clip!=0&&file!=0));all=all&&pass;
        auto ms=[&](long long endpoint){return endpoint?(endpoint-sent)*1000.0/frequency.QuadPart:-1.0;};
        if(output_index++)json<<",\n";
        json<<"{\"index\":"<<i<<",\"warmup\":"<<(i<0?"true":"false")<<",\"status\":\""<<(pass?"pass":"fail")<<"\",\"send_qpc\":"<<sent<<",\"send_return_qpc\":"<<send_return<<",\"input_events_inserted\":"<<inserted
            <<",\"last_baseline_qpc\":"<<baseline_last<<",\"first_visible_qpc\":"<<visible<<",\"visible_lower_bound_ms\":"<<ms(baseline_last)<<",\"visible_upper_bound_ms\":"<<ms(visible)
            <<",\"foreground_before_pid\":"<<foreground_before<<",\"foreground_at_first_visible_pid\":"<<foreground_visible<<",\"target_window_evidence\":"<<(target_window_evidence?"true":"false")
            <<",\"endpoint_full_frame\":"<<quote(endpoint_frame)<<",\"endpoint_frame_capture_after_timing\":true"
            <<",\"evidence_capture_start_qpc\":"<<evidence_start<<",\"evidence_capture_end_qpc\":"<<evidence_end<<",\"escape_sent_qpc\":"<<escape_sent<<",\"configured_region_post_evidence_hold_ms\":"<<(snapshots?1500:400)
            <<",\"clipboard_valid_qpc\":"<<clip<<",\"clipboard_observed_ms\":"<<ms(clip)<<",\"clipboard_width\":"<<clip_w<<",\"clipboard_height\":"<<clip_h
            <<",\"file_complete_qpc\":"<<file<<",\"file_observed_ms\":"<<ms(file)<<",\"file_width\":"<<file_w<<",\"file_height\":"<<file_h<<",\"file_path\":"<<quote(filename)
            <<",\"observer_frames\":"<<frame_count<<",\"max_observer_gap_ms\":"<<max_gap<<",\"frame_samples_qpc_changed_count\":["<<frames.str()<<"]}";
        json.flush();printf("trial=%d warmup=%d pass=%d visual_ms=%.3f clipboard_ms=%.3f file_ms=%.3f\n",i,i<0,pass,ms(visible),ms(clip),ms(file));fflush(stdout);
    }
    json<<"\n],\"status\":\""<<(all?"pass":"fail")<<"\",\"limitations\":[\"Pixel response is interval-censored by observer readback; report bounds and sampling gap.\",\"Clipboard/file endpoints are first observed valid images, including observer validation overhead.\",\"Region cancellation is an injected Escape after the measured first visible response.\",\"VM display and GPU path may differ from the host; do not mix with host API benchmarks.\"]}\n";
    wic.Reset(); // Release apartment-bound WIC objects before uninitializing COM.
    CloseHandle(target);CoUninitialize();return all?0:1;
}
