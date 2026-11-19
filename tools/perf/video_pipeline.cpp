// Actual unchanged cursor-video API phases over a generated fixture. No desktop/cursor access.
#include "../../src/common.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <vector>
bool IsEnglish(){return true;}
#include "../../src/cursor_video.cpp"
using Clock=std::chrono::steady_clock;
static double Millis(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
int wmain(int argc,wchar_t** argv){
 if(argc!=3)return 2;
 auto root=cursor_video::Full(argv[1]),ffmpeg=cursor_video::Full(argv[2]);std::filesystem::create_directories(root);
 auto source=root+L"\\pipeline-source.mp4";
 auto made=cursor_video::Run(ffmpeg,{L"-v",L"error",L"-y",L"-f",L"lavfi",L"-i",L"testsrc2=size=640x360:rate=30:duration=4",L"-an",L"-c:v",L"libx264",L"-preset",L"ultrafast",L"-crf",L"0",L"-threads",L"4",source},{},30000);
 if(!made.ok)return 3;
 std::vector<cursor_follow::Sample> samples={{0,.2,.2,false,true,false},{.3,.2,.2,true,true,false},{1.5,.8,.7,true,true,false},{3,.8,.7,false,true,false},{4,.8,.7,false,true,false}};
 auto started=Clock::now();auto saved=cursor_video::SaveTrack(source,640,360,30,4,samples);double save=Millis(started);
 cursor_video::Track track;started=Clock::now();bool loaded=cursor_video::LoadTrack(source,track);double load=Millis(started);
 if(!saved.ok||!loaded)return 4;
 started=Clock::now();auto result=cursor_video::Render(ffmpeg,source,track,root+L"\\pipeline-focus.mp4");double render=Millis(started);
 if(!result.ok)return 5;
 std::printf("{\"save_track_ms\":%.6f,\"load_track_ms\":%.6f,\"render_api_ms\":%.6f,\"hash_controls\":[",save,load,render);
 auto large=root+L"\\synthetic-hash-128MiB.bin";
 {std::ofstream output(std::filesystem::path(large),std::ios::binary);std::vector<char> block(1<<20);for(size_t i=0;i<block.size();++i)block[i]=(char)((i*13+i/251)&255);for(int i=0;i<128;++i)output.write(block.data(),block.size());}
 bool first=true;
 for(int rep=0;rep<5;++rep){
  cursor_video::Handle file(CreateFileW(large.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
  uint64_t bytes=0;std::string digest;started=Clock::now();bool ok=cursor_video::Identity(file.h,bytes,digest);double ms=Millis(started);
  if(!ok)return 6;
  if(!first)std::printf(",");first=false;
  std::printf("{\"rep\":%d,\"bytes\":%llu,\"ms\":%.6f,\"sha256\":\"%s\"}",rep,(unsigned long long)bytes,ms,digest.c_str());
 }
 std::printf("]}\n");return 0;
}
