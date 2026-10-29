#pragma once

#include <atomic>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mfmediaengine.h>

namespace strata::desktop {

/** What a media engine says, kept until the frame loop reads it (it calls from its own threads). */
class MediaEvents final : public IMFMediaEngineNotify {
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (out == nullptr)
            return E_POINTER;
        if (id == IID_IUnknown || id == __uuidof(IMFMediaEngineNotify)) {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0U)
            delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE EventNotify(DWORD event, DWORD_PTR, DWORD) override {
        if (event == MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA)
            loaded = true;
        else if (event == MF_MEDIA_ENGINE_EVENT_ERROR)
            failed = true;
        else if (event == MF_MEDIA_ENGINE_EVENT_ENDED)
            ended = true;
        return S_OK;
    }

    std::atomic<bool> loaded = false;
    std::atomic<bool> failed = false;
    /** Played to its end without looping; whoever plays it again clears it. */
    std::atomic<bool> ended = false;

  private:
    std::atomic<ULONG> references_ = 1U;
};

} // namespace strata::desktop
