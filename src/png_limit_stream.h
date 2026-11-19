#pragma once
#include <windows.h>
#include <objidl.h>
#include <algorithm>
#include <cstdint>
#include <limits>

// Bounded output for the append-only Microsoft PNG encoder. A threshold hit is
// distinct from a codec/IO error. Any non-append stream use disables early exit,
// so callers can use their ordinary full-size decision instead.
class PngLimitStream final : public IStream {
public:
    PngLimitStream(IStream* stream, uint64_t limit) : stream_(stream), limit_(limit) { stream_->AddRef(); }
    bool Exceeded() const { return exceeded_; }
    bool AppendOnly() const { return appendOnly_; }
    uint64_t HighWater() const { return highWater_; }
    uint64_t Writes() const { return writes_; }
    uint64_t Seeks() const { return seeks_; }
    uint64_t Resizes() const { return resizes_; }
    void DisableLimit() { appendOnly_ = false; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id != IID_IUnknown && id != IID_ISequentialStream && id != IID_IStream) return E_NOINTERFACE;
        *object = static_cast<IStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override { ULONG n = (ULONG)InterlockedDecrement(&refs_); if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE Read(void* data, ULONG size, ULONG* count) override {
        appendOnly_ = false;
        ULONG read = 0; const HRESULT result = stream_->Read(data, size, &read);
        position_ += read; if (count) *count = read; return result;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG size, ULONG* count) override {
        if (count) *count = 0;
        if (!data && size) return STG_E_INVALIDPOINTER;
        ++writes_;
        if (position_ > std::numeric_limits<uint64_t>::max() - size) return STG_E_INVALIDPARAMETER;
        if (position_ != highWater_) appendOnly_ = false;
        if (appendOnly_ && position_ + size > limit_) { exceeded_ = true; return STG_E_MEDIUMFULL; }
        ULONG written = 0; const HRESULT result = stream_->Write(data, size, &written);
        position_ += written; highWater_ = std::max(highWater_, position_);
        if (count) *count = written; return result;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        ++seeks_; ULARGE_INTEGER next{};
        const HRESULT status = stream_->Seek(move, origin, &next);
        if (SUCCEEDED(status)) {
            if (next.QuadPart != position_) appendOnly_ = false;
            position_ = next.QuadPart;
            if (result) *result = next;
        }
        return status;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        ++resizes_;
        if (size.QuadPart != highWater_) appendOnly_ = false;
        const HRESULT status = stream_->SetSize(size);
        if (SUCCEEDED(status)) highWater_ = size.QuadPart;
        return status;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target, ULARGE_INTEGER size, ULARGE_INTEGER* read, ULARGE_INTEGER* written) override {
        appendOnly_ = false; return stream_->CopyTo(target, size, read, written);
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD flags) override { return exceeded_ ? STG_E_MEDIUMFULL : stream_->Commit(flags); }
    HRESULT STDMETHODCALLTYPE Revert() override { appendOnly_ = false; return stream_->Revert(); }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type) override { return stream_->LockRegion(offset, size, type); }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER offset, ULARGE_INTEGER size, DWORD type) override { return stream_->UnlockRegion(offset, size, type); }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override { return stream_->Stat(stat, flags); }
    HRESULT STDMETHODCALLTYPE Clone(IStream** clone) override { appendOnly_ = false; return stream_->Clone(clone); }
private:
    ~PngLimitStream() { stream_->Release(); }
    LONG refs_ = 1;
    IStream* stream_;
    uint64_t limit_, position_ = 0, highWater_ = 0, writes_ = 0, seeks_ = 0, resizes_ = 0;
    bool exceeded_ = false, appendOnly_ = true;
};
