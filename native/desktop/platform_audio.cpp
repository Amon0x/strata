#include "platform_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include "media_events.hpp"

namespace strata::desktop {
namespace {

using data::JsonValue;
using Microsoft::WRL::ComPtr;
using namespace platform_io;

/** Engines kept for a source once their sound ends, to play it again at once. */
constexpr std::size_t idle_per_source = 8U;
constexpr std::size_t idle_total = 32U;

class AudioError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/**
 * One sound playing: a Media Foundation engine with no video. A sound without an id is a one-shot
 * nothing names again, so a click can sound many times over itself.
 */
struct Voice final {
    Voice() = default;
    ~Voice() {
        if (engine != nullptr)
            engine->Shutdown();
        engine.Reset();
        if (events != nullptr)
            events->Release();
    }
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;

    std::string client;
    std::string id;
    std::wstring source;
    ComPtr<IMFMediaEngine> engine;
    MediaEvents* events = nullptr;
    /** The play request, answered once the sound is open and its length known. */
    std::optional<PlatformRequest> waiting;
};

class AudioService final : public PlatformService {
  public:
    AudioService(PlatformOutbox& outbox, ResourcePathResolver resolve)
        : outbox_(outbox), resolve_(std::move(resolve)) {}

    ~AudioService() override {
        voices_.clear();
        idle_.clear();
        if (started_)
            MFShutdown();
    }

    AudioService(const AudioService&) = delete;
    AudioService& operator=(const AudioService&) = delete;

    [[nodiscard]] bool serves(const std::string_view kind) const noexcept override {
        return kind.starts_with("audio.");
    }

    void run(const PlatformRequest& request) override {
        try {
            if (request.kind == "audio.play") {
                play(request);
                return;
            }
            const std::string id = text_field(request.payload, "id");
            const auto found = std::ranges::find_if(voices_, [&](const auto& voice) {
                return voice->client == request.client && voice->id == id;
            });
            if (request.kind == "audio.stop") {
                const bool playing = found != voices_.end();
                if (playing)
                    static_cast<void>(retire(found));
                outbox_.reply(request, {{"id", JsonValue(id)},
                                        {"ok", JsonValue(true)},
                                        {"stopped", JsonValue(playing)}});
                return;
            }
            if (found == voices_.end())
                throw AudioError("no sound plays as \"" + id + "\"");
            Voice& voice = **found;
            if (request.kind == "audio.pause") {
                voice.engine->Pause();
            } else if (request.kind == "audio.resume") {
                voice.engine->Play();
            } else if (request.kind == "audio.set") {
                configure(voice, request.payload, false);
            } else {
                throw AudioError("unknown platform request " + request.kind);
            }
            outbox_.reply(request, status(voice));
        } catch (const std::exception& error) {
            outbox_.reply(request, {{"error", JsonValue(std::string(error.what()))},
                                    {"id", JsonValue(text_field(request.payload, "id"))},
                                    {"ok", JsonValue(false)}});
        }
    }

    void tick() override {
        for (auto voice = voices_.begin(); voice != voices_.end();) {
            Voice& playing = **voice;
            if (playing.events->failed) {
                if (playing.waiting.has_value()) {
                    outbox_.reply(*playing.waiting,
                                  {{"error", JsonValue("the sound could not be played")},
                                   {"id", JsonValue(playing.id)},
                                   {"ok", JsonValue(false)}});
                }
                voice = voices_.erase(voice);
                continue;
            }
            if (playing.waiting.has_value() && playing.events->loaded) {
                outbox_.reply(*playing.waiting, status(playing));
                playing.waiting.reset();
            }
            // A sound that has played out (not looping) is done; its engine waits to play again.
            if (!playing.waiting.has_value() && playing.engine->IsEnded() != FALSE) {
                voice = retire(voice);
                continue;
            }
            ++voice;
        }
    }

    void close_client(const std::string_view client) override {
        for (auto voice = voices_.begin(); voice != voices_.end();) {
            if ((*voice)->client == client)
                voice = retire(voice);
            else
                ++voice;
        }
    }

  private:
    using Voices = std::vector<std::unique_ptr<Voice>>;

    void play(const PlatformRequest& request) {
        const std::string named = requested_path(request.payload, "source");
        if (named.empty())
            throw AudioError("audio.play has no source");
        const std::filesystem::path resolved = resolve_path(named, resolve_);
        const std::wstring source = resolved.native();
        const std::string id = text_field(request.payload, "id");
        // An id plays one sound at a time: playing it again replaces what it played.
        if (!id.empty()) {
            const auto found = std::ranges::find_if(voices_, [&](const auto& voice) {
                return voice->client == request.client && voice->id == id;
            });
            if (found != voices_.end())
                static_cast<void>(retire(found));
        }
        std::unique_ptr<Voice> voice;
        if (const auto kept = std::ranges::find_if(
                idle_, [&](const auto& idle) { return idle->source == source; });
            kept != idle_.end()) {
            voice = std::move(*kept);
            idle_.erase(kept);
            voice->engine->SetCurrentTime(0.0);
        } else {
            voice = open(source);
        }
        voice->client = request.client;
        voice->id = id;
        configure(*voice, request.payload, true);
        if (FAILED(voice->engine->Play()))
            throw AudioError("the sound could not be played");
        // An engine played before already knows its sound's length.
        if (voice->events->loaded)
            outbox_.reply(request, status(*voice));
        else
            voice->waiting = request;
        voices_.push_back(std::move(voice));
    }

    [[nodiscard]] std::unique_ptr<Voice> open(const std::wstring& source) {
        if (!started_) {
            if (FAILED(MFStartup(MF_VERSION)))
                throw AudioError("Media Foundation is unavailable");
            started_ = true;
        }
        ComPtr<IMFMediaEngineClassFactory> factory;
        ComPtr<IMFAttributes> attributes;
        if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) ||
            FAILED(MFCreateAttributes(&attributes, 1U))) {
            throw AudioError("the media engine is unavailable");
        }
        auto voice = std::make_unique<Voice>();
        voice->source = source;
        voice->events = new MediaEvents();
        attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, voice->events);
        if (FAILED(factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY, attributes.Get(),
                                           &voice->engine))) {
            throw AudioError("the media engine could not start");
        }
        BSTR location = SysAllocString(source.c_str());
        const HRESULT opened = voice->engine->SetSource(location);
        SysFreeString(location);
        if (FAILED(opened))
            throw AudioError("the sound could not be opened");
        return voice;
    }

    /**
     * Applies what `payload` sets. Playing sets everything, what it leaves out to its default (an
     * engine played before keeps nothing of then); audio.set changes only what it names.
     */
    static void configure(Voice& voice, const JsonValue& payload, const bool playing) {
        const JsonValue* loop = payload.find("loop");
        if (loop != nullptr && loop->boolean() != nullptr)
            voice.engine->SetLoop(*loop->boolean() ? TRUE : FALSE);
        else if (playing)
            voice.engine->SetLoop(FALSE);
        const JsonValue* muted = payload.find("muted");
        if (muted != nullptr && muted->boolean() != nullptr)
            voice.engine->SetMuted(*muted->boolean() ? TRUE : FALSE);
        else if (playing)
            voice.engine->SetMuted(FALSE);
        if (const std::optional<double> volume = number_field(payload, "volume"))
            voice.engine->SetVolume(std::clamp(*volume, 0.0, 1.0));
        else if (playing)
            voice.engine->SetVolume(1.0);
        const std::optional<double> rate = number_field(payload, "rate");
        if (rate.has_value() || playing) {
            const double speed = std::clamp(rate.value_or(1.0), 0.25, 4.0);
            voice.engine->SetDefaultPlaybackRate(speed);
            voice.engine->SetPlaybackRate(speed);
        }
    }

    [[nodiscard]] static JsonValue::Object status(const Voice& voice) {
        const double duration = voice.engine->GetDuration();
        return {{"duration", JsonValue(std::isfinite(duration) ? duration : 0.0)},
                {"ended", JsonValue(voice.engine->IsEnded() != FALSE)},
                {"id", JsonValue(voice.id)},
                {"ok", JsonValue(true)},
                {"paused", JsonValue(voice.engine->IsPaused() != FALSE)},
                {"position", JsonValue(voice.engine->GetCurrentTime())}};
    }

    /** Stops a voice; its engine waits to play the same source again, while there is room. */
    [[nodiscard]] Voices::iterator retire(const Voices::iterator voice) {
        std::unique_ptr<Voice> done = std::move(*voice);
        const Voices::iterator next = voices_.erase(voice);
        // An engine still opening, or one that failed, has nothing to offer.
        if (done->waiting.has_value() || !done->events->loaded || done->events->failed)
            return next;
        done->engine->Pause();
        done->engine->SetLoop(FALSE);
        done->waiting.reset();
        const auto same = static_cast<std::size_t>(std::ranges::count_if(
            idle_, [&done](const auto& idle) { return idle->source == done->source; }));
        if (same >= idle_per_source)
            return next;
        if (idle_.size() >= idle_total)
            idle_.erase(idle_.begin());
        idle_.push_back(std::move(done));
        return next;
    }

    PlatformOutbox& outbox_;
    ResourcePathResolver resolve_;
    bool started_ = false;
    Voices voices_;
    /** Engines whose sounds ended, oldest first. */
    Voices idle_;
};

} // namespace

std::unique_ptr<PlatformService> make_audio_service(PlatformOutbox& outbox,
                                                    ResourcePathResolver resolve) {
    return std::make_unique<AudioService>(outbox, std::move(resolve));
}

} // namespace strata::desktop
