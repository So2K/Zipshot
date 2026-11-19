#pragma once
#include "cursor_follow.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cursor_video {
struct Track {
    int width = 0, height = 0;
    double fps = 30, duration = 0;
    std::vector<cursor_follow::Sample> samples;
    uint64_t sourceBytes = 0;
    std::string sourceSha256;
};
struct Result { bool ok = false; std::wstring error; };

// A freshly saved track and its immutable source share one verified read handle.
// Keeping this context alive prevents writes/deletion until automatic rendering ends.
class PreparedSource {
public:
    PreparedSource();
    ~PreparedSource();
    PreparedSource(PreparedSource&&) noexcept;
    PreparedSource& operator=(PreparedSource&&) noexcept;
    PreparedSource(const PreparedSource&) = delete;
    PreparedSource& operator=(const PreparedSource&) = delete;
private:
    struct State;
    std::unique_ptr<State> state_;
    friend Result SaveTrackPrepared(const std::wstring&,int,int,double,double,
                                   const std::vector<cursor_follow::Sample>&,PreparedSource&);
    friend Result RenderPrepared(const std::wstring&,const PreparedSource&,const std::wstring&,bool);
};

std::wstring TrackPath(const std::wstring& original);
Result SaveTrack(const std::wstring& original, int width, int height, double fps, double duration,
                 const std::vector<cursor_follow::Sample>& samples);
Result SaveTrackPrepared(const std::wstring& original, int width, int height, double fps, double duration,
                         const std::vector<cursor_follow::Sample>& samples, PreparedSource& out);
bool LoadTrack(const std::wstring& original, Track& out, std::wstring* error = nullptr);
// Background-only work. Never queries the system cursor or alters the input file.
// Publishes a completed new MP4 without replacing any existing destination; keeps input audio.
Result Render(const std::wstring& ffmpeg, const std::wstring& original, const Track& track,
              const std::wstring& out, bool forceAllFollow = false);
Result RenderPrepared(const std::wstring& ffmpeg, const PreparedSource& source,
                      const std::wstring& out, bool forceAllFollow = false);
}  // namespace cursor_video
