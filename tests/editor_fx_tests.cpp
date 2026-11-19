// Production editor effect/export functions and frozen pre-optimization reference.
// The reused brush harness intercepts host input/settings boundaries; no HWND exists.
#define main EditorBrushOriginalMain
#include "editor_brush_tests.cpp"
#undef main
#include "../tools/perf/ui_editor_reference.h"
#include <atomic>
#include <limits>
#include <thread>

namespace fx_test {
int assertions = 0, failures = 0;
std::atomic_int pixelCases{0};
void Check(bool ok, const char* message) { ++assertions; if (!ok) ++failures; std::printf("%s %s\n",ok?"PASS":"FAIL",message); }
Image Fixture(int w,int h,bool alpha=false) {
    Image image;image.w=w;image.h=h;image.alpha=alpha;image.px.resize((size_t)w*h);
    uint32_t random=0x68259a31;
    for(auto& p:image.px){random^=random<<13;random^=random>>17;random^=random<<5;p=(alpha?random&0xff000000:0xff000000)|(random&0xffffff);}
    return image;
}
bool Equal(const Image& a,const Image& b) {++pixelCases;return a.w==b.w&&a.h==b.h&&a.alpha==b.alpha&&a.px==b.px;}
bool PassReference(int w,int h,int radius,bool horizontal) {
    auto input=Fixture(w,h,true).px;input.push_back(0x12345678);const auto original=input;
    std::vector<uint32_t> expected(input.size(),0x81abcdef),actual=expected;
    LegacyBoxBlurPass(input,expected,w,h,radius,horizontal);
    BoxBlurPass(input,actual,w,h,radius,horizontal);
    ++pixelCases;return actual==expected&&input==original;
}
void BlurArithmetic() {
    for(auto size:std::vector<std::pair<int,int>>{{1,1},{1,4096},{4096,1},{17,19},{63,65},{64,64},{65,65},{321,199}}){
        bool equal=true;
        for(int radius:{0,1,5,11,33,256,257})for(bool horizontal:{false,true})equal&=PassReference(size.first,size.second,radius,horizontal);
        Check(equal,"blur passes preserve every old integer average, edge pixel, alpha byte and buffer tail");
    }
    for(auto size:std::vector<std::pair<int,int>>{{1,1},{65,67},{321,199}})for(bool alpha:{false,true})for(uint32_t strength:{0u,15u,35u,99u}){
        Editor current;LegacyEditor old;current.img=std::make_shared<Image>(Fixture(size.first,size.second,alpha));old.img=current.img;
        Check(Equal(current.FxImage(FX_Blur,strength),old.FxImage(FX_Blur,strength)),"complete cached blur matches all six old passes and Image metadata");
    }
    std::atomic_bool exact{true};std::thread one([&]{if(!PassReference(129,67,5,false))exact=false;});
    std::thread two([&]{if(!PassReference(129,67,33,false))exact=false;});one.join();two.join();
    Check(exact,"concurrent independent blur radii keep division tables thread-local");
}
void CompareExport(const Image& input,const std::vector<Shape>& shapes={},const D2D1_RECT_F* area=nullptr,ID2D1Geometry* mask=nullptr) {
    Editor current;LegacyEditor old;current.img=std::make_shared<Image>(input);old.img=current.img;current.shapes=old.shapes=shapes;
    // The legacy reference's actual effect cache is initialized with its old CPU path.
    for(const Shape& shape:shapes)if(shape.tool==T_Blur)old.FxImage(FX_Blur,(uint32_t)shape.size2);
    const Image expected = old.Export(area,mask), actual = current.Export(area,mask);
    Check(!expected.empty() && !actual.empty() && Equal(actual,expected),"export pixels, dimensions and transparency match the old renderer exactly");
}
void ExportPaths() {
    for(auto size:std::vector<std::pair<int,int>>{{1,1},{17,19},{65,67},{1001,573}}){Image opaque=Fixture(size.first,size.second);
        // Opaque source alpha is ignored by D2D, even when its unused bytes are0/partial.
        for(size_t i=0;i<opaque.px.size();++i)if(i%3==0)opaque.px[i]&=0xffffff;else if(i%3==1)opaque.px[i]=(opaque.px[i]&0xffffff)|0x80000000;
        CompareExport(opaque);
    }
    Image opaque=Fixture(65,67),transparent=Fixture(65,67,true);transparent.px[0]=0x00112233;transparent.px[1]=0x80123456;
    CompareExport(transparent);
    Image allTransparent=transparent;for(auto& pixel:allTransparent.px)pixel&=0xffffff;CompareExport(allTransparent);
    Image padded=opaque;padded.px.insert(padded.px.end(),7,0xdeadbeef);CompareExport(padded);
    for(D2D1_RECT_F area:std::vector<D2D1_RECT_F>{{64,66,65,67},{0,0,1,1},{1,1,64,66},{-2,-1,66,69},{.5f,.25f,64.5f,66.25f}}){CompareExport(opaque,{},&area);CompareExport(transparent,{},&area);}
    Shape crop{T_Crop};crop.pts={{1,1},{64,66}};CompareExport(opaque,{crop});
    crop.pts={{.5f,.25f},{64.5f,66.25f}};CompareExport(opaque,{crop});
    Shape rectangle{T_Rect};rectangle.pts={{2,3},{58,61}};rectangle.size=3;rectangle.color=Rgb(255,0,0);rectangle.color2=kClear;CompareExport(opaque,{rectangle});
    Shape stroke{T_Freehand};stroke.pts={{2,4},{30,30},{62,60}};stroke.color=Rgb(10,250,30);stroke.size=4;Editor::BuildGeo(stroke);CompareExport(transparent,{stroke});
    Shape blur{T_Blur};blur.pts={{0,0},{64,66}};blur.size2=35;CompareExport(opaque,{blur});
    ComPtr<ID2D1EllipseGeometry> ellipse;D2D()->CreateEllipseGeometry(D2D1::Ellipse({32.5f,33.5f},30,31),&ellipse);
    Check(ellipse.Get()!=nullptr,"ellipse mask fixture uses an actual Direct2D geometry");
    if(ellipse)CompareExport(opaque,{},nullptr,ellipse.Get());
    Editor invalid;Check(invalid.Export().empty(),"missing source image returns an empty export before dereference");
    invalid.img=std::make_shared<Image>();invalid.img->w=10;invalid.img->h=10;invalid.img->px={0xff112233};Check(invalid.Export().empty(),"short source buffer is rejected before renderer upload");
    invalid.img=std::make_shared<Image>(opaque);const D2D1_RECT_F nan{0,0,std::numeric_limits<float>::quiet_NaN(),1};Check(invalid.Export(&nan).empty(),"non-finite export crop is rejected");
    const D2D1_RECT_F huge{0,0,2147483648.0f,1};Check(invalid.Export(&huge).empty(),"unrepresentable crop dimension is rejected before integer conversion");
}
void CacheAndHistory() {
    Editor current;LegacyEditor old;auto first=std::make_shared<Image>(Fixture(129,97));auto second=std::make_shared<Image>(*first);for(auto& p:second->px)p^=0x00ffffff;
    current.img=old.img=first;const Image firstBlur=current.FxImage(FX_Blur,35);const Image* address=&current.FxImage(FX_Blur,35);
    Check(address==&current.FxImage(FX_Blur,35)&&current.fxCache.size()==1,"repeated blur reuses the original cache entry");
    current.g.fx[123]=nullptr;current.SetBitmap(second);old.SetBitmap(second);
    Check(current.fxCache.empty()&&current.g.fx.empty(),"replacing an image invalidates CPU and render-target effect caches");
    Check(Equal(current.FxImage(FX_Blur,35),old.FxImage(FX_Blur,35))&&current.FxImage(FX_Blur,35).px!=firstBlur.px,"blur after image replacement uses new pixels and exact old arithmetic");
    Shape blur{T_Blur};blur.pts={{0,0},{128,96}};blur.size2=35;current.shapes={blur};current.SetBitmap(first);current.Snapshot();current.SetBitmap(second);
    current.Export();current.Restore(current.undo,current.redo);
    LegacyEditor firstReference;firstReference.img=first;firstReference.shapes={blur};firstReference.FxImage(FX_Blur,35);
    Check(current.img==first&&Equal(current.Export(),firstReference.Export()),"undo restores original bitmap and recomputes its exact blur result");
    current.Restore(current.redo,current.undo);
    LegacyEditor secondReference;secondReference.img=second;secondReference.shapes={blur};secondReference.FxImage(FX_Blur,35);
    Check(current.img==second&&Equal(current.Export(),secondReference.Export()),"redo restores replacement bitmap and its exact blur result");
}
}
int wmain(){SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);if(!SetProcessAffinityMask(GetCurrentProcess(),0xf))return 2;std::setvbuf(stdout,nullptr,_IONBF,0);CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);fx_test::BlurArithmetic();fx_test::ExportPaths();fx_test::CacheAndHistory();CoUninitialize();std::printf("%d assertions, %d failures, %d pixel-equivalence cases\n",fx_test::assertions,fx_test::failures,fx_test::pixelCases.load());return fx_test::failures?1:0;}
