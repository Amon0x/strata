#include <strata/presentation.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

namespace strata::host {
namespace {

[[nodiscard]] std::int64_t now() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] ProfilerHostFrame packet_counts(const RenderPacket& packet) {
    ProfilerHostFrame result;
    result.frame_index = packet.frame_index;
    result.draw_calls = packet.planned_draw_count;
    result.batches = packet.batches.size();
    result.vertices = packet.vertices.size() / packet_vertex_bytes;
    for (const ResourceOperation& resource : packet.resources) {
        const std::uint64_t texels = std::uint64_t{resource.width} * resource.height;
        const std::uint64_t bytes = texels * (texture_format_rgba(resource.format) ? 4U : 1U);
        if (resource.kind == resource_create || resource.kind == resource_image) {
            ++result.texture_creations;
            result.texture_creation_bytes += bytes;
        }
        if (resource.kind == resource_upload || resource.kind == resource_image) {
            ++result.texture_uploads;
            result.texture_upload_bytes += resource.bytes.size();
        } else if (resource.kind == resource_vector_glyphs) {
            result.texture_upload_bytes += resource.vector_glyphs.records.size() +
                resource.vector_glyphs.segments.size() + resource.vector_glyphs.bands.size();
        }
    }
    return result;
}

} // namespace

struct PresentationReceipt::State final {
    void complete(std::exception_ptr failure = {}) noexcept {
        if (ready.load(std::memory_order_acquire))
            return;
        error = std::move(failure);
        ready.store(true, std::memory_order_release);
    }
    std::atomic_bool ready{false};
    std::exception_ptr error;
};

PresentationReceipt::PresentationReceipt(std::shared_ptr<State> state) noexcept
    : state_(std::move(state)) {}

bool PresentationReceipt::ready() const noexcept {
    return state_ != nullptr && state_->ready.load(std::memory_order_acquire);
}

void PresentationReceipt::get() const {
    if (!ready())
        throw std::logic_error("presentation receipt is not complete");
    if (state_->error)
        std::rethrow_exception(state_->error);
}

struct Presentation::Impl final {
    using Receipt = std::shared_ptr<PresentationReceipt::State>;
    struct Attach { PresentationStream stream; std::string id; };
    struct Programs { PresentationStream stream; PresentationPrograms sources; Receipt receipt; };
    struct Resize { PresentationViewport viewport; };
    struct Pointer { std::optional<std::array<double, 2U>> position; };
    struct Transparent { bool value; };
    struct Commit { std::uint64_t sequence; PresentationCommit value; };
    struct Retire { PresentationStream stream; EncodedRenderPacket terminal; Receipt receipt; };
    struct Capture { std::uint64_t request; };
    struct PendingCapture { std::uint64_t request; std::uint64_t minimum_sequence; };
    using Message = std::variant<Attach, Programs, Resize, Pointer, Transparent,
                                 Commit, Retire, Capture>;

    struct Stream final {
        Stream(std::string name, std::string prefix)
            : id(std::move(name)), program_prefix(std::move(prefix)) {}
        std::string id;
        std::string program_prefix;
        RenderPacketDecoder decoder;
        const RenderPacket* packet = nullptr;
        std::optional<ProfilerHostFrame> pending;
        bool resources_pending = false;
        std::uint64_t sequence = 0U;
        std::int64_t accepted_nanos = 0;
    };

    Impl(Factory create, const PresentationExecution mode, Wake notify)
        : factory(std::move(create)), execution(mode), wake(std::move(notify)),
          caller(std::this_thread::get_id()) {
        if (!factory)
            throw std::invalid_argument("presentation requires an owned driver factory");
        if (execution != PresentationExecution::caller &&
            execution != PresentationExecution::threaded)
            throw std::invalid_argument("presentation execution mode is invalid");
        if (execution == PresentationExecution::caller)
            initialize();
        else
            worker = std::thread([this] { run(); });
    }

    ~Impl() {
        stop();
        if (worker.joinable())
            worker.join();
    }

    void initialize() {
        driver = factory();
        factory = {};
        if (driver == nullptr)
            throw std::invalid_argument("presentation factory returned no driver");
        PresentationInfo info = driver->info();
        std::scoped_lock lock(mutex);
        information = std::move(info);
    }

    void require_open() const {
        if (failure)
            std::rethrow_exception(failure);
        if (closing)
            throw std::logic_error("presentation owner is stopping");
        if (execution == PresentationExecution::caller && caller != std::this_thread::get_id())
            throw std::logic_error("caller-driven presentation requires its constructing thread");
    }

    template <typename Command, typename... Args>
    void post(Args&&... args) {
        std::optional<Message> immediate;
        {
            std::scoped_lock lock(mutex);
            require_open();
            if (execution == PresentationExecution::threaded)
                messages.emplace_back(std::in_place_type<Command>, std::forward<Args>(args)...);
            else
                immediate.emplace(std::in_place_type<Command>, std::forward<Args>(args)...);
        }
        if (immediate.has_value())
            consume_inline(*immediate);
        else
            changed.notify_one();
    }

    template <typename Command, typename... Args>
    std::uint64_t post_numbered(std::uint64_t& next, Args&&... args) {
        std::optional<Message> immediate;
        std::uint64_t number;
        {
            std::scoped_lock lock(mutex);
            require_open();
            if (next == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("presentation identity space is exhausted");
            number = next;
            if (execution == PresentationExecution::threaded)
                messages.emplace_back(std::in_place_type<Command>, number,
                                      std::forward<Args>(args)...);
            else
                immediate.emplace(std::in_place_type<Command>, number,
                                  std::forward<Args>(args)...);
            ++next;
        }
        if (immediate.has_value())
            consume_inline(*immediate);
        else
            changed.notify_one();
        return number;
    }

    void consume_inline(Message& message) {
        try {
            consume(message);
            if (std::holds_alternative<Commit>(message) && (dirty || live || redraw))
                draw_frame();
        } catch (...) {
            fail(std::current_exception());
            fail_message(message, failure);
            driver.reset();
            ended.store(true, std::memory_order_release);
            throw;
        }
    }

    void consume(Message& message) {
        std::visit([this](auto& value) { accept(value); }, message);
    }

    void accept(Attach& value) {
        const std::string identity = std::to_string(value.stream);
        streams.try_emplace(value.stream, value.id + "#" + identity,
                            "strata:presentation/" + identity + "/");
    }

    void accept(Programs& value) {
        try {
            const std::string& prefix = stream(value.stream).program_prefix;
            for (MaterialSource& material : value.sources.materials) {
                if (material.declaration.id == "strata:unified_ui")
                    throw std::invalid_argument("the built-in unified material cannot be redeclared");
                material.declaration.id.insert(0U, prefix);
                driver->declare_material(material);
            }
            for (EffectSource& effect : value.sources.effects) {
                effect.declaration.effect_id.insert(0U, prefix);
                driver->declare_effect(effect);
            }
            dirty = dirty || !value.sources.materials.empty() || !value.sources.effects.empty();
            value.receipt->complete();
        } catch (...) {
            value.receipt->complete(std::current_exception());
        }
        notify_owner();
    }

    void accept(Resize& value) {
        driver->resize(value.viewport);
        dirty = true;
    }

    void accept(Pointer& value) {
        if (pointer == value.position)
            return;
        pointer = value.position;
        driver->set_pointer(value.position);
        dirty = true;
    }

    void accept(Transparent& value) {
        if (transparent == value.value)
            return;
        transparent = value.value;
        driver->set_transparent(value.value);
        dirty = true;
    }

    [[nodiscard]] Stream& stream(const PresentationStream id) {
        const auto found = streams.find(id);
        if (found == streams.end())
            throw std::invalid_argument("presentation stream is not attached");
        return found->second;
    }

    void publish(PresentationFrame frame) {
        {
            std::scoped_lock lock(mutex);
            // A stalled owner sees recent feedback; captures and resource frames are never dropped.
            if (frames.size() >= retained_feedback) {
                const auto oldest = std::ranges::find_if(frames, [](const PresentationFrame& held) {
                    return !held.capture.has_value() && held.capture_requests.empty() &&
                           std::ranges::all_of(held.layers, [](const auto& layer) {
                               return layer.backdrop.empty();
                           });
                });
                if (oldest != frames.end())
                    frames.erase(oldest);
            }
            frames.push_back(std::move(frame));
        }
        notify_feedback();
    }

    void publish_undrawn(const PresentationStream id, Stream& source) {
        if (!source.pending.has_value())
            return;
        // Waking for a resource-free skipped commit makes an idle owner's next frame wake it again.
        if (!source.resources_pending) {
            source.pending.reset();
            return;
        }
        PresentationFrame frame;
        frame.sequence = source.sequence;
        frame.started_nanos = source.accepted_nanos;
        frame.finished_nanos = now();
        PresentationLayerFeedback layer;
        layer.stream = id;
        layer.profiler = *source.pending;
        layer.profiler.draw_calls = 0U;
        layer.profiler.batches = 0U;
        layer.profiler.vertices = 0U;
        frame.layers.push_back(std::move(layer));
        publish(std::move(frame));
        source.pending.reset();
        source.resources_pending = false;
    }

    void accept(Commit& value) {
        for (const PresentationStream id : value.value.order)
            static_cast<void>(stream(id));
        for (PresentationLayerCommit& layer : value.value.layers) {
            Stream& target = stream(layer.stream);
            publish_undrawn(layer.stream, target);
            const std::int64_t started = now();
            const RenderPacket& packet = target.decoder.decode(layer.packet.bytes(), target.program_prefix);
            ProfilerHostFrame counts = packet_counts(packet);
            dirty = dirty || !layer.unchanged || !packet.resources.empty();
            target.resources_pending = !packet.resources.empty();
            if (!packet.resources.empty())
                driver->consume_resources(packet);
            target.decoder.acknowledge_resources();
            counts.submit_nanoseconds = now() - started;
            target.packet = &packet;
            target.pending = counts;
            target.sequence = value.sequence;
            target.accepted_nanos = started;
        }
        dirty = dirty || order != value.value.order;
        order = std::move(value.value.order);
        needs_ui_frames = value.value.animate;
        sequence = value.sequence;
        dirty = dirty || std::ranges::any_of(captures, [this](const PendingCapture& capture) {
            return capture.minimum_sequence <= sequence;
        });
    }

    void accept(Retire& value) {
        Stream& target = stream(value.stream);
        publish_undrawn(value.stream, target);
        const RenderPacket& terminal = target.decoder.decode(value.terminal.bytes(), target.program_prefix);
        driver->consume_resources(terminal);
        driver->release_layer(target.id);
        driver->release_programs(target.program_prefix);
        std::erase(order, value.stream);
        streams.erase(value.stream);
        dirty = true;
        value.receipt->complete();
        notify_owner();
    }

    void accept(Capture& value) {
        captures.push_back({value.request, sequence + 1U});
    }

    void draw_frame() {
        const std::int64_t frame_started = now();
        if (!driver->begin_frame()) {
            redraw = true;
            visible = false;
            return;
        }
        PresentationFrame frame;
        frame.sequence = sequence;
        frame.started_nanos = frame_started;
        frame.layers.reserve(order.size());
        const bool capturing = std::ranges::any_of(captures, [this](const PendingCapture& capture) {
            return capture.minimum_sequence <= sequence;
        });
        if (capturing)
            driver->capture_next_frame();
        live = false;
        for (const PresentationStream id : order) {
            Stream& source = stream(id);
            if (source.packet == nullptr)
                continue;
            PresentationLayerFeedback feedback;
            feedback.stream = id;
            feedback.profiler = source.pending.has_value() ? *source.pending
                                                          : packet_counts(*source.packet);
            const std::int64_t started = now();
            feedback.rendering = driver->draw(source.id, *source.packet);
            feedback.backdrop = driver->take_backdrop_samples(source.id);
            feedback.profiler.submit_nanoseconds += now() - started;
            feedback.profiler.blur_passes = feedback.rendering.blur_passes;
            feedback.profiler.blur_target_width = feedback.rendering.blur_target_width;
            feedback.profiler.blur_target_height = feedback.rendering.blur_target_height;
            feedback.profiler.blur_nanoseconds = feedback.rendering.blur_nanos;
            live = live || feedback.rendering.live;
            source.decoder.acknowledge_draw();
            source.pending.reset();
            source.resources_pending = false;
            frame.layers.push_back(std::move(feedback));
        }
        const std::int64_t present_started = now();
        const PresentationResult result = driver->end_frame();
        visible = result.presented;
        redraw = result.redraw;
        if (visible)
            presented.fetch_add(1U, std::memory_order_relaxed);
        frame.drawn = true;
        frame.present_nanos = now() - present_started;
        frame.presented = visible;
        frame.finished_nanos = now();
        if (capturing) {
            frame.capture = driver->take_capture();
            if (!frame.capture.has_value())
                throw std::runtime_error("presentation driver did not return its requested capture");
            for (const PendingCapture& capture : captures) {
                if (capture.minimum_sequence <= sequence)
                    frame.capture_requests.push_back(capture.request);
            }
            std::erase_if(captures, [this](const PendingCapture& capture) {
                return capture.minimum_sequence <= sequence;
            });
        }
        dirty = false;
        publish(std::move(frame));
    }

    static void fail_message(Message& message, const std::exception_ptr& error) noexcept {
        std::visit([&error](auto& value) {
            if constexpr (requires { value.receipt; })
                value.receipt->complete(error);
        }, message);
    }

    void fail(std::exception_ptr error) noexcept {
        std::scoped_lock lock(mutex);
        if (!failure)
            failure = std::move(error);
        closing = true;
        for (Message& message : messages)
            fail_message(message, failure);
        messages.clear();
    }

    /** One wake until the owner next takes its feedback, however many frames pass meanwhile. */
    void notify_feedback() noexcept {
        if (!wake_pending.exchange(true, std::memory_order_acq_rel))
            notify_owner();
    }

    void notify_owner() noexcept {
        if (execution != PresentationExecution::threaded || !wake)
            return;
        try {
            wake();
        } catch (...) {
            fail(std::current_exception());
        }
    }

    void run() noexcept {
        std::deque<Message> batch;
        try {
            initialize();
            for (;;) {
                {
                    std::unique_lock lock(mutex);
                    const auto requested = [this] { return closing || !messages.empty(); };
                    if (!dirty && !live && !needs_ui_frames && !redraw)
                        changed.wait(lock, requested);
                    else if (!visible)
                        changed.wait_for(lock, std::chrono::milliseconds(50), requested);
                    messages.swap(batch);
                    if (closing && batch.empty())
                        break;
                }
                for (Message& message : batch)
                    consume(message);
                batch.clear();
                consumed.store(sequence, std::memory_order_release);
                if (owner_waiting.exchange(false, std::memory_order_acq_rel))
                    notify_owner();
                if (dirty || live || needs_ui_frames || redraw)
                    draw_frame();
            }
        } catch (...) {
            const std::exception_ptr error = std::current_exception();
            for (Message& message : batch)
                fail_message(message, error);
            fail(error);
        }
        streams.clear();
        driver.reset();
        factory = {};
        ended.store(true, std::memory_order_release);
        notify_owner();
    }

    void stop() noexcept {
        {
            std::scoped_lock lock(mutex);
            closing = true;
        }
        changed.notify_one();
        if (execution == PresentationExecution::caller) {
            streams.clear();
            driver.reset();
            ended.store(true, std::memory_order_release);
        }
    }

    Factory factory;
    PresentationExecution execution;
    Wake wake;
    std::thread::id caller;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Message> messages;
    std::vector<PresentationFrame> frames;
    PresentationInfo information;
    std::exception_ptr failure;
    bool closing = false;
    std::atomic_bool ended{false};
    std::atomic<std::uint64_t> presented{0U};
    std::atomic<std::uint64_t> consumed{0U};
    std::atomic<std::uint64_t> committed{0U};
    std::atomic_bool owner_waiting{false};
    std::atomic_bool wake_pending{false};
    static constexpr std::size_t retained_feedback = 64U;
    std::uint64_t next_stream = 1U;
    std::uint64_t next_sequence = 1U;
    std::uint64_t next_capture = 1U;
    /** Consumer-only state; the worker destroys it before publishing ended. */
    std::unique_ptr<PresentationDriver> driver;
    std::map<PresentationStream, Stream> streams;
    std::vector<PresentationStream> order;
    std::vector<PendingCapture> captures;
    std::optional<std::array<double, 2U>> pointer;
    bool transparent = false;
    std::uint64_t sequence = 0U;
    bool dirty = false;
    bool live = false;
    bool redraw = false;
    bool needs_ui_frames = false;
    bool visible = true;
    std::thread worker;
};

Presentation::Presentation(Factory factory, const PresentationExecution execution, Wake wake)
    : impl_(std::make_unique<Impl>(std::move(factory), execution, std::move(wake))) {}
Presentation::~Presentation() = default;

PresentationStream Presentation::attach(std::string id) {
    if (id.empty())
        throw std::invalid_argument("presentation layer id must not be empty");
    return impl_->post_numbered<Impl::Attach>(impl_->next_stream, std::move(id));
}

PresentationReceipt Presentation::declare_programs(const PresentationStream stream,
                                                    PresentationPrograms&& programs) {
    auto receipt = std::make_shared<PresentationReceipt::State>();
    impl_->post<Impl::Programs>(stream, std::move(programs), receipt);
    return PresentationReceipt(std::move(receipt));
}

void Presentation::resize(const PresentationViewport viewport) {
    impl_->post<Impl::Resize>(viewport);
}
void Presentation::set_pointer(const std::optional<std::array<double, 2U>> position) {
    impl_->post<Impl::Pointer>(position);
}
void Presentation::set_transparent(const bool transparent) {
    impl_->post<Impl::Transparent>(transparent);
}
bool Presentation::ready_for_frame() noexcept {
    if (impl_->execution != PresentationExecution::threaded)
        return true;
    const auto caught_up = [this] {
        return impl_->consumed.load(std::memory_order_acquire) >=
               impl_->committed.load(std::memory_order_relaxed);
    };
    if (caught_up())
        return true;
    impl_->owner_waiting.store(true, std::memory_order_release);
    // The consumer may have finished between the check and the flag.
    if (!caught_up())
        return false;
    impl_->owner_waiting.store(false, std::memory_order_release);
    return true;
}
std::uint64_t Presentation::commit(PresentationCommit&& commit) {
    const std::uint64_t sequence =
        impl_->post_numbered<Impl::Commit>(impl_->next_sequence, std::move(commit));
    impl_->committed.store(sequence, std::memory_order_relaxed);
    return sequence;
}
PresentationReceipt Presentation::retire(const PresentationStream stream,
                                          EncodedRenderPacket terminal) {
    auto receipt = std::make_shared<PresentationReceipt::State>();
    impl_->post<Impl::Retire>(stream, std::move(terminal), receipt);
    return PresentationReceipt(std::move(receipt));
}
std::uint64_t Presentation::capture_next_frame() {
    return impl_->post_numbered<Impl::Capture>(impl_->next_capture);
}
void Presentation::take_frames(std::vector<PresentationFrame>& output) {
    output.clear();
    impl_->wake_pending.store(false, std::memory_order_release);
    std::scoped_lock lock(impl_->mutex);
    output.swap(impl_->frames);
}
PresentationInfo Presentation::info() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->information;
}
std::uint64_t Presentation::present_count() const noexcept {
    return impl_->presented.load(std::memory_order_relaxed);
}
void Presentation::rethrow_failure() const {
    std::exception_ptr error;
    {
        std::scoped_lock lock(impl_->mutex);
        error = impl_->failure;
    }
    if (error)
        std::rethrow_exception(error);
}
void Presentation::stop() noexcept { impl_->stop(); }
bool Presentation::stopped() const noexcept {
    return impl_->ended.load(std::memory_order_acquire);
}

} // namespace strata::host
