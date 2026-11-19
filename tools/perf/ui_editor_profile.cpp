// Actual Editor::FxImage and Export, synthetic memory images, no HWND or input.
#define main UiBrushFunctionalMain
#include "../../tests/editor_brush_tests.cpp"
#undef main
#include "ui_editor_reference.h"
#include <chrono>
#include <psapi.h>

namespace perf {
double Clock(){LARGE_INTEGER q,f;QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);return q.QuadPart*1000.0/f.QuadPart;}
Image Fixture(int w,int h,bool alpha=false){Image im;im.w=w;im.h=h;im.alpha=alpha;im.px.resize((size_t)w*h);uint32_t state=0x47259173;for(auto& p:im.px){state^=state<<13;state^=state>>17;state^=state<<5;p=(alpha?state&0xff000000:0xff000000)|(state&0xffffff);}return im;}
void Pass(const std::vector<uint32_t>& src,std::vector<uint32_t>& dst,int w,int h,int r,bool horizontal,const std::vector<uint8_t>& quotient){
    if(horizontal){for(int y=0;y<h;++y){const uint32_t* row=src.data()+(size_t)y*w;uint32_t* out=dst.data()+(size_t)y*w;int rr=0,gg=0,bb=0;
        for(int x=-r;x<=r;++x){uint32_t p=row[std::clamp(x,0,w-1)];rr+=p>>16&255;gg+=p>>8&255;bb+=p&255;}
        for(int x=0;x<w;++x){out[x]=0xff000000u|(uint32_t)quotient[rr]<<16|(uint32_t)quotient[gg]<<8|quotient[bb];uint32_t a=row[std::min(x+r+1,w-1)],b=row[std::max(x-r,0)];rr+=(int)(a>>16&255)-(int)(b>>16&255);gg+=(int)(a>>8&255)-(int)(b>>8&255);bb+=(int)(a&255)-(int)(b&255);}}return;}
    std::vector<int> rr(w),gg(w),bb(w);
    for(int y=-r;y<=r;++y){const uint32_t* row=src.data()+(size_t)std::clamp(y,0,h-1)*w;for(int x=0;x<w;++x){uint32_t p=row[x];rr[x]+=p>>16&255;gg[x]+=p>>8&255;bb[x]+=p&255;}}
    for(int y=0;y<h;++y){uint32_t* out=dst.data()+(size_t)y*w;const uint32_t* add=src.data()+(size_t)std::min(y+r+1,h-1)*w;const uint32_t* remove=src.data()+(size_t)std::max(y-r,0)*w;
        for(int x=0;x<w;++x){out[x]=0xff000000u|(uint32_t)quotient[rr[x]]<<16|(uint32_t)quotient[gg[x]]<<8|quotient[bb[x]];uint32_t a=add[x],b=remove[x];rr[x]+=(int)(a>>16&255)-(int)(b>>16&255);gg[x]+=(int)(a>>8&255)-(int)(b>>8&255);bb[x]+=(int)(a&255)-(int)(b&255);}}
}
Image BlurRows(const Image& input,uint32_t strength){Image out=input;std::vector<uint32_t> temp(out.px.size());const int r=std::max<int>(1,strength/3),div=2*r+1;std::vector<uint8_t> quotient(255*div+1);for(size_t i=0;i<quotient.size();++i)quotient[i]=(uint8_t)(i/div);for(int i=0;i<3;++i){Pass(out.px,temp,out.w,out.h,r,true,quotient);Pass(temp,out.px,out.w,out.h,r,false,quotient);}return out;}
Image OpaqueCopy(const Image& input){Image out=input;for(auto& p:out.px)p|=0xff000000u;return out;}
bool Equal(const Image& a,const Image& b){return a.w==b.w&&a.h==b.h&&a.alpha==b.alpha&&a.px==b.px;}
void Print(const char* name,int w,int h,const std::vector<double>& values){auto s=values;std::sort(s.begin(),s.end());std::printf("{\"case\":\"%s\",\"width\":%d,\"height\":%d,\"median_ms\":%.6f,\"p95_ms\":%.6f,\"ms\":[",name,w,h,s[s.size()/2],s[(size_t)std::ceil(s.size()*.95)-1]);for(size_t i=0;i<values.size();++i)std::printf("%s%.6f",i?",":"",values[i]);std::printf("]}");}
}

int wmain(){
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);if(!SetProcessAffinityMask(GetCurrentProcess(),0xf))return 2;CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    bool exact=true;int equalityCases=0;struct Row{const char* name;int w,h;std::vector<double> ms;};std::vector<Row> rows;
    for(auto size:std::vector<std::pair<int,int>>{{1,1},{1,17},{19,1},{17,19},{101,73}})for(uint32_t strength:{3u,15u,35u,99u})for(bool alpha:{false,true}){
        Editor e;LegacyEditor old;e.img=std::make_shared<Image>(perf::Fixture(size.first,size.second,alpha));old.img=e.img;const Image baseline=old.FxImage(FX_Blur,strength),candidate=e.FxImage(FX_Blur,strength);exact&=perf::Equal(baseline,candidate);++equalityCases;}
    for(auto size:std::vector<std::pair<int,int>>{{1920,1080},{3840,2160}}){Editor e;LegacyEditor old;e.img=std::make_shared<Image>(perf::Fixture(size.first,size.second));old.img=e.img;
        Row blur{"cold_blur_before",size.first,size.second},fast{"cold_blur_after",size.first,size.second};
        for(int i=-3;i<20;++i){Image expected,actual;double a=0,b=0;auto baseline=[&]{old.fxCache.clear();const double begin=perf::Clock();const Image& computed=old.FxImage(FX_Blur,35);a=perf::Clock()-begin;expected=computed;};auto candidate=[&]{e.fxCache.clear();const double begin=perf::Clock();const Image& computed=e.FxImage(FX_Blur,35);b=perf::Clock()-begin;actual=computed;};if(i%2){candidate();baseline();}else{baseline();candidate();}exact&=perf::Equal(expected,actual);++equalityCases;if(i>=0){blur.ms.push_back(a);fast.ms.push_back(b);}}
        rows.push_back(std::move(blur));rows.push_back(std::move(fast));e.shapes.clear();
        Row exports{"plain_opaque_export_before",size.first,size.second},copies{"plain_opaque_export_after",size.first,size.second};
        for(int i=-3;i<20;++i){Image expected,actual;double a=0,b=0;auto baseline=[&]{const double begin=perf::Clock();expected=old.Export();a=perf::Clock()-begin;};auto candidate=[&]{const double begin=perf::Clock();actual=e.Export();b=perf::Clock()-begin;};if(i%2){candidate();baseline();}else{baseline();candidate();}exact&=perf::Equal(expected,actual);++equalityCases;if(i>=0){exports.ms.push_back(a);copies.ms.push_back(b);}}
        rows.push_back(std::move(exports));rows.push_back(std::move(copies));}
    bool trims=true;std::vector<DWORD> faults;
    {Editor e;e.img=std::make_shared<Image>(perf::Fixture(3840,2160));Row warm{"plain_export_without_own_ws_trim",3840,2160},trimmed{"plain_export_after_own_ws_trim",3840,2160};
        for(int i=0;i<8;++i){Image output=e.Export();double begin=perf::Clock();output=e.Export();warm.ms.push_back(perf::Clock()-begin);
            trims&=SetProcessWorkingSetSize(GetCurrentProcess(),(SIZE_T)-1,(SIZE_T)-1)!=FALSE;PROCESS_MEMORY_COUNTERS before{},after{};GetProcessMemoryInfo(GetCurrentProcess(),&before,sizeof(before));begin=perf::Clock();output=e.Export();trimmed.ms.push_back(perf::Clock()-begin);GetProcessMemoryInfo(GetCurrentProcess(),&after,sizeof(after));faults.push_back(after.PageFaultCount-before.PageFaultCount);}
        rows.push_back(std::move(warm));rows.push_back(std::move(trimmed));}
    std::printf("{\"scope\":\"production CPU effect/export before/after components against frozen1fdf0a7, not UI input-to-visible latency\",\"exact_image_equality\":%s,\"equality_cases\":%d,\"max_logical_cpus\":4,\"priority\":\"below_normal\",\"own_ws_trim_succeeded\":%s,\"own_ws_trim_next_export_page_faults\":[",exact?"true":"false",equalityCases,trims?"true":"false");for(size_t i=0;i<faults.size();++i)std::printf("%s%lu",i?",":"",faults[i]);std::printf("],\"rows\":[");for(size_t i=0;i<rows.size();++i){if(i)std::putchar(',');auto& r=rows[i];perf::Print(r.name,r.w,r.h,r.ms);}std::printf("]}\n");CoUninitialize();return exact?0:1;
}
