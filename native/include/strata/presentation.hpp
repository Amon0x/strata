#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <strata/config.hpp>
#include <strata/profiler.hpp>
#include <strata/render_packet.hpp>

namespace strata::host {

using PresentationStream = std::uint64_t;

enum class PresentationExecution { caller, threaded };

struct MaterialSource final {
    MaterialDeclaration declaration;
    std::string source;
};

struct EffectSource final {
    EffectPassDeclaration declaration;
    std::string source;
};

struct PresentationPrograms final {
    std::vector<MaterialSource> materials;
    std::vector<EffectSource> effects;
};

struct PresentationViewport final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    double logical_width = 0.0;
    double logical_height = 0.0;
};

/** A backend-owned target's pixels, transferred as straight RGBA8 rows, top first. */
struct PresentationCapture final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> rgba;
};

struct PresentationInfo final {
    std::string adapter;
    std::string driver_version;
    std::uint32_t vendor_id = 0U;
    std::uint32_t device_id = 0U;
    std::uint64_t dedicated_video_memory = 0U;
    bool vsync = true;
};

struct PresentationLayerTiming final {
    std::uint64_t blur_passes = 0U;
    std::uint32_t blur_target_width = 0U;
    std::uint32_t blur_target_height = 0U;
    std::uint64_t blur_nanos = 0U;
    std::uint64_t effect_passes = 0U;
    std::uint32_t effect_target_width = 0U;
    std::uint32_t effect_target_height = 0U;
    std::uint64_t effect_nanos = 0U;
    std::uint64_t gpu_nanos = 0U;
    bool live = false;
};

/** An offscreen draw is complete without presenting; an occluded/outdated target needs another turn. */
struct PresentationResult final {
    bool presented = false;
    bool redraw = false;
};

/** Only the presentation owner calls this driver, including its factory and destructor. */
class PresentationDriver {
  public:
    virtual ~PresentationDriver() = default;
    [[nodiscard]] virtual PresentationInfo info() const = 0;
    virtual void declare_material(const MaterialSource& material) = 0;
    virtual void declare_effect(const EffectSource& effect) = 0;
    /** Removes one retired stream's declarations; in-flight GPU readers retain their programs. */
    virtual void release_programs(std::string_view prefix) = 0;
    virtual void resize(const PresentationViewport& viewport) = 0;
    virtual void set_pointer(std::optional<std::array<double, 2U>> position) = 0;
    virtual void set_transparent(bool transparent) = 0;
    virtual void consume_resources(const RenderPacket& packet) = 0;
    virtual void release_layer(std::string_view id) noexcept = 0;
    /** Acquires a drawable target; false defers drawing without discarding the retained commit. */
    [[nodiscard]] virtual bool begin_frame() = 0;
    [[nodiscard]] virtual PresentationLayerTiming draw(std::string_view id,
                                                       const RenderPacket& packet) = 0;
    [[nodiscard]] virtual std::vector<strata_backdrop_sample> take_backdrop_samples(
        std::string_view id) = 0;
    /** Completes the draw with the driver's own display pacing and target-availability outcome. */
    [[nodiscard]] virtual PresentationResult end_frame() = 0;
    virtual void capture_next_frame() = 0;
    [[nodiscard]] virtual std::optional<PresentationCapture> take_capture() = 0;
};

struct PresentationLayerCommit final {
    PresentationStream stream = 0U;
    EncodedRenderPacket packet;
    /** The producer's unchanged observation suppresses redraw, never ordered consumption. */
    bool unchanged = false;
};

/** One atomic visible layer order, after every supplied packet's resources have been consumed. */
struct PresentationCommit final {
    std::vector<PresentationLayerCommit> layers;
    std::vector<PresentationStream> order;
    /** Requests display-paced owner turns for time-driven UI work; caller mode drives its own clock. */
    bool animate = false;
};

struct PresentationLayerFeedback final {
    PresentationStream stream = 0U;
    ProfilerHostFrame profiler;
    PresentationLayerTiming rendering;
    std::vector<strata_backdrop_sample> backdrop;
};

/** CPU submission times, not scan-out evidence. The commit sequence survives retained redraws. */
struct PresentationFrame final {
    std::uint64_t sequence = 0U;
    std::int64_t started_nanos = 0;
    std::int64_t present_nanos = 0;
    std::int64_t finished_nanos = 0;
    bool drawn = false;
    bool presented = false;
    std::vector<PresentationLayerFeedback> layers;
    std::vector<std::uint64_t> capture_requests;
    std::optional<PresentationCapture> capture;
};

/** Independent completion ownership; success is published only after the GPU owner consumed it. */
class PresentationReceipt final {
  public:
    PresentationReceipt() noexcept = default;
    [[nodiscard]] bool ready() const noexcept;
    /** Requires readiness and rethrows the owned failure, if any. */
    void get() const;

  private:
    friend class Presentation;
    struct State;
    explicit PresentationReceipt(std::shared_ptr<State> state) noexcept;
    std::shared_ptr<State> state_;
};

/**
 * Ordered, runtime-free presentation ownership. Producers transfer bytes and declarations; one
 * consumer owns each decoder and all GPU state. Caller mode uses the same consumption path inline.
 * A threaded driver owns its target lifecycle; borrowed context/target callbacks are not a safe
 * substitute. Keep external window/device ownership alive until stopped() reports completion.
 */
class Presentation final {
  public:
    using Factory = std::function<std::unique_ptr<PresentationDriver>()>;
    /** Thread-safe notification only; the callback must not enter a runtime or UI service. */
    using Wake = std::function<void()>;
    explicit Presentation(Factory factory,
                          PresentationExecution execution = PresentationExecution::threaded,
                          Wake wake = {});
    ~Presentation();
    Presentation(const Presentation&) = delete;
    Presentation& operator=(const Presentation&) = delete;

    [[nodiscard]] PresentationStream attach(std::string id);
    /** Authored program names belong to this stream, never another live or staged scene. */
    [[nodiscard]] PresentationReceipt declare_programs(PresentationStream stream,
                                                        PresentationPrograms&& programs);
    void resize(PresentationViewport viewport);
    void set_pointer(std::optional<std::array<double, 2U>> position);
    void set_transparent(bool transparent);
    /** Admission moves the commit only after queue storage exists. */
    [[nodiscard]] std::uint64_t commit(PresentationCommit&& commit);
    [[nodiscard]] PresentationReceipt retire(PresentationStream stream,
                                              EncodedRenderPacket terminal);
    /** Captures a presentation at or beyond the next commit, never an earlier retained redraw. */
    [[nodiscard]] std::uint64_t capture_next_frame();
    /** Reuses the caller's previous output capacity; feedback contains no borrowed decoder data. */
    void take_frames(std::vector<PresentationFrame>& output);
    [[nodiscard]] PresentationInfo info() const;
    /** Successful Present returns, not proof that every submitted image reached scan-out. */
    [[nodiscard]] std::uint64_t present_count() const noexcept;
    void rethrow_failure() const;
    /** Nonjoining: accepted commands drain, then the driver destroys its GPU state on its owner. */
    void stop() noexcept;
    [[nodiscard]] bool stopped() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::host
