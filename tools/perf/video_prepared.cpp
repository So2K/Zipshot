// Benchmark-only retained immutable source context; production APIs stay unchanged.
#include "../../src/common.h"
#include <chrono>
#include <filesystem>
#include <cstdio>
bool IsEnglish(){return true;}
#ifdef ZIPSHOT_PERF_CURRENT_API
#include "../../src/cursor_video.cpp"
#else
#include "../../build/perf/video/video_prepared_engine.inc"
#endif
int wmain(int argc,wchar_t** argv){
 if(argc!=4)return 2;
 const auto root=cursor_video::Full(argv[1]),ffmpeg=cursor_video::Full(argv[2]),fixture=cursor_video::Full(argv[3]);
 std::filesystem::create_directories(root);
 const std::vector<cursor_follow::Sample> samples={{0,.2,.2,false,true,false},{.3,.2,.2,true,true,false},{1.5,.8,.7,true,true,false},{3,.8,.7,false,true,false},{4,.8,.7,false,true,false}};
 const bool order[]={false,true,true,false,false,true,true,false};
 std::printf("{\"scope\":\"Benchmark-only immutable HANDLE SHA reuse shim in unchanged API pipeline. Includes SaveTrack, LoadTrack, Render and validation.\",\"runs\":[");bool first=true;
 for(int rep=0;rep<8;++rep){
  const bool prepared=order[rep];const auto stem=root+L"\\run-"+std::to_wstring(rep),source=stem+L".mp4",target=stem+L"-focus.mp4";
  if(!CreateHardLinkW(source.c_str(),fixture.c_str(),nullptr))return 3;
  auto start=std::chrono::steady_clock::now();
#ifdef ZIPSHOT_PERF_CURRENT_API
  cursor_video::PreparedSource context;
  auto saved=prepared?cursor_video::SaveTrackPrepared(source,640,360,30,4,samples,context):cursor_video::SaveTrack(source,640,360,30,4,samples);auto savedAt=std::chrono::steady_clock::now();
  cursor_video::Track track;const bool loaded=prepared||cursor_video::LoadTrack(source,track);auto loadedAt=std::chrono::steady_clock::now();
  if(!saved.ok||!loaded)return 5;
  auto rendered=prepared?cursor_video::RenderPrepared(ffmpeg,context,target):cursor_video::Render(ffmpeg,source,track,target);auto ended=std::chrono::steady_clock::now();
  cursor_video::Track savedTrack;if(!cursor_video::LoadTrack(source,savedTrack))return 7;track=std::move(savedTrack);
#else
  cursor_video::PerfPrepared context;
  cursor_video::Handle hold(prepared?CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr):INVALID_HANDLE_VALUE);
  if(prepared){if(!hold)return 4;context.held=hold.h;cursor_video::perfContext=&context;}
  auto saved=cursor_video::SaveTrack(source,640,360,30,4,samples);auto savedAt=std::chrono::steady_clock::now();
  cursor_video::Track track;const bool loaded=cursor_video::LoadTrack(source,track);auto loadedAt=std::chrono::steady_clock::now();
  if(!saved.ok||!loaded)return 5;auto rendered=cursor_video::Render(ffmpeg,source,track,target);auto ended=std::chrono::steady_clock::now();cursor_video::perfContext=nullptr;
#endif
  if(!rendered.ok)return 6;
  auto ms=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
  if(!first)std::printf(",");first=false;
  std::printf("{\"rep\":%d,\"prepared\":%s,\"save_ms\":%.6f,\"load_ms\":%.6f,\"render_ms\":%.6f,\"total_ms\":%.6f,\"source_sha256\":\"%s\"}",rep,prepared?"true":"false",ms(start,savedAt),ms(savedAt,loadedAt),ms(loadedAt,ended),ms(start,ended),track.sourceSha256.c_str());
 }
 std::printf("]}\n");return 0;
}
