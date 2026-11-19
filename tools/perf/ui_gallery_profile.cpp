// Actual production RebuildView on an owned hidden HWND; synthetic metadata only.
// The functional harness supplies no-input/clipboard/activation/backend boundaries.
#define wmain UiGalleryFunctionalMain
#include "../../tests/mainwindow_gallery_tests.cpp"
#undef wmain
#include "ui_gallery_baseline.h"
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <string_view>

namespace perf {
volatile size_t sink = 0;
std::vector<size_t> newest, oldest;
double Clock() { LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f); return q.QuadPart * 1000.0 / f.QuadPart; }
template<class F> std::vector<double> Times(F operation, int count = 15) {
    std::vector<double> result;
    for (int i = -3; i < count; ++i) { const double before = Clock(); operation(); const double ms = Clock() - before; if (i >= 0) result.push_back(ms); }
    return result;
}
void Print(const char* name, const std::vector<double>& values) {
    auto sorted = values; std::sort(sorted.begin(), sorted.end());
    std::printf("{\"case\":\"%s\",\"median_ms\":%.6f,\"p95_ms\":%.6f,\"ms\":[", name, sorted[sorted.size()/2], sorted[(size_t)std::ceil(sorted.size()*.95)-1]);
    for (size_t i=0;i<values.size();++i)std::printf("%s%.6f",i?",":"",values[i]);
    std::printf("]}");
}
std::vector<size_t> CachedSelect() {
    std::vector<size_t> result;
    const auto& order = g_mw->oldestFirst ? oldest : newest;
    result.reserve(order.size());
    for (size_t at : order) if (gallery::detail::Matches(g_mw->catalog[at],g_mw->query,g_mw->filter)) result.push_back(at);
    return result;
}
// Prototype only, not a production edit. Same item, selection and thumbnail rules.
void FastRebuild() {
    auto pathAt=[](int at)->std::wstring {return at>=0&&at<(int)g_mw->files.size()?g_mw->files[at].path:L"";};
    const auto selected=pathAt(g_mw->sel),anchor=pathAt(g_mw->anchor);
    std::unordered_map<std::wstring_view,Entry*> previous;previous.reserve(g_mw->files.size());
    for(auto& e:g_mw->files)previous.emplace(e.path,&e);
    const auto chosen=CachedSelect();std::vector<Entry> view;view.reserve(chosen.size());
    std::unordered_set<std::wstring_view> visible;visible.reserve(chosen.size());
    for(size_t at:chosen){Entry e;static_cast<gallery::Item&>(e)=g_mw->catalog[at];auto old=previous.find(e.path);
        if(old!=previous.end()&&old->second->time==e.time){std::swap(e.thumb,old->second->thumb);e.tw=old->second->tw;e.th=old->second->th;e.done=old->second->done;}
        view.push_back(std::move(e));}
    FreeAllThumbs();g_mw->files=std::move(view);g_mw->sel=g_mw->anchor=-1;
    for(int i=0;i<(int)g_mw->files.size();++i){auto& e=g_mw->files[i];visible.insert(e.path);if(e.path==selected)g_mw->sel=i;if(e.path==anchor)g_mw->anchor=i;}
    std::erase_if(g_mw->picked,[&](const std::wstring& p){return !visible.contains(p);});
    g_mw->hoverTile=g_mw->dropTile=g_mw->downTile=-1;g_mw->dragArmed=g_mw->collapseOnUp=g_mw->scrollDrag=false;
    if(GetCapture()==g_mw->hwnd)ReleaseCapture();ClampScroll();LayoutToolbar();InvalidateRect(g_mw->hwnd,nullptr,FALSE);
}
// Isolates only quadratic-selection pruning: the rest is the production method.
void HashPruneRebuild() {
    auto picked=std::move(g_mw->picked);g_mw->picked.clear();BaselineRebuildView();
    std::unordered_set<std::wstring_view> visible;visible.reserve(g_mw->files.size());
    for(const auto& e:g_mw->files)visible.insert(e.path);
    std::erase_if(picked,[&](const std::wstring& p){return !visible.contains(p);});
    g_mw->picked=std::move(picked);
}
std::string Signature() {
    uint64_t hash=1469598103934665603ULL;
    auto add=[&](const std::wstring& s){for(wchar_t c:s)hash=(hash^(uint16_t)c)*1099511628211ULL;hash=(hash^0xffff)*1099511628211ULL;};
    for(const auto& e:g_mw->files){add(e.path);add(e.name);hash=(hash^e.time)*1099511628211ULL;hash=(hash^(unsigned)e.kind)*1099511628211ULL;hash=(hash^e.shelf)*1099511628211ULL;}
    for(const auto& p:g_mw->picked)add(p);hash=(hash^(g_mw->sel+1))*1099511628211ULL;hash=(hash^(g_mw->anchor+1))*1099511628211ULL;
    return std::to_string(hash);
}
}

int wmain() {
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);if(!SetProcessAffinityMask(GetCurrentProcess(),0xf))return 2;
    OleInitialize(nullptr);g_inst=GetModuleHandleW(nullptr);
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=g_inst;wc.lpszClassName=L"ZipshotPerfGalleryOwned";RegisterClassW(&wc);
    const HWND foreground=GetForegroundWindow();
    g_mw=new State;g_mw->hwnd=CreateWindowExW(WS_EX_NOACTIVATE,wc.lpszClassName,L"Synthetic gallery profiler",WS_OVERLAPPEDWINDOW,0,0,1200,800,nullptr,nullptr,g_inst,nullptr);
    if(!g_mw->hwnd)return 3;CreateFonts();Measure();
    for(size_t i=0;i<10199;++i){gallery::Item e;e.name=(i%3?L"Снимок_":L"Capture_")+std::to_wstring(i)+(i%9==0?L".gif":i%13==0?L".mp4":L".png");e.path=L"C:\\synthetic-gallery-only\\2026-11\\"+e.name;e.time=(i*7919)%10201;e.kind=i%9==0?Kind::Gif:i%13==0?Kind::Video:Kind::Image;e.shelf=i%17==0;g_mw->catalog.push_back(e);}
    perf::newest=gallery::Select(g_mw->catalog,L"",gallery::Filter::All,false);perf::oldest=gallery::Select(g_mw->catalog,L"",gallery::Filter::All,true);
    bool equal=true;
    for(auto query:{L"",L"Снимок_",L"10198",L".GIF",L"missing"})for(auto filter:{gallery::Filter::All,gallery::Filter::Images,gallery::Filter::Gifs,gallery::Filter::Videos,gallery::Filter::Shelf})for(bool asc:{false,true}){
        g_mw->query=query;g_mw->filter=filter;g_mw->oldestFirst=asc;equal&=gallery::Select(g_mw->catalog,query,filter,asc)==perf::CachedSelect();}
    g_mw->query.clear();g_mw->filter=gallery::Filter::All;g_mw->oldestFirst=false;RebuildView();
    g_mw->sel=42;g_mw->anchor=23;g_mw->picked={g_mw->files[42].path,g_mw->files[23].path};BaselineRebuildView();const auto before=perf::Signature();perf::FastRebuild();equal&=before==perf::Signature();
    std::vector<std::pair<const char*,std::vector<double>>> rows;
    rows.push_back({"select_all_sort_each",perf::Times([]{auto v=gallery::Select(g_mw->catalog,L"",gallery::Filter::All,false);perf::sink=v.size();},30)});
    rows.push_back({"select_all_cached_order",perf::Times([]{auto v=perf::CachedSelect();perf::sink=v.size();},30)});
    rows.push_back({"rebuild_no_selection_baseline",perf::Times([]{g_mw->picked.clear();BaselineRebuildView();})});
    rows.push_back({"rebuild_no_selection_prototype",perf::Times([]{g_mw->picked.clear();perf::FastRebuild();})});
    g_mw->picked.clear();for(const auto& e:g_mw->files)g_mw->picked.push_back(e.path);
    rows.push_back({"rebuild_10199_selected_baseline",perf::Times([]{BaselineRebuildView();},10)});
    const auto pruneBefore=perf::Signature();perf::HashPruneRebuild();equal&=pruneBefore==perf::Signature();
    rows.push_back({"rebuild_10199_selected_hash_prune_only",perf::Times([]{perf::HashPruneRebuild();},10)});
    const auto selectedBefore=perf::Signature();perf::FastRebuild();equal&=selectedBefore==perf::Signature();
    rows.push_back({"rebuild_10199_selected_prototype",perf::Times([]{perf::FastRebuild();},10)});
    g_mw->query=L"Снимок_";
    rows.push_back({"broad_query_sort_each",perf::Times([]{auto v=gallery::Select(g_mw->catalog,g_mw->query,g_mw->filter,false);perf::sink=v.size();},30)});
    rows.push_back({"broad_query_cached_order",perf::Times([]{auto v=perf::CachedSelect();perf::sink=v.size();},30)});
    for(int scenario=0;scenario<3;++scenario){
        std::vector<double> baseline,optimized;
        auto prepare=[&]{g_mw->query.clear();g_mw->filter=gallery::Filter::All;g_mw->picked.clear();BaselineRebuildView();if(scenario)for(const auto& e:g_mw->files)g_mw->picked.push_back(e.path);if(scenario==2)g_mw->query=L"Снимок_";};
        for(int i=-3;i<20;++i){double a=0,b=0;std::string oldSignature,newSignature;
            auto old=[&]{prepare();double begin=perf::Clock();BaselineRebuildView();a=perf::Clock()-begin;oldSignature=perf::Signature();};
            auto fast=[&]{prepare();double begin=perf::Clock();RebuildView();b=perf::Clock()-begin;newSignature=perf::Signature();};
            if(i%2){fast();old();}else{old();fast();}equal&=oldSignature==newSignature;if(i>=0){baseline.push_back(a);optimized.push_back(b);}}
        rows.push_back({scenario==0?"paired_none_before":scenario==1?"paired_allpicked_before":"paired_unicode_filter_before",std::move(baseline)});
        rows.push_back({scenario==0?"paired_none_after":scenario==1?"paired_allpicked_after":"paired_unicode_filter_after",std::move(optimized)});
    }
    std::printf("{\"scope\":\"component CPU on owned hidden HWND, not input-to-visible latency\",\"items\":%zu,\"semantic_equality\":%s,\"foreground_unchanged\":%s,\"max_logical_cpus\":4,\"priority\":\"below_normal\",\"rows\":[",g_mw->catalog.size(),equal?"true":"false",GetForegroundWindow()==foreground?"true":"false");
    for(size_t i=0;i<rows.size();++i){if(i)std::putchar(',');perf::Print(rows[i].first,rows[i].second);}std::printf("]}\n");
    const HWND hwnd=g_mw->hwnd;FreeAllThumbs();DeleteObject(g_mw->menuFont);DeleteObject(g_mw->tableFont);DeleteObject(g_mw->badgeFont);delete g_mw;g_mw=nullptr;DestroyWindow(hwnd);OleUninitialize();return equal?0:1;
}
