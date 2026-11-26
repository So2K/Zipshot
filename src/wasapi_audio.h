#pragma once
#include <atomic>
#include <memory>
#include <string>

namespace wasapi_audio {

enum class Mode { None, System, Mic, Both };

Mode ParseMode(const std::wstring& audioSource);
const wchar_t* ModeToSetting(Mode m);
bool IsWasapiSetting(const std::wstring& audioSource);

// Records 48 kHz stereo 16-bit PCM WAV from Windows WASAPI loopback (system audio)
// and/or default microphone, driven by a steady QPC clock so silence stays in exact A/V sync.
class Recorder {
public:
    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Starts recording into `wavPath`. `leadSilenceMs` optionally prepends silence (e.g. if unmuted mid-segment).
    bool Start(const std::wstring& wavPath, Mode mode, bool startMuted = false, uint32_t leadSilenceMs = 0);
    void SetMuted(bool muted);
    bool Muted() const;
    bool Active() const;
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wasapi_audio
