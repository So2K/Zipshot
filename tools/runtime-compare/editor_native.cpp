// Calls the exact production editor export path, not the documentation WIC adapter.
// No windows, desktop capture, clipboard, input or installed settings are used.
#include "../../src/editor.cpp"
#include <cstdio>
#include <stdexcept>

static void ConfigureExportCase(Editor& e, const std::wstring& name) {
    e.shapes.clear(); e.fxCache.clear();
    float w = (float)e.img->w, h = (float)e.img->h;
    auto shape = [&](Tool tool, std::vector<D2D1_POINT_2F> points) {
        Shape s{tool}; s.pts = std::move(points); s.color = Rgb(255,0,0);
        s.color2 = Rgb(0,0,0,0); s.size = 4; s.shadow = false;
        if (tool == T_Freehand) Editor::BuildGeo(s);
        if (tool == T_Blur) s.size2 = 15;
        e.shapes.push_back(std::move(s));
    };
    if (name == L"arrow") shape(T_Arrow, {{w*.2f,h*.75f},{w*.8f,h*.25f}});
    else if (name == L"arrows32") {
        for (int i=0;i<32;++i) {
            float y=h*(.12f+.72f*i/31);
            shape(T_Arrow, {{w*.15f,y},{w*.85f,h-y}});
        }
    } else if (name == L"freehand") {
        std::vector<D2D1_POINT_2F> points;
        for(int i=0;i<120;++i) points.push_back({w*(.15f+.7f*i/119),h*(.5f+.18f*(float)std::sin(i*6*3.141592653589793/119))});
        shape(T_Freehand,std::move(points));
    } else if (name == L"blur_cold" || name == L"blur_warm") shape(T_Blur,{{w*.25f,h*.3f},{w*.75f,h*.7f}});
    else if (name != L"plain") throw std::runtime_error("Unknown export case");
}

int wmain(int argc, wchar_t** argv) {
    if (argc!=6) { std::fprintf(stderr,"fixture case warmups samples output.png\n"); return 2; }
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    if(!SetProcessAffinityMask(GetCurrentProcess(),0xF)) return 8; // Same four logical CPUs for both export harnesses.
    SetPriorityClass(GetCurrentProcess(),NORMAL_PRIORITY_CLASS);
    g_inst=GetModuleHandleW(nullptr);
    auto source=std::make_shared<Image>();
    if(!LoadImageFile(argv[1],*source)) return 3;
    Editor editor; editor.img=source;
    std::wstring name=argv[2]; int warm=_wtoi(argv[3]), count=_wtoi(argv[4]);
    if(warm<0||count<1||count>10000) return 2;
    ConfigureExportCase(editor,name);
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    std::vector<double> times;
    Image output;
    for(int i=-warm;i<count;++i) {
        if(name==L"blur_cold") editor.fxCache.clear();
        LARGE_INTEGER a,b; QueryPerformanceCounter(&a);
        Image next=editor.Export();
        QueryPerformanceCounter(&b);
        output=std::move(next); // Dispose the previous output outside the timed interval, like ShareX.
        if(output.empty()||output.w!=source->w||output.h!=source->h||output.px.size()!=source->px.size()) return 4;
        if(i>=0) times.push_back((b.QuadPart-a.QuadPart)*1000.0/frequency.QuadPart);
    }
    size_t changed=0; uint64_t signature=1469598103934665603ull;
    for(size_t i=0;i<output.px.size();++i) { changed += output.px[i]!=source->px[i]; signature=(signature^output.px[i])*1099511628211ull; }
    if(name!=L"plain" && !changed) return 5;
    if(name==L"plain" && changed) return 6;
    if(!EncodeImage(output,argv[5],false)) return 7;
    std::printf("{\"engine\":\"Zipshot production Editor::Export\",\"scope\":\"render_export_only_not_UI_latency\",\"width\":%d,\"height\":%d,\"case\":\"%ls\",\"warmups\":%d,\"samples\":%d,\"changed_pixels\":%zu,\"pixel_signature\":\"%016llx\",\"ms\":[",source->w,source->h,name.c_str(),warm,count,changed,signature);
    for(size_t i=0;i<times.size();++i) std::printf("%s%.6f",i?",":"",times[i]);
    std::puts("]}");
    CoUninitialize(); return 0;
}
