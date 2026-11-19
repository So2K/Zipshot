#include "cursor_video.h"
#include "common.h"
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <locale>
#include <regex>
#include <sstream>

namespace cursor_video {
namespace {
constexpr size_t MaxTrackBytes = 128ull * 1024 * 1024;
constexpr size_t MaxSamples = 2000000;
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : h(v) {}
    ~Handle() { close(); }
    void close() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};
Result Error(const wchar_t* ru, const wchar_t* en) { return {false, Tr(ru, en)}; }
std::wstring Full(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    std::wstring out(n, L'\0');
    DWORD size = n ? GetFullPathNameW(path.c_str(), n, out.data(), nullptr) : 0;
    if (!size || size >= n) return {};
    out.resize(size); return out;
}
std::wstring Parent(const std::wstring& path) { size_t p = path.find_last_of(L"\\/"); return p == std::wstring::npos ? L"" : path.substr(0, p); }
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring Quote(const std::wstring& value) {
    std::wstring out = L"\""; size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        out.append(slashes * (ch == L'\"' ? 2 : 1), L'\\'); slashes = 0;
        if (ch == L'\"') out += L'\\';
        out += ch;
    }
    out.append(slashes * 2, L'\\'); return out + L"\"";
}
std::wstring Unique(const std::wstring& parent, const wchar_t* suffix) {
    static std::atomic<unsigned long long> serial{0};
    return parent + L"\\.zipshot-cursor-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++serial) + suffix;
}
bool WriteNew(const std::wstring& path, const std::string& bytes, bool* created = nullptr) {
    if (created) *created = false;
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return false;
    if (created) *created = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD count = (DWORD)std::min<size_t>(bytes.size() - offset, 1 << 20), written = 0;
        if (!WriteFile(file.h, bytes.data() + offset, count, &written, nullptr) || !written) return false;
        offset += written;
    }
    return FlushFileBuffers(file.h) != FALSE;
}
bool ReadSmall(const std::wstring& path, std::string& bytes) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (!file || !GetFileSizeEx(file.h, &size) || size.QuadPart < 0 || size.QuadPart > (LONGLONG)MaxTrackBytes) return false;
    bytes.resize((size_t)size.QuadPart); size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD got = 0;
        if (!ReadFile(file.h, bytes.data() + offset, (DWORD)std::min<size_t>(bytes.size()-offset,1<<20), &got, nullptr) || !got) return false;
        offset += got;
    }
    return true;
}
bool Identity(HANDLE file, uint64_t& bytes, std::string& digest) {
    LARGE_INTEGER size{}, zero{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &size) || size.QuadPart <= 0 || !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) return false;
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    BYTE buffer[65536];
    while (ok) {
        DWORD got = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        ok = CryptHashData(hash, buffer, got, 0) != FALSE;
    }
    BYTE value[32]; DWORD count = sizeof(value);
    ok = ok && CryptGetHashParam(hash, HP_HASHVAL, value, &count, 0) && count == 32;
    if (ok) {
        static const char hex[] = "0123456789abcdef"; digest.clear();
        for (BYTE x : value) { digest += hex[x >> 4]; digest += hex[x & 15]; }
        bytes = (uint64_t)size.QuadPart;
    }
    if (hash) CryptDestroyHash(hash); CryptReleaseContext(provider, 0); return ok;
}
bool Valid(const Track& track) {
    if (track.width < 2 || track.height < 2 || track.width > 32768 || track.height > 32768 ||
        !std::isfinite(track.fps) || track.fps < 1 || track.fps > 240 || !std::isfinite(track.duration) || track.duration <= 0 || track.duration > 86400 ||
        track.samples.empty() || track.samples.size() > MaxSamples || track.sourceSha256.size() != 64 || !track.sourceBytes) return false;
    if (!std::all_of(track.sourceSha256.begin(),track.sourceSha256.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) return false;
    double previous = -1;
    for (const auto& s : track.samples) {
        if (!std::isfinite(s.t) || !std::isfinite(s.x) || !std::isfinite(s.y) || s.t < 0 || s.t < previous) return false;
        previous = s.t;
    }
    return true;
}
// Fixed schema, strict numbers and booleans. No permissive regex extraction from corrupt JSON.
struct Reader {
    const char* p; const char* end;
    explicit Reader(const std::string& s) : p(s.data()), end(p+s.size()) {}
    void ws() { while(p!=end && (*p==' '||*p=='\r'||*p=='\n'||*p=='\t'))++p; }
    bool ch(char c) { ws(); if(p==end||*p!=c)return false;++p;return true; }
    bool key(const char* text) { ws(); std::string k="\""+std::string(text)+"\"";if((size_t)(end-p)<k.size()||std::memcmp(p,k.data(),k.size()))return false;p+=k.size();return ch(':'); }
    bool number(double& out) { ws();auto r=std::from_chars(p,end,out,std::chars_format::general);if(r.ec!=std::errc()||!std::isfinite(out))return false;p=r.ptr;return true; }
    bool integer(uint64_t& out) { ws();auto r=std::from_chars(p,end,out);if(r.ec!=std::errc())return false;p=r.ptr;return true; }
    bool boolean(bool& out) { ws();if(end-p>=4&&!std::memcmp(p,"true",4)){p+=4;out=true;return true;}if(end-p>=5&&!std::memcmp(p,"false",5)){p+=5;out=false;return true;}return false; }
    bool hash(std::string& out) { if(!ch('"')||end-p<65)return false;out.assign(p,64);p+=64;return ch('"'); }
};
std::string Encode(const Track& track) {
    std::ostringstream s; s.imbue(std::locale::classic()); s << std::setprecision(17);
    s << "{\"version\":1,\"width\":"<<track.width<<",\"height\":"<<track.height<<",\"fps\":"<<track.fps<<",\"duration\":"<<track.duration
      <<",\"source_bytes\":"<<track.sourceBytes<<",\"source_sha256\":\""<<track.sourceSha256<<"\",\"samples\":[";
    bool first=true;for(const auto& x:track.samples){if(!first)s<<',';first=false;s<<'['<<x.t<<','<<x.x<<','<<x.y<<','<<(x.enabled?"true":"false")<<','<<(x.visible?"true":"false")<<','<<(x.paused?"true":"false")<<']';}s<<"]}\n";return s.str();
}
bool Decode(const std::string& text, Track& out) {
    Reader r(text); Track t; uint64_t version=0,w=0,h=0;
    if(!r.ch('{')||!r.key("version")||!r.integer(version)||version!=1||!r.ch(',')||!r.key("width")||!r.integer(w)||w>32768||!r.ch(',')||!r.key("height")||!r.integer(h)||h>32768||
       !r.ch(',')||!r.key("fps")||!r.number(t.fps)||!r.ch(',')||!r.key("duration")||!r.number(t.duration)||!r.ch(',')||!r.key("source_bytes")||!r.integer(t.sourceBytes)||
       !r.ch(',')||!r.key("source_sha256")||!r.hash(t.sourceSha256)||!r.ch(',')||!r.key("samples")||!r.ch('['))return false;
    t.width=(int)w;t.height=(int)h;r.ws();
    while(r.p!=r.end && *r.p!=']'){
        cursor_follow::Sample x;
        if(t.samples.size()>=MaxSamples||!r.ch('[')||!r.number(x.t)||!r.ch(',')||!r.number(x.x)||!r.ch(',')||!r.number(x.y)||!r.ch(',')||!r.boolean(x.enabled)||!r.ch(',')||!r.boolean(x.visible)||!r.ch(',')||!r.boolean(x.paused)||!r.ch(']'))return false;
        t.samples.push_back(x);r.ws();if(r.p!=r.end&&*r.p==']')break;if(!r.ch(','))return false;r.ws();if(r.p==r.end||*r.p==']')return false;
    }
    if(!r.ch(']')||!r.ch('}'))return false;r.ws();if(r.p!=r.end||!Valid(t))return false;out=std::move(t);return true;
}
struct ChildResult { bool ok=false; std::string text; };
ChildResult Run(const std::wstring& exe, const std::vector<std::wstring>& args, const std::wstring& cwd, DWORD timeout) {
    ChildResult result; SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE rd=nullptr,wr=nullptr;
    if(!CreatePipe(&rd,&wr,&sa,0))return result;Handle reader(rd),writer(wr);SetHandleInformation(rd,HANDLE_FLAG_INHERIT,0);
    Handle null(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
    STARTUPINFOW si{sizeof(si)};si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=null.h;si.hStdOutput=wr;si.hStdError=wr;
    std::wstring command=Quote(exe);for(const auto& arg:args)command+=L" "+Quote(arg);PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS,nullptr,cwd.empty()?nullptr:cwd.c_str(),&si,&pi))return result;
    Handle process(pi.hProcess),thread(pi.hThread);writer.close();ULONGLONG start=GetTickCount64();bool ended=false;
    for(;;){
        if(GetTickCount64()-start>timeout){TerminateProcess(pi.hProcess,1);WaitForSingleObject(pi.hProcess,5000);return result;}
        DWORD available=0;if(PeekNamedPipe(rd,nullptr,0,nullptr,&available,nullptr)&&available){char buffer[4096];DWORD got=0;if(ReadFile(rd,buffer,std::min<DWORD>(available,sizeof(buffer)),&got,nullptr)&&got){result.text.append(buffer,got);if(result.text.size()>2*1024*1024)result.text.erase(0,result.text.size()-2*1024*1024);continue;}}
        if(ended)break;
        ended=WaitForSingleObject(pi.hProcess,30)==WAIT_OBJECT_0;
    }
    DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);result.ok=code==0;return result;
}
double Number(const std::string& s) { double x=0;auto r=std::from_chars(s.data(),s.data()+s.size(),x);return r.ec==std::errc()&&std::isfinite(x)?x:0; }
double Rate(const std::string& s) { auto p=s.find('/');return p==std::string::npos?Number(s):(Number(s.substr(p+1))?Number(s.substr(0,p))/Number(s.substr(p+1)):0); }
struct Info { int width=0,height=0,audio=0;double fps=0,duration=0,start=0,videoStart=0;uint64_t frames=0;bool probed=false; };
std::wstring ProbeExe(const std::wstring& ffmpeg) {
    std::wstring local=Parent(ffmpeg)+L"\\ffprobe.exe";if(Exists(local))return local;
    DWORD n=SearchPathW(nullptr,L"ffprobe.exe",nullptr,0,nullptr,nullptr);if(!n)return {};
    std::wstring path(n+1,L'\0');DWORD len=SearchPathW(nullptr,L"ffprobe.exe",nullptr,(DWORD)path.size(),path.data(),nullptr);if(!len||len>=path.size())return {};path.resize(len);return path;
}
bool Probe(const std::wstring& ffmpeg,const std::wstring& file,Info& out) {
    std::wstring probe=ProbeExe(ffmpeg);
    if(!probe.empty()){
        auto result=Run(probe,{L"-v",L"error",L"-show_entries",L"stream=codec_type,width,height,r_frame_rate,avg_frame_rate,nb_frames,duration,start_time:format=duration,start_time",L"-of",L"default",file},{},120000);
        if(!result.ok)return false;
        std::istringstream lines(result.text);std::string line,section,type;Info stream;double formatDuration=0;
        while(std::getline(lines,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();
            if(line=="[STREAM]"){section="stream";stream={};type.clear();continue;}if(line=="[FORMAT]"){section="format";continue;}
            if(line=="[/STREAM]"){if(type=="video"&&!out.width){out.width=stream.width;out.height=stream.height;out.fps=stream.fps;out.duration=stream.duration;out.videoStart=stream.start;out.frames=stream.frames;}else if(type=="audio")++out.audio;continue;}
            auto p=line.find('=');if(p==std::string::npos)continue;std::string key=line.substr(0,p),value=line.substr(p+1);
            if(section=="stream"){if(key=="codec_type")type=value;else if(key=="width")stream.width=(int)Number(value);else if(key=="height")stream.height=(int)Number(value);else if(key=="r_frame_rate")stream.fps=Rate(value);else if(key=="avg_frame_rate"&&!stream.fps)stream.fps=Rate(value);else if(key=="nb_frames")stream.frames=(uint64_t)Number(value);else if(key=="duration")stream.duration=Number(value);else if(key=="start_time")stream.start=Number(value);}
            else if(section=="format"){if(key=="duration")formatDuration=Number(value);else if(key=="start_time")out.start=Number(value);}
        }
        if(!out.duration)out.duration=formatDuration;if(out.duration>86400&&out.start>0&&out.duration>out.start)out.duration-=out.start;
        out.probed=true;return out.width>0&&out.height>0&&out.duration>0;
    }
    // App installs may contain ffmpeg alone. This is a real one-frame decode, not guessed metadata.
    auto result=Run(ffmpeg,{L"-hide_banner",L"-i",file,L"-map",L"0:v:0",L"-frames:v",L"1",L"-f",L"null",L"NUL"},{},120000);
    if(!result.ok)return false;
    std::smatch m;std::regex duration("Duration: ([0-9]+):([0-9]+):([0-9.]+)");
    if(std::regex_search(result.text,m,duration))out.duration=Number(m[1])*3600+Number(m[2])*60+Number(m[3]);
    std::istringstream lines(result.text);std::string line;bool input=true;
    while(std::getline(lines,line)){if(line.starts_with("Output #"))input=false;if(!input||line.find("Stream #")==std::string::npos)continue;
        if(line.find("Audio:")!=std::string::npos)++out.audio;
        if(!out.width&&line.find("Video:")!=std::string::npos){std::string video=line.substr(line.find("Video:")+6);std::regex dimensions(" ([1-9][0-9]{0,5})x([1-9][0-9]{0,5})(?:[ ,\\[])") ,fps("([0-9.]+) fps");if(std::regex_search(video,m,dimensions)){out.width=(int)Number(m[1]);out.height=(int)Number(m[2]);}if(std::regex_search(video,m,fps))out.fps=Number(m[1]);}
    }
    return out.width>0&&out.height>0&&out.duration>0;
}
struct Workspace {
    std::wstring dir,temp;bool published=false,tempOwned=false,commandsOwned=false,filterOwned=false;
    ~Workspace(){if(tempOwned&&!published)DeleteFileW(temp.c_str());if(commandsOwned)DeleteFileW((dir+L"\\camera.cmd").c_str());if(filterOwned)DeleteFileW((dir+L"\\camera-filter.txt").c_str());if(!dir.empty())RemoveDirectoryW(dir.c_str());}
};
} // namespace

struct PreparedSource::State {
    std::wstring source;
    Track track;
    Handle input;
};
PreparedSource::PreparedSource() = default;
PreparedSource::~PreparedSource() = default;
PreparedSource::PreparedSource(PreparedSource&&) noexcept = default;
PreparedSource& PreparedSource::operator=(PreparedSource&&) noexcept = default;

std::wstring TrackPath(const std::wstring& original) { return original+L".cursor.json"; }
Result SaveTrack(const std::wstring& original,int width,int height,double fps,double duration,const std::vector<cursor_follow::Sample>& samples) {
    PreparedSource prepared;
    return SaveTrackPrepared(original,width,height,fps,duration,samples,prepared);
}
Result SaveTrackPrepared(const std::wstring& original,int width,int height,double fps,double duration,const std::vector<cursor_follow::Sample>& samples,PreparedSource& out) {
    out.state_.reset();
    try {
        auto state=std::make_unique<PreparedSource::State>();state->source=Full(original);
        Track& track=state->track;track.width=width;track.height=height;track.fps=fps;track.duration=duration;track.samples=samples;
        state->input.h=CreateFileW(state->source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(!state->input||!Identity(state->input.h,track.sourceBytes,track.sourceSha256))return Error(L"Не удалось прочитать исходное видео.",L"Could not read the original video.");
        if(!Valid(track))return Error(L"Некорректные данные трека курсора.",L"Invalid cursor track data.");
        const std::string encoded = Encode(track);
        if(encoded.size() > MaxTrackBytes)return Error(L"Трек курсора слишком большой. Исходное видео сохранено.",L"Cursor track is too large. The original video is retained.");
        std::wstring final=TrackPath(state->source);if(Exists(final))return Error(L"Трек курсора уже существует.",L"Cursor track already exists.");
        Workspace work;work.temp=Unique(Parent(state->source),L".json.tmp");
        if(!WriteNew(work.temp,encoded,&work.tempOwned)||!MoveFileExW(work.temp.c_str(),final.c_str(),MOVEFILE_WRITE_THROUGH))return Error(L"Не удалось сохранить трек курсора.",L"Could not save the cursor track.");
        work.published=true;out.state_=std::move(state);return {true,{}};
    }catch(...){return Error(L"Не удалось сохранить трек курсора.",L"Could not save the cursor track.");}
}
bool LoadTrack(const std::wstring& original,Track& out,std::wstring* error) {
    if(error)error->clear();
    try {
        std::wstring path=TrackPath(Full(original));
        if(!Exists(path)){if(error)*error=Tr(L"Нет трека курсора. Он записывается только для новых видео.",L"No cursor track. Only newly recorded videos contain cursor data.");return false;}
        std::string bytes;Track track;
        if(!ReadSmall(path,bytes)||!Decode(bytes,track)){if(error)*error=Tr(L"Трек курсора повреждён или имеет неподдерживаемую версию.",L"Cursor track is damaged or has an unsupported version.");return false;}
        out=std::move(track);return true;
    }catch(...){if(error)*error=Tr(L"Не удалось прочитать трек курсора.",L"Could not read the cursor track.");return false;}
}
namespace {
Result RenderVerified(const std::wstring& executable,const std::wstring& source,const Track& track,const std::wstring& output,bool forceAllFollow);
}
Result Render(const std::wstring& executable,const std::wstring& original,const Track& track,const std::wstring& output,bool forceAllFollow) {
    try {
        if(!Valid(track))return Error(L"Нет корректного трека курсора.",L"No valid cursor track.");
        const std::wstring source=Full(original);
        Handle input(CreateFileW(source.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr));uint64_t sourceBytes=0;std::string sourceHash;
        if(!input||!Identity(input.h,sourceBytes,sourceHash))return Error(L"Не удалось прочитать исходное видео.",L"Could not read the original video.");
        if(sourceBytes!=track.sourceBytes||sourceHash!=track.sourceSha256)return Error(L"Трек относится к другому или изменённому видео.",L"Track belongs to another or modified video.");
        return RenderVerified(executable,source,track,output,forceAllFollow);
    }catch(...){return Error(L"Не удалось обработать видео. Исходник сохранён.",L"Could not process the video. The original is unchanged.");}
}
Result RenderPrepared(const std::wstring& executable,const PreparedSource& prepared,const std::wstring& output,bool forceAllFollow) {
    if(!prepared.state_||!prepared.state_->input)return Error(L"Нет проверенного исходного видео.",L"No verified source video.");
    return RenderVerified(executable,prepared.state_->source,prepared.state_->track,output,forceAllFollow);
}
namespace {
Result RenderVerified(const std::wstring& executable,const std::wstring& source,const Track& track,const std::wstring& output,bool forceAllFollow) {
    try {
        if(!Valid(track))return Error(L"Нет корректного трека курсора.",L"No valid cursor track.");
        std::wstring ffmpeg=Full(executable),out=Full(output);
        if(ffmpeg.empty()||!Exists(ffmpeg))return Error(L"Не найден ffmpeg.exe.",L"ffmpeg.exe was not found.");
        if(out.empty()||Parent(out).empty()||Exists(out))return Error(L"Выберите новое имя выходного файла.",L"Choose a new output filename.");
        Info before;if(!Probe(ffmpeg,source,before))return Error(L"Не удалось проверить параметры исходного видео.",L"Could not verify the original video metadata.");
        if(before.width!=track.width||before.height!=track.height)return Error(L"Размер видео не совпадает с треком курсора.",L"Video dimensions do not match the cursor track.");
        double fps=before.fps>0&&before.fps<=240?before.fps:track.fps;
        if(track.duration*fps>2000000)return Error(L"Видео слишком длинное для этой обработки.",L"Video is too long for this operation.");
        auto samples=track.samples;if(forceAllFollow)for(auto& sample:samples)sample.enabled=true;
        auto cameras=cursor_follow::BuildPath(samples,track.duration,fps);
        if(cameras.empty())return Error(L"Трек курсора не содержит кадров.",L"Cursor track contains no frames.");
        Workspace work;work.dir=Unique(Parent(out),L".work");if(!CreateDirectoryW(work.dir.c_str(),nullptr)){work.dir.clear();return Error(L"Не удалось создать рабочую папку.",L"Could not create a working directory.");}
        std::ostringstream commands;commands.imbue(std::locale::classic());commands<<std::fixed<<std::setprecision(9);
        int oldW=-1,oldH=-1,oldX=-1,oldY=-1;
        for(const auto& camera:cameras){
            if(!std::isfinite(camera.left)||!std::isfinite(camera.top)||!std::isfinite(camera.width)||!std::isfinite(camera.height))return Error(L"Некорректная траектория камеры.",L"Invalid camera path.");
            int w=std::clamp((int)std::floor(camera.width*track.width/2)*2,2,track.width),h=std::clamp((int)std::floor(camera.height*track.height/2)*2,2,track.height);
            if(camera.width>=1-1e-9)w=track.width;if(camera.height>=1-1e-9)h=track.height;
            int x=std::clamp((int)std::floor(camera.left*track.width/2)*2,0,track.width-w),y=std::clamp((int)std::floor(camera.top*track.height/2)*2,0,track.height-h);
            if(w==oldW&&h==oldH&&x==oldX&&y==oldY)continue;
            double time=camera.t+std::max(0.0,before.videoStart-before.start);
            commands<<time<<" crop@camera w "<<w<<", crop@camera h "<<h<<", crop@camera x "<<x<<", crop@camera y "<<y<<";\n";
            oldW=w;oldH=h;oldX=x;oldY=y;
        }
        std::ostringstream filter;filter.imbue(std::locale::classic());filter<<"sendcmd=f=camera.cmd,crop@camera="<<track.width<<':'<<track.height<<":0:0,scale="<<track.width<<':'<<track.height<<":eval=frame";
        if(!WriteNew(work.dir+L"\\camera.cmd",commands.str(),&work.commandsOwned)||!WriteNew(work.dir+L"\\camera-filter.txt",filter.str(),&work.filterOwned))return Error(L"Не удалось подготовить обработку видео.",L"Could not prepare the video processing files.");
        work.temp=Unique(Parent(out),L".mp4");Handle reservation(CreateFileW(work.temp.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!reservation)return Error(L"Не удалось создать выходной файл.",L"Could not create an output file.");
        work.tempOwned=true;
        auto rendered=Run(ffmpeg,{L"-hide_banner",L"-loglevel",L"error",L"-y",L"-i",source,L"-filter_threads",L"1",L"-filter_script:v",L"camera-filter.txt",L"-map",L"0:v:0",L"-map",L"0:a?",L"-c:v",L"libx264",L"-preset",L"veryfast",L"-crf",L"18",L"-threads",L"4",L"-pix_fmt",(track.width%2||track.height%2)?L"yuv444p":L"yuv420p",L"-fps_mode",L"passthrough",L"-c:a",L"copy",L"-map_metadata",L"0",L"-movflags",L"+faststart",work.temp},work.dir,(DWORD)std::min(86400000.0,std::max(120000.0,before.duration*20000+30000)));
        reservation.close();
        if(!rendered.ok)return Error(L"FFmpeg не смог обработать видео. Исходник сохранён.",L"FFmpeg could not process the video. The original is unchanged.");
        Info after;if(!Probe(ffmpeg,work.temp,after)||after.width!=before.width||after.height!=before.height||
            std::abs(after.duration-before.duration)>std::max(.25,3.0/fps)||(before.fps>0&&after.fps>0&&std::abs(after.fps-before.fps)>std::max(.1,before.fps*.02))||(before.frames&&after.frames&&before.frames!=after.frames)||(before.probed&&after.probed&&before.audio!=after.audio))
            return Error(L"Проверка обработанного видео не пройдена. Исходник сохранён.",L"Processed-video validation failed. The original is unchanged.");
        if(!MoveFileExW(work.temp.c_str(),out.c_str(),MOVEFILE_WRITE_THROUGH))return Error(L"Не удалось сохранить новую копию: имя уже занято или папка недоступна.",L"Could not publish the new copy: filename exists or directory is unavailable.");
        work.published=true;return {true,{}};
    }catch(...){return Error(L"Не удалось обработать видео. Исходник сохранён.",L"Could not process the video. The original is unchanged.");}
}
} // namespace
} // namespace cursor_video
