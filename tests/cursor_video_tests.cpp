// Generated lavfi video + supplied cursor samples only. No GUI, desktop or cursor access.
#include "../src/common.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <thread>
#include <limits>
bool IsEnglish() { return true; }
static bool forceNoProbe = false;
static DWORD WINAPI TestGetFileAttributesW(LPCWSTR path) {
    std::wstring value=path;
    if(forceNoProbe && value.size()>=11 && !_wcsicmp(value.c_str()+value.size()-11,L"ffprobe.exe"))return INVALID_FILE_ATTRIBUTES;
    return GetFileAttributesW(path);
}
static DWORD WINAPI TestSearchPathW(LPCWSTR path,LPCWSTR file,LPCWSTR ext,DWORD size,LPWSTR buffer,LPWSTR* part) {
    if(forceNoProbe && !_wcsicmp(file,L"ffprobe.exe"))return 0;
    return SearchPathW(path,file,ext,size,buffer,part);
}
#define GetFileAttributesW TestGetFileAttributesW
#define SearchPathW TestSearchPathW
#include "../src/cursor_video.cpp"
#undef GetFileAttributesW
#undef SearchPathW

static int failures=0;
static void Check(bool pass,const char* text){std::printf("%s %s\n",pass?"PASS":"FAIL",text);if(!pass)++failures;}
static std::string Bytes(const std::wstring& path){std::ifstream file(std::filesystem::path(path),std::ios::binary);return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
static void Write(const std::wstring& path,const std::string& value){std::ofstream file(std::filesystem::path(path),std::ios::binary);file<<value;}
static size_t Leftovers(const std::wstring& root){size_t n=0;for(const auto& file:std::filesystem::directory_iterator(root))if(file.path().filename().wstring().starts_with(L".zipshot-cursor-"))++n;return n;}
static std::string Frame(const std::wstring& ffmpeg,const std::wstring& file,int frame,const std::wstring& extra=L""){
    std::wstring filter=L"select=eq(n\\,"+std::to_wstring(frame)+L")"+extra;
    auto result=cursor_video::Run(ffmpeg,{L"-v",L"error",L"-threads",L"1",L"-i",file,L"-vf",filter,L"-fps_mode",L"passthrough",L"-frames:v",L"1",L"-f",L"rawvideo",L"-pix_fmt",L"rgb24",L"-"},{},30000);
    return result.ok?result.text:std::string{};
}
static double Difference(const std::string& a,const std::string& b){if(a.empty()||a.size()!=b.size())return 1000;double sum=0;for(size_t i=0;i<a.size();++i)sum+=std::abs((int)(unsigned char)a[i]-(int)(unsigned char)b[i]);return sum/a.size();}

int wmain(int argc,wchar_t** argv){
    if(argc!=3)return 2;
    auto ffmpeg=cursor_video::Full(argv[2]);auto root=cursor_video::Full(argv[1])+L"\\данные ' [x],";
    std::filesystem::create_directories(root);
    auto original=root+L"\\источник's clip, [1].mp4";
    auto fixture=cursor_video::Run(ffmpeg,{L"-v",L"error",L"-y",L"-f",L"lavfi",L"-i",L"testsrc2=size=640x360:rate=30:duration=3",L"-an",L"-c:v",L"libx264",L"-preset",L"ultrafast",L"-crf",L"0",L"-threads",L"4",original},{},30000);
    Check(fixture.ok && cursor_video::Exists(original),"synthetic moving silent fixture generated under Unicode/punctuation paths");
    if(!fixture.ok)return 1;
    auto sourceBytes=Bytes(original);
    cursor_video::Track untouched;untouched.width=77;std::wstring error;
    Check(!cursor_video::LoadTrack(original,untouched,&error)&&!error.empty()&&untouched.width==77,"legacy video has a clear no-track error, without inventing samples");
    std::vector<cursor_follow::Sample> samples={{0,.2,.5,false,true,false},{.5,.2,.5,true,true,false},{1.0,.8,.5,true,true,false},{1.8,.8,.5,false,true,false},{2.5,.8,.5,false,true,false}};
    auto saved=cursor_video::SaveTrack(original,640,360,30,2.5,samples);
    Check(saved.ok,"sidecar saved atomically beside the original");
    cursor_video::Track track;
    Check(cursor_video::LoadTrack(original,track,&error)&&track.samples.size()==samples.size()&&track.width==640&&track.height==360&&track.sourceBytes==sourceBytes.size(),"strict JSON sidecar roundtrip preserves samples and source identity");
    std::string sidecar=Bytes(cursor_video::TrackPath(original));
    Check(!cursor_video::SaveTrack(original,640,360,30,2.5,samples).ok&&Bytes(cursor_video::TrackPath(original))==sidecar,"existing track is never overwritten");
    auto invalid=samples;invalid[1].x=std::numeric_limits<double>::quiet_NaN();
    Check(!cursor_video::SaveTrack(original,640,360,30,2.5,invalid).ok,"nonfinite sample data is rejected");
    auto output=root+L"\\result's focus, [2].mp4";
    auto result=cursor_video::Render(ffmpeg,original,track,output);
    if(!result.ok)std::fwprintf(stderr,L"Render error: %s\n",result.error.c_str());
    Check(result.ok,"off/on/off dynamic crop render succeeds without invented cursor pixels");
    cursor_video::Info before,after;
    bool metadata=cursor_video::Probe(ffmpeg,original,before)&&cursor_video::Probe(ffmpeg,output,after);
    Check(metadata&&before.width==after.width&&before.height==after.height,"output preserves original dimensions");
    Check(metadata&&std::abs(before.duration-after.duration)<.1&&before.frames==after.frames&&after.frames==90,"shorter 2.5s track does not trim the 3s/90-frame input tail");
    Check(metadata&&before.fps==after.fps&&after.fps==30,"output preserves frame cadence");
    auto path=cursor_follow::BuildPath(samples,track.duration,30);
    auto camera=path[36];int w=(int)std::floor(camera.width*640/2)*2,h=(int)std::floor(camera.height*360/2)*2;
    int x=std::clamp((int)std::floor(camera.left*640/2)*2,0,640-w),y=std::clamp((int)std::floor(camera.top*360/2)*2,0,360-h);
    auto actual=Frame(ffmpeg,output,36);auto full=Frame(ffmpeg,original,36);
    auto expected=Frame(ffmpeg,original,36,L",crop="+std::to_wstring(w)+L":"+std::to_wstring(h)+L":"+std::to_wstring(x)+L":"+std::to_wstring(y)+L",scale=640:360");
    double expectedError=Difference(actual,expected),fullError=Difference(actual,full);
    std::printf("camera expected MAE %.3f, full-frame MAE %.3f\n",expectedError,fullError);
    Check(expectedError<15&&expectedError<fullError*.5,"rendered middle frame follows the supplied camera crop and position");
    Check(Difference(Frame(ffmpeg,output,0),Frame(ffmpeg,original,0))<5,"initial FollowOff frame remains full-frame");
    Check(Bytes(original)==sourceBytes,"original video bytes remain unchanged after rendering");
    auto oldOutput=Bytes(output);
    Check(!cursor_video::Render(ffmpeg,original,track,output).ok&&Bytes(output)==oldOutput,"existing output is not overwritten");
    auto wrong=track;wrong.width=320;
    Check(!cursor_video::Render(ffmpeg,original,wrong,root+L"\\wrong.mp4").ok&&!cursor_video::Exists(root+L"\\wrong.mp4"),"real video dimensions are checked against the track");
    auto changed=root+L"\\changed.mp4";Write(changed,sourceBytes+"changed");
    Check(!cursor_video::Render(ffmpeg,changed,track,root+L"\\bad.mp4").ok,"source identity mismatch is rejected before export");
    auto allOff=track;for(auto& sample:allOff.samples)sample.enabled=false;
    auto forced=root+L"\\forced.mp4";
    Check(cursor_video::Render(ffmpeg,original,allOff,forced,true).ok&&Difference(Frame(ffmpeg,forced,36),full)>15,"forceAllFollow enables recorded positions without changing source track flags");
    Check(!allOff.samples[0].enabled,"forceAllFollow does not mutate caller track");
    auto legacy=root+L"\\legacy.mp4";Write(legacy,sourceBytes);Write(cursor_video::TrackPath(legacy),sidecar+" trailing");
    Check(!cursor_video::LoadTrack(legacy,untouched,&error),"trailing/corrupt JSON is rejected");
    Check(!cursor_video::Render(root+L"\\missing-ffmpeg.exe",original,track,root+L"\\missing.mp4").ok,"missing FFmpeg returns a clear failure");
    forceNoProbe=true;
    auto ffmpegOnly=root+L"\\ffmpeg-only.mp4";auto fallback=cursor_video::Render(ffmpeg,original,track,ffmpegOnly);
    forceNoProbe=false;
    if(!fallback.ok)std::fwprintf(stderr,L"FFmpeg-only fallback error: %s\n",fallback.error.c_str());
    Check(fallback.ok && cursor_video::Probe(ffmpeg,ffmpegOnly,after) && after.frames==90,"real FFmpeg metadata fallback works when no ffprobe is discoverable");

    auto audioSource=root+L"\\synthetic-audio-private-test.mp4";
    auto audioFixture=cursor_video::Run(ffmpeg,{L"-v",L"error",L"-y",L"-f",L"lavfi",L"-i",L"testsrc2=size=320x180:rate=30:duration=3",L"-f",L"lavfi",L"-i",L"sine=frequency=880:sample_rate=48000:duration=3",L"-map",L"0:v:0",L"-map",L"1:a:0",L"-c:v",L"libx264",L"-preset",L"ultrafast",L"-crf",L"0",L"-threads",L"4",L"-c:a",L"aac",L"-b:a",L"96k",audioSource},{},30000);
    Check(audioFixture.ok,"synthetic AAC/sine fixture generated only in ignored test output, without playback");
    auto audioOriginal=Bytes(audioSource);
    auto audioSaved=cursor_video::SaveTrack(audioSource,320,180,30,3,samples);cursor_video::Track audioTrack;
    bool audioLoaded=cursor_video::LoadTrack(audioSource,audioTrack);auto audioOut=root+L"\\audio-preserved-private-test.mp4";
    auto audioRender=audioLoaded?cursor_video::Render(ffmpeg,audioSource,audioTrack,audioOut):cursor_video::Result{};
    cursor_video::Info audioBefore,audioAfter;
    Check(audioSaved.ok&&audioRender.ok&&cursor_video::Probe(ffmpeg,audioSource,audioBefore)&&cursor_video::Probe(ffmpeg,audioOut,audioAfter)&&audioBefore.audio==1&&audioAfter.audio==1,"audio stream is preserved alongside the processed video");
    auto originalAAC=cursor_video::Run(ffmpeg,{L"-v",L"error",L"-i",audioSource,L"-map",L"0:a:0",L"-c:a",L"copy",L"-f",L"adts",L"-"},{},30000);
    auto processedAAC=cursor_video::Run(ffmpeg,{L"-v",L"error",L"-i",audioOut,L"-map",L"0:a:0",L"-c:a",L"copy",L"-f",L"adts",L"-"},{},30000);
    Check(originalAAC.ok&&processedAAC.ok&&!originalAAC.text.empty()&&originalAAC.text==processedAAC.text&&Bytes(audioSource)==audioOriginal,"AAC packet bytes and original audio/video source remain unchanged");
    Check(Leftovers(root)==0,"all completed/failed exports clean only their owned temporary paths");
    return failures?1:0;
}
