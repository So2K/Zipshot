#include "wasapi_audio.h"
#include "common.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace wasapi_audio {

Mode ParseMode(const std::wstring& s) {
    if (_wcsicmp(s.c_str(), L"wasapi:system") == 0) return Mode::System;
    if (_wcsicmp(s.c_str(), L"wasapi:mic") == 0) return Mode::Mic;
    if (_wcsicmp(s.c_str(), L"wasapi:both") == 0) return Mode::Both;
    return Mode::None;
}

const wchar_t* ModeToSetting(Mode m) {
    switch (m) {
    case Mode::System: return L"wasapi:system";
    case Mode::Mic: return L"wasapi:mic";
    case Mode::Both: return L"wasapi:both";
    default: return L"";
    }
}

bool IsWasapiSetting(const std::wstring& s) { return ParseMode(s) != Mode::None; }

namespace {

constexpr uint32_t kOutRate = 48000;
constexpr uint16_t kOutChannels = 2;

struct Endpoint {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    uint32_t srcRate = 48000;
    uint16_t srcChannels = 2;
    uint16_t bitsPerSample = 32;
    bool isFloat = true;
    std::deque<float> fifo;  // interleaved stereo @ 48 kHz
    double fracPos = 0.0;
    float lastL = 0.0f, lastR = 0.0f;

    bool Open(EDataFlow flow, bool loopback) {
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return false;
        ComPtr<IMMDevice> dev;
        if (FAILED(en->GetDefaultAudioEndpoint(flow, flow == eCapture ? eCommunications : eConsole, &dev))) {
            if (FAILED(en->GetDefaultAudioEndpoint(flow, eConsole, &dev))) return false;
        }
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client))) return false;
        WAVEFORMATEX* wf = nullptr;
        if (FAILED(client->GetMixFormat(&wf)) || !wf) return false;
        srcRate = wf->nSamplesPerSec ? wf->nSamplesPerSec : 48000;
        srcChannels = wf->nChannels ? wf->nChannels : 2;
        bitsPerSample = wf->wBitsPerSample;
        isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
            auto* ex = (const WAVEFORMATEXTENSIBLE*)wf;
            isFloat = IsEqualGUID(ex->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
        }
        DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        REFERENCE_TIME hnsBuf = 1000000;  // 100 ms buffer
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, hnsBuf, 0, wf, nullptr);
        CoTaskMemFree(wf);
        if (FAILED(hr)) return false;
        if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) return false;
        if (FAILED(client->Start())) return false;
        return true;
    }

    void Close() {
        if (client) client->Stop();
        capture.Reset();
        client.Reset();
    }

    void Pump() {
        if (!capture) return;
        UINT32 packetFrames = 0;
        while (SUCCEEDED(capture->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
            std::vector<float> stereo((size_t)frames * 2, 0.0f);
            if (!silent) {
                for (UINT32 i = 0; i < frames; ++i) {
                    auto sampleAt = [&](uint16_t ch) -> float {
                        size_t idx = (size_t)i * srcChannels + ch;
                        if (isFloat && bitsPerSample == 32) return ((const float*)data)[idx];
                        if (!isFloat && bitsPerSample == 16) return ((const int16_t*)data)[idx] / 32768.0f;
                        if (!isFloat && bitsPerSample == 32) return (float)(((const int32_t*)data)[idx] / 2147483648.0);
                        return 0.0f;
                    };
                    float l = sampleAt(0);
                    float r = srcChannels >= 2 ? sampleAt(1) : l;
                    if (srcChannels > 2) {
                        // Gently fold surround/center channels into stereo so 5.1/7.1 games aren't missing dialogue.
                        float extra = 0.0f;
                        for (uint16_t c = 2; c < srcChannels; ++c) extra += sampleAt(c);
                        extra *= 0.35f;
                        l += extra;
                        r += extra;
                    }
                    stereo[(size_t)i * 2] = l;
                    stereo[(size_t)i * 2 + 1] = r;
                }
            }
            capture->ReleaseBuffer(frames);

            if (srcRate == kOutRate) {
                fifo.insert(fifo.end(), stereo.begin(), stereo.end());
            } else if (frames > 0) {
                const double ratio = (double)srcRate / (double)kOutRate;
                while (fracPos < (double)frames) {
                    int i0 = (int)fracPos;
                    float t = (float)(fracPos - i0);
                    float l0 = i0 == 0 ? lastL : stereo[(size_t)(i0 - 1) * 2];
                    float r0 = i0 == 0 ? lastR : stereo[(size_t)(i0 - 1) * 2 + 1];
                    float l1 = stereo[(size_t)i0 * 2];
                    float r1 = stereo[(size_t)i0 * 2 + 1];
                    fifo.push_back(l0 + (l1 - l0) * t);
                    fifo.push_back(r0 + (r1 - r0) * t);
                    fracPos += ratio;
                }
                fracPos -= (double)frames;
                lastL = stereo[(size_t)(frames - 1) * 2];
                lastR = stereo[(size_t)(frames - 1) * 2 + 1];
            }
            // Cap FIFO at ~250 ms so clock skew never builds unbounded latency.
            const size_t maxFloats = (size_t)kOutRate * kOutChannels / 4;
            if (fifo.size() > maxFloats) fifo.erase(fifo.begin(), fifo.begin() + (fifo.size() - maxFloats));
        }
    }
};

#pragma pack(push, 1)
struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t chunkSize = 36;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmtSize = 16;
    uint16_t audioFormat = 1;  // PCM
    uint16_t channels = kOutChannels;
    uint32_t sampleRate = kOutRate;
    uint32_t byteRate = kOutRate * kOutChannels * 2;
    uint16_t blockAlign = kOutChannels * 2;
    uint16_t bitsPerSample = 16;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)

}  // namespace

struct Recorder::Impl {
    std::atomic_bool stop{false};
    std::atomic_bool active{false};
    std::atomic_bool muted{false};
    std::thread worker;
};

Recorder::Recorder() : impl_(std::make_unique<Impl>()) {}
Recorder::~Recorder() { Stop(); }

bool Recorder::Start(const std::wstring& wavPath, Mode mode, bool startMuted, uint32_t leadSilenceMs) {
    Stop();
    if (mode == Mode::None || wavPath.empty()) return false;
    impl_->stop.store(false);
    impl_->muted.store(startMuted);
    impl_->active.store(true);
    try {
        impl_->worker = std::thread([impl = impl_.get(), wavPath, mode, leadSilenceMs] {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Endpoint sys, mic;
            bool hasSys = (mode == Mode::System || mode == Mode::Both) && sys.Open(eRender, true);
            bool hasMic = (mode == Mode::Mic || mode == Mode::Both) && mic.Open(eCapture, false);
            if (!hasSys && !hasMic) {
                Log(L"wasapi_audio: no audio endpoint could be opened (mode=%d)", (int)mode);
                if (SUCCEEDED(hr)) CoUninitialize();
                impl->active.store(false);
                return;
            }
            HANDLE file = CreateFileW(wavPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                sys.Close();
                mic.Close();
                if (SUCCEEDED(hr)) CoUninitialize();
                impl->active.store(false);
                return;
            }
            WavHeader hdr{};
            DWORD wr = 0;
            WriteFile(file, &hdr, sizeof(hdr), &wr, nullptr);
            uint64_t totalDataBytes = 0;

            auto writePcm = [&](const std::vector<int16_t>& pcm) {
                if (pcm.empty()) return;
                DWORD bytes = (DWORD)(pcm.size() * sizeof(int16_t));
                DWORD w = 0;
                if (WriteFile(file, pcm.data(), bytes, &w, nullptr)) totalDataBytes += w;
            };

            // Optional leading silence if audio was enabled mid-segment.
            if (leadSilenceMs > 0) {
                size_t leadFrames = (size_t)leadSilenceMs * kOutRate / 1000;
                std::vector<int16_t> zeros(std::min<size_t>(leadFrames, kOutRate) * kOutChannels, 0);
                while (leadFrames > 0) {
                    size_t chunk = std::min<size_t>(leadFrames, kOutRate);
                    if (zeros.size() != chunk * kOutChannels) zeros.assign(chunk * kOutChannels, 0);
                    writePcm(zeros);
                    leadFrames -= chunk;
                }
            }

            LARGE_INTEGER freq{}, tStart{};
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&tStart);
            uint64_t emittedFrames = 0;
            std::vector<int16_t> outBuf;

            while (!impl->stop.load()) {
                Sleep(10);
                if (hasSys) sys.Pump();
                if (hasMic) mic.Pump();

                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                double elapsedSec = (double)(now.QuadPart - tStart.QuadPart) / (double)freq.QuadPart;
                uint64_t targetFrames = (uint64_t)(elapsedSec * kOutRate);
                if (targetFrames <= emittedFrames) continue;
                uint64_t count = std::min<uint64_t>(targetFrames - emittedFrames, kOutRate / 5);
                emittedFrames += count;

                const bool isMuted = impl->muted.load();
                outBuf.resize((size_t)count * kOutChannels);
                for (uint64_t i = 0; i < count; ++i) {
                    float l = 0.0f, r = 0.0f;
                    if (hasSys && sys.fifo.size() >= 2) {
                        l += sys.fifo[0];
                        r += sys.fifo[1];
                        sys.fifo.pop_front();
                        sys.fifo.pop_front();
                    }
                    if (hasMic && mic.fifo.size() >= 2) {
                        l += mic.fifo[0];
                        r += mic.fifo[1];
                        mic.fifo.pop_front();
                        mic.fifo.pop_front();
                    }
                    if (isMuted) { l = 0.0f; r = 0.0f; }
                    l = std::clamp(l, -1.0f, 1.0f);
                    r = std::clamp(r, -1.0f, 1.0f);
                    outBuf[(size_t)i * 2] = (int16_t)(l * 32767.0f);
                    outBuf[(size_t)i * 2 + 1] = (int16_t)(r * 32767.0f);
                }
                writePcm(outBuf);
            }

            sys.Close();
            mic.Close();
            if (totalDataBytes <= 0xFFFFFFFFull - 36) {
                hdr.dataSize = (uint32_t)totalDataBytes;
                hdr.chunkSize = 36u + hdr.dataSize;
                LARGE_INTEGER zero{};
                SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
                WriteFile(file, &hdr, sizeof(hdr), &wr, nullptr);
            }
            CloseHandle(file);
            if (SUCCEEDED(hr)) CoUninitialize();
            impl->active.store(false);
        });
        return true;
    } catch (...) {
        impl_->active.store(false);
        return false;
    }
}

void Recorder::SetMuted(bool muted) { impl_->muted.store(muted); }
bool Recorder::Muted() const { return impl_->muted.load(); }
bool Recorder::Active() const { return impl_->active.load(); }

void Recorder::Stop() {
    impl_->stop.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->active.store(false);
}

}  // namespace wasapi_audio
