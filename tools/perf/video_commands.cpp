// Original camera model, unchanged rounding. Produces baseline and changed-scalar-only controls.
#include "../../src/cursor_follow.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <cstdio>

int wmain(int argc,wchar_t** argv){
    if(argc<4)return 2;
    int width=_wtoi(argv[1]),height=_wtoi(argv[2]);std::filesystem::path dir=argv[3];
    double duration=argc>4?_wtof(argv[4]):4,fps=argc>5?_wtof(argv[5]):30;
    std::filesystem::create_directories(dir);
    std::vector<cursor_follow::Sample> samples={{0,.2,.3,false,true,false},{.3,.2,.3,true,true,false},{1.2,.8,.7,true,true,false},{2.2,.3,.8,true,true,false},{3.2,.3,.8,false,true,false},{4,.3,.8,false,true,false}};
    if(duration>4){samples.clear();for(double t=0;t<=duration;t+=.1)samples.push_back({t,.5+.45*std::sin(t*.8),.5+.4*std::cos(t*.57),true,true,false});}
    auto cameras=cursor_follow::BuildPath(samples,duration,fps);
    std::ofstream all(dir/L"all.cmd"),delta(dir/L"delta.cmd"),path(dir/L"camera-path.csv");
    all.imbue(std::locale::classic());delta.imbue(std::locale::classic());all<<std::fixed<<std::setprecision(9);delta<<std::fixed<<std::setprecision(9);
    path<<"time,w,h,x,y\n";int ow=-1,oh=-1,ox=-1,oy=-1,na=0,nd=0,intervals=0;
    for(const auto& camera:cameras){
        int w=std::clamp((int)std::floor(camera.width*width/2)*2,2,width),h=std::clamp((int)std::floor(camera.height*height/2)*2,2,height);
        if(camera.width>=1-1e-9)w=width;if(camera.height>=1-1e-9)h=height;
        int x=std::clamp((int)std::floor(camera.left*width/2)*2,0,width-w),y=std::clamp((int)std::floor(camera.top*height/2)*2,0,height-h);
        path<<std::setprecision(17)<<camera.t<<','<<w<<','<<h<<','<<x<<','<<y<<'\n';
        if(w==ow&&h==oh&&x==ox&&y==oy)continue;
        all<<camera.t<<" crop@camera w "<<w<<", crop@camera h "<<h<<", crop@camera x "<<x<<", crop@camera y "<<y<<";\n";na+=4;++intervals;
        delta<<camera.t<<' ';bool first=true;
        auto emit=[&](char name,int value,bool changed){if(!changed)return;if(!first)delta<<", ";first=false;delta<<"crop@camera "<<name<<' '<<value;++nd;};
        emit('w',w,w!=ow);emit('h',h,h!=oh);emit('x',x,x!=ox);emit('y',y,y!=oy);delta<<";\n";
        ow=w;oh=h;ox=x;oy=y;
    }
    std::printf("{\"intervals\":%d,\"baseline_commands\":%d,\"delta_commands\":%d}\n",intervals,na,nd);return 0;
}
