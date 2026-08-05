#include "runtime/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

#ifdef STRATA_ENABLE_TRACY
#include <tracy/TracyC.h>
#endif

namespace strata::runtime {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(ProfilerCounter::count)>
    counter_names{
        "render.draw_calls",
        "render.batches",
        "render.batch_breaks.texture",
        "render.batch_breaks.clip",
        "render.batch_breaks.material",
        "render.batch_breaks.scope",
        "render.batch_breaks.effect",
        "render.encoded_geometry_cache_hits",
        "render.encoded_geometry_cache_misses",
        "render.vertices",
        "textures.uploads",
        "textures.upload_bytes",
        "textures.queued_uploads",
        "textures.queued_upload_bytes",
        "textures.creations",
        "textures.creation_bytes",
        "textures.pending_creations",
        "gpu.deferred_resources",
        "gpu.deferred_resource_deletions",
        "resources.reload_duration_nanos",
        "hot_reload.gpu_upload_jobs",
        "hot_reload.gpu_upload_bytes",
        "hot_reload.gpu_upload_deferred_jobs",
        "hot_reload.gpu_upload_deferred_bytes",
        "hot_reload.gpu_upload_over_budget_jobs",
        "hot_reload.gpu_upload_over_budget_bytes",
        "hot_reload.gpu_upload_rejected_jobs",
        "hot_reload.gpu_upload_rejected_bytes",
        "text.layout_requests",
        "text.layout_cache_hits",
        "text.layout_cache_misses",
        "text.layout_cache_lookup_nanos",
        "text.layout_cache_restore_nanos",
        "text.layout_cache_store_nanos",
        "text.shaping_nanos",
        "layout.nanos",
        "input.nanos",
        "input.events_processed",
        "input.events_deferred",
        "input.dispatches",
        "input.mutated_nodes",
        "input.coalesced_moves",
        "input.behavior_dispatches",
        "layout.measured_nodes",
        "layout.reused_nodes",
        "layout.arranged_nodes",
        "inspector.nanos",
        "animation.nanos",
        "animation.mutated_nodes",
        "animation.running_players",
        "render.submission_nanos",
        "blur.passes",
        "blur.target_width",
        "blur.target_height",
        "blur.nanos",
        "frames",
        "description.nodes",
        "expressions.evaluated",
        "description.rebuilds",
        "input.injected_events",
        "render.commands",
        "render.fragments_built",
        "render.fragments_reused",
        "render.nodes_visited",
        "render.overlays",
        "render.portals",
        "render.packet_bytes",
        "host.submissions",
        "host.submit_nanos",
        "resources.reloads",
        "diagnostics.records",
        "input.pointer_geometry_rebuilds",
        "input.fast_path_frames",
        "text.font_resolution_nanos",
        "text.opentype_nanos",
        "text.line_assembly_nanos",
    };

[[nodiscard]] std::int64_t steady_now_nanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

[[nodiscard]] bool valid_section_name(const std::string_view name) noexcept {
    return !name.empty() && !name.contains('/') && !name.contains('\0');
}

} // namespace

std::string_view profiler_counter_name(const ProfilerCounter counter) noexcept {
    const std::size_t index = static_cast<std::size_t>(counter);
    return index < counter_names.size() ? counter_names[index] : std::string_view{};
}

void ProfilerConfig::validate() const {
    if (timing_window_size == 0U || maximum_sections == 0U ||
        maximum_recent_spikes == 0U || spike_absolute_floor_nanos < 0 ||
        !std::isfinite(spike_baseline_multiplier) || spike_baseline_multiplier < 1.0 ||
        spike_minimum_baseline_samples == 0U) {
        throw std::invalid_argument("profiler configuration contains an invalid bound or spike policy");
    }
}

Profiler::RollingWindow::RollingWindow(const std::size_t requested_capacity)
    : capacity(requested_capacity) {
    ordered_samples.reserve(capacity);
}

bool Profiler::RollingWindow::add(const std::int64_t value) {
    if (value < 0) throw std::invalid_argument("profiler timing samples must be non-negative");
    const bool evicted = samples.size() == capacity;
    if (samples.size() == capacity) {
        const std::int64_t removed = samples.front();
        samples.pop_front();
        sum -= static_cast<std::uint64_t>(removed);
        ordered_samples.erase(std::ranges::lower_bound(ordered_samples, removed));
    }
    samples.push_back(value);
    ordered_samples.insert(
        std::ranges::upper_bound(ordered_samples, value),
        value
    );
    const std::uint64_t converted = static_cast<std::uint64_t>(value);
    sum = converted > std::numeric_limits<std::uint64_t>::max() - sum
        ? std::numeric_limits<std::uint64_t>::max()
        : sum + converted;
    maximum_value = ordered_samples.back();
    return evicted;
}

std::int64_t Profiler::RollingWindow::average() const noexcept {
    if (samples.empty()) return 0;
    const std::uint64_t value = sum / samples.size();
    return value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(value);
}

std::int64_t Profiler::RollingWindow::maximum() const noexcept { return maximum_value; }

std::int64_t Profiler::RollingWindow::percentile(const double fraction) const {
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) {
        throw std::invalid_argument("profiler percentile must be in [0, 1]");
    }
    if (samples.empty()) return 0;
    const std::size_t rank = static_cast<std::size_t>(std::ceil(
        fraction * static_cast<double>(ordered_samples.size())
    ));
    const std::size_t index =
        std::clamp(rank, std::size_t{1U}, ordered_samples.size()) - 1U;
    return ordered_samples[index];
}

Profiler::Section::Section(
    Profiler* const profiler,
    const std::uint64_t generation,
    const std::uint64_t token,
    const std::string_view name
) noexcept : profiler_(profiler), generation_(generation), token_(token) {
#ifdef STRATA_ENABLE_TRACY
    static constexpr ___tracy_source_location_data source_location{
        nullptr,
        "strata::runtime::Profiler::Section",
        __FILE__,
        __LINE__,
        0U,
    };
    const TracyCZoneCtx trace = ___tracy_emit_zone_begin(&source_location, 1);
    trace_id_ = trace.id;
    trace_active_ = trace.active;
    ___tracy_emit_zone_name(trace, name.data(), name.size());
#else
    static_cast<void>(name);
#endif
}

Profiler::Section::Section(Section&& other) noexcept
    : profiler_(std::exchange(other.profiler_, nullptr)),
      generation_(other.generation_),
      token_(other.token_),
      trace_id_(other.trace_id_),
      trace_active_(std::exchange(other.trace_active_, 0)) {}

Profiler::Section& Profiler::Section::operator=(Section&& other) noexcept {
    if (this == &other) return *this;
    close();
    profiler_ = std::exchange(other.profiler_, nullptr);
    generation_ = other.generation_;
    token_ = other.token_;
    trace_id_ = other.trace_id_;
    trace_active_ = std::exchange(other.trace_active_, 0);
    return *this;
}

Profiler::Section::~Section() { close(); }

void Profiler::Section::close() noexcept {
#ifdef STRATA_ENABLE_TRACY
    if (std::exchange(trace_active_, 0) != 0) {
        ___tracy_emit_zone_end(TracyCZoneCtx{trace_id_, 1});
    }
#endif
    Profiler* const profiler = std::exchange(profiler_, nullptr);
    if (profiler == nullptr) return;
    try {
        profiler->close_section(generation_, token_);
    } catch (...) {
        // Profiling teardown must not terminate the profiled operation during stack unwinding.
    }
}

bool Profiler::Section::active() const noexcept { return profiler_ != nullptr; }

Profiler::Frame::Frame(Profiler* const profiler, const std::uint64_t generation) noexcept
    : profiler_(profiler), generation_(generation) {}

Profiler::Frame::Frame(Frame&& other) noexcept
    : profiler_(std::exchange(other.profiler_, nullptr)), generation_(other.generation_) {}

Profiler::Frame& Profiler::Frame::operator=(Frame&& other) noexcept {
    if (this == &other) return *this;
    close();
    profiler_ = std::exchange(other.profiler_, nullptr);
    generation_ = other.generation_;
    return *this;
}

Profiler::Frame::~Frame() { close(); }

void Profiler::Frame::close() noexcept {
    Profiler* const profiler = std::exchange(profiler_, nullptr);
    if (profiler == nullptr) return;
    try {
        profiler->end_frame(generation_);
    } catch (...) {
        // Profiling teardown must not terminate the profiled operation during stack unwinding.
    }
}

bool Profiler::Frame::active() const noexcept { return profiler_ != nullptr; }

Profiler::Profiler(
    const ProfilerScope scope,
    std::string scope_id,
    ProfilerConfig config,
    Clock clock
) : scope_(scope),
    scope_id_(std::move(scope_id)),
    config_(config),
    clock_(std::move(clock)) {
    config_.validate();
    if (scope_id_.empty()) throw std::invalid_argument("profiler scope id must not be empty");
    if (!clock_) clock_ = &steady_now_nanos;
}

Profiler::Frame Profiler::frame(const std::uint64_t frame_index) {
    begin_frame(frame_index);
    std::scoped_lock lock(mutex_);
    return frame_active_ && owner_thread_ == std::this_thread::get_id()
        ? Frame(this, generation_)
        : Frame{};
}

Profiler::Section Profiler::section(const std::string_view name) {
    if (!valid_section_name(name)) {
        throw std::invalid_argument("profiler section name must be non-empty and contain no slash");
    }
    const std::int64_t started_at = now();
    std::scoped_lock lock(mutex_);
    if (!enabled_ || !frame_active_ || owner_thread_ != std::this_thread::get_id()) return {};
    const std::optional<std::uint64_t> parent = open_sections_.empty()
        ? std::nullopt
        : std::optional(open_sections_.back().section_id);
    const std::optional<std::uint64_t> id = find_or_create_section_locked(name, parent);
    if (!id.has_value()) {
        increment_saturated(dropped_section_samples_);
        return {};
    }
    if (next_token_ == std::numeric_limits<std::uint64_t>::max()) {
        increment_saturated(dropped_timing_samples_);
        return {};
    }
    const std::uint64_t token = next_token_++;
    open_sections_.push_back(OpenSection{token, *id, started_at, generation_});
    return Section(this, generation_, token, name);
}

void Profiler::set_capture_enabled(const bool enabled) {
    const std::int64_t ended_at = now();
    std::scoped_lock lock(mutex_);
    if (enabled_ == enabled) return;
    if (frame_active_) {
        while (!open_sections_.empty()) close_top_locked(ended_at);
        frame_active_ = false;
        owner_thread_ = {};
    }
    enabled_ = enabled;
    ++generation_;
    if (!frame_active_) publish_completed_snapshot_locked();
}

bool Profiler::capture_enabled() const noexcept {
    std::scoped_lock lock(mutex_);
    return enabled_;
}

void Profiler::increment(const ProfilerCounter counter, const std::uint64_t amount) {
    const std::size_t index = static_cast<std::size_t>(counter);
    if (index >= counters_.size()) throw std::invalid_argument("profiler counter identity is invalid");
    std::scoped_lock lock(mutex_);
    if (!enabled_) return;
    increment_saturated(counters_[index], amount);
    if (!frame_active_) invalidate_completed_snapshot_locked();
}

void Profiler::record(const ProfilerCounter counter, const std::uint64_t value) {
    const std::size_t index = static_cast<std::size_t>(counter);
    if (index >= counters_.size()) throw std::invalid_argument("profiler counter identity is invalid");
    std::scoped_lock lock(mutex_);
    if (enabled_) {
        counters_[index] = value;
        if (!frame_active_) invalidate_completed_snapshot_locked();
    }
}

void Profiler::record_counters(
    const std::initializer_list<std::pair<ProfilerCounter, std::uint64_t>> values
) {
    std::scoped_lock lock(mutex_);
    if (!enabled_) return;
    for (const auto& [counter, value] : values) {
        const std::size_t index = static_cast<std::size_t>(counter);
        if (index >= counters_.size()) {
            throw std::invalid_argument("profiler counter identity is invalid");
        }
        counters_[index] = value;
    }
    if (!frame_active_) invalidate_completed_snapshot_locked();
}

void Profiler::record_host_frame(const HostFrameProfilerTelemetry& value) {
    if (value.submit_nanos < 0) {
        throw std::invalid_argument("host frame profiler duration must be non-negative");
    }
    std::scoped_lock lock(mutex_);
    if (!enabled_) return;
    const auto set = [this](const ProfilerCounter counter, const std::uint64_t amount) {
        counters_[static_cast<std::size_t>(counter)] = amount;
    };
    set(ProfilerCounter::draw_calls, value.draw_calls);
    set(ProfilerCounter::batches, value.batches);
    set(ProfilerCounter::vertices, value.vertices);
    set(ProfilerCounter::texture_creations, value.texture_creations);
    set(ProfilerCounter::texture_creation_bytes, value.texture_creation_bytes);
    set(ProfilerCounter::texture_uploads, value.texture_uploads);
    set(ProfilerCounter::texture_upload_bytes, value.texture_upload_bytes);
    set(ProfilerCounter::pending_texture_creations, value.queued_texture_creations);
    set(ProfilerCounter::queued_texture_uploads, value.queued_texture_uploads);
    set(ProfilerCounter::queued_texture_upload_bytes, value.queued_texture_upload_bytes);
    set(ProfilerCounter::deferred_gpu_resources, value.deferred_gpu_resources);
    set(
        ProfilerCounter::deferred_gpu_resource_deletions,
        value.deferred_gpu_resource_deletions
    );
    set(ProfilerCounter::blur_passes, value.blur_passes);
    set(ProfilerCounter::blur_target_width, value.blur_target_width);
    set(ProfilerCounter::blur_target_height, value.blur_target_height);
    set(ProfilerCounter::blur_nanos, value.blur_nanos);
    set(ProfilerCounter::host_submit_nanos, static_cast<std::uint64_t>(value.submit_nanos));
    increment_saturated(counters_[static_cast<std::size_t>(ProfilerCounter::host_submissions)]);

    if (!frame_root_id_.has_value()) {
        frame_root_id_ = find_or_create_section_locked("frame", std::nullopt);
    }
    if (!frame_root_id_.has_value()) {
        increment_saturated(dropped_section_samples_);
    } else if (const std::optional<std::uint64_t> id =
                   find_or_create_section_locked("host-submit", frame_root_id_);
               id.has_value()) {
        record_timing_locked(*id, value.submit_nanos);
    } else {
        increment_saturated(dropped_section_samples_);
    }
    if (!frame_active_) invalidate_completed_snapshot_locked();
}

void Profiler::record_external_timing(
    const std::string_view name,
    const std::int64_t duration_nanos
) {
    if (!valid_section_name(name) || duration_nanos < 0) {
        throw std::invalid_argument("external profiler timing requires a valid name and duration");
    }
    std::scoped_lock lock(mutex_);
    if (!enabled_) return;
    if (!frame_root_id_.has_value()) {
        frame_root_id_ = find_or_create_section_locked("frame", std::nullopt);
    }
    if (!frame_root_id_.has_value()) {
        increment_saturated(dropped_section_samples_);
        return;
    }
    const std::optional<std::uint64_t> id = find_or_create_section_locked(name, frame_root_id_);
    if (!id.has_value()) {
        increment_saturated(dropped_section_samples_);
        return;
    }
    record_timing_locked(*id, duration_nanos);
    if (!frame_active_) invalidate_completed_snapshot_locked();
}

ProfilerSnapshot Profiler::snapshot() const {
    std::scoped_lock lock(mutex_);
    if (completed_snapshot_dirty_) publish_completed_snapshot_locked();
    if (has_completed_snapshot_) {
        ProfilerSnapshot result = completed_snapshot_;
        result.capture_enabled = enabled_;
        result.dropped_section_samples = dropped_section_samples_;
        result.dropped_timing_samples = dropped_timing_samples_;
        result.dropped_spikes = dropped_spikes_;
        return result;
    }
    return ProfilerSnapshot{
        scope_, scope_id_, 0U, enabled_, dropped_section_samples_, dropped_timing_samples_,
        dropped_spikes_, {}, {}, {},
    };
}

ProfilerSnapshot Profiler::snapshot_locked() const {
    ProfilerSnapshot result{
        scope_, scope_id_, frame_index_, enabled_, dropped_section_samples_,
        dropped_timing_samples_, dropped_spikes_, {}, counters_locked(false),
        std::vector<ProfilerSpikeSnapshot>(spikes_.begin(), spikes_.end()),
    };
    result.sections.reserve(sections_.size());
    for (const SectionRecord& section : sections_) {
        result.sections.push_back(ProfilerSectionSnapshot{
            section.id,
            section.parent_id,
            section.name,
            section.path,
            section.timings.samples.size(),
            section.last_sample_frame_index,
            section.last_sample_frame_index == frame_index_ && !section.timings.samples.empty()
                ? section.timings.samples.back()
                : 0,
            section.timings.average(),
            section.timings.percentile(0.50),
            section.timings.percentile(0.95),
            section.timings.percentile(0.99),
            section.timings.maximum(),
        });
    }
    return result;
}

void Profiler::invalidate_completed_snapshot_locked() {
    if (has_completed_snapshot_) {
        completed_snapshot_dirty_ = true;
    } else {
        publish_completed_snapshot_locked();
    }
}

void Profiler::publish_completed_snapshot_locked() const {
    completed_snapshot_ = snapshot_locked();
    has_completed_snapshot_ = true;
    completed_snapshot_dirty_ = false;
}

void Profiler::reset() {
    std::scoped_lock lock(mutex_);
    frame_active_ = false;
    owner_thread_ = {};
    frame_index_ = 0U;
    ++generation_;
    next_section_id_ = 1U;
    next_token_ = 1U;
    frame_root_id_.reset();
    sections_.clear();
    section_ids_.clear();
    open_sections_.clear();
    reset_counters_locked();
    spikes_.clear();
    dropped_section_samples_ = 0U;
    dropped_timing_samples_ = 0U;
    dropped_spikes_ = 0U;
    completed_snapshot_ = {};
    has_completed_snapshot_ = false;
    completed_snapshot_dirty_ = false;
}

std::int64_t Profiler::now() const noexcept {
    try {
        const std::int64_t value = clock_();
        return std::max<std::int64_t>(0, value);
    } catch (...) {
        return 0;
    }
}

void Profiler::begin_frame(const std::uint64_t frame_index) {
    const std::int64_t started_at = now();
    std::scoped_lock lock(mutex_);
    if (!enabled_) return;
    if (frame_active_) {
        if (owner_thread_ != std::this_thread::get_id()) {
            increment_saturated(dropped_timing_samples_);
            return;
        }
        while (!open_sections_.empty()) close_top_locked(started_at);
        frame_active_ = false;
        owner_thread_ = {};
        publish_completed_snapshot_locked();
    }
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        increment_saturated(dropped_timing_samples_);
        return;
    }
    if (next_token_ == std::numeric_limits<std::uint64_t>::max()) {
        increment_saturated(dropped_timing_samples_);
        return;
    }
    ++generation_;
    frame_index_ = frame_index;
    frame_active_ = true;
    owner_thread_ = std::this_thread::get_id();
    reset_counters_locked();
    const std::optional<std::uint64_t> root = find_or_create_section_locked("frame", std::nullopt);
    if (!root.has_value()) {
        frame_active_ = false;
        owner_thread_ = {};
        increment_saturated(dropped_section_samples_);
        return;
    }
    frame_root_id_ = root;
    open_sections_.push_back(OpenSection{next_token_++, *root, started_at, generation_});
    counters_[static_cast<std::size_t>(ProfilerCounter::frames)] = 1U;
}

void Profiler::end_frame(const std::uint64_t generation) {
    const std::int64_t ended_at = now();
    std::scoped_lock lock(mutex_);
    if (!frame_active_ || generation != generation_ ||
        owner_thread_ != std::this_thread::get_id()) return;
    while (!open_sections_.empty()) close_top_locked(ended_at);
    frame_active_ = false;
    owner_thread_ = {};
    publish_completed_snapshot_locked();
}

void Profiler::close_section(const std::uint64_t generation, const std::uint64_t token) {
    const std::int64_t ended_at = now();
    std::scoped_lock lock(mutex_);
    if (!frame_active_ || generation != generation_ ||
        owner_thread_ != std::this_thread::get_id()) return;
    const auto found = std::ranges::find(open_sections_, token, &OpenSection::token);
    if (found == open_sections_.end()) return;
    const std::size_t index = static_cast<std::size_t>(found - open_sections_.begin());
    while (open_sections_.size() > index) close_top_locked(ended_at);
}

std::optional<std::uint64_t> Profiler::find_or_create_section_locked(
    const std::string_view name,
    const std::optional<std::uint64_t> parent_id
) {
    SectionKey key{parent_id, std::string(name)};
    if (const auto found = section_ids_.find(key); found != section_ids_.end()) {
        return found->second;
    }
    if (sections_.size() == config_.maximum_sections ||
        next_section_id_ == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    std::string path;
    if (parent_id.has_value()) {
        const SectionRecord* const parent = section_locked(*parent_id);
        if (parent == nullptr) return std::nullopt;
        path = parent->path + "/" + std::string(name);
    } else {
        path = name;
    }
    const std::uint64_t id = next_section_id_++;
    sections_.push_back(SectionRecord{
        id, parent_id, std::string(name), std::move(path),
        RollingWindow(config_.timing_window_size), 0U,
    });
    section_ids_.emplace(std::move(key), id);
    return id;
}

void Profiler::close_top_locked(const std::int64_t ended_at_nanos) {
    if (open_sections_.empty()) return;
    const OpenSection open = open_sections_.back();
    open_sections_.pop_back();
    if (open.generation != generation_) return;
    record_timing_locked(open.section_id, std::max<std::int64_t>(0, ended_at_nanos - open.started_at_nanos));
}

void Profiler::record_timing_locked(
    const std::uint64_t section_id,
    const std::int64_t duration_nanos
) {
    SectionRecord* const section = section_locked(section_id);
    if (section == nullptr) {
        increment_saturated(dropped_timing_samples_);
        return;
    }
    const std::size_t prior_samples = section->timings.samples.size();
    const std::int64_t baseline = section->timings.average();
    const bool spike = prior_samples >= config_.spike_minimum_baseline_samples &&
        baseline > 0 && duration_nanos >= config_.spike_absolute_floor_nanos &&
        static_cast<long double>(duration_nanos) >=
            static_cast<long double>(baseline) * config_.spike_baseline_multiplier;
    if (section->timings.add(duration_nanos)) increment_saturated(dropped_timing_samples_);
    section->last_sample_frame_index = frame_index_;
    if (spike) record_spike_locked(*section, duration_nanos, baseline);
}

void Profiler::record_spike_locked(
    const SectionRecord& section,
    const std::int64_t duration_nanos,
    const std::int64_t baseline_nanos
) {
    const long double adaptive = std::ceil(
        static_cast<long double>(baseline_nanos) * config_.spike_baseline_multiplier
    );
    const std::int64_t adaptive_nanos = adaptive >= std::numeric_limits<std::int64_t>::max()
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(adaptive);
    ProfilerSpikeSnapshot spike{
        section.id,
        section.parent_id,
        section.name,
        section.path,
        stack_locked(section.id),
        frame_index_,
        duration_nanos,
        config_.spike_absolute_floor_nanos,
        std::max(config_.spike_absolute_floor_nanos, adaptive_nanos),
        config_.spike_baseline_multiplier,
        baseline_nanos,
        counters_locked(true),
    };
    if (spikes_.size() == config_.maximum_recent_spikes) {
        spikes_.pop_front();
        increment_saturated(dropped_spikes_);
    }
    spikes_.push_back(std::move(spike));
}

std::vector<std::uint64_t> Profiler::stack_locked(const std::uint64_t section_id) const {
    std::vector<std::uint64_t> result;
    const SectionRecord* current = section_locked(section_id);
    while (current != nullptr) {
        result.push_back(current->id);
        current = current->parent_id.has_value() ? section_locked(*current->parent_id) : nullptr;
    }
    std::ranges::reverse(result);
    return result;
}

std::vector<ProfilerCounterSnapshot> Profiler::counters_locked(const bool nonzero_only) const {
    std::vector<ProfilerCounterSnapshot> result;
    result.reserve(counters_.size());
    for (std::size_t index = 0U; index < counters_.size(); ++index) {
        if (nonzero_only && counters_[index] == 0U) continue;
        result.push_back(ProfilerCounterSnapshot{
            static_cast<ProfilerCounter>(index), counters_[index],
        });
    }
    return result;
}

Profiler::SectionRecord* Profiler::section_locked(const std::uint64_t id) noexcept {
    return id == 0U || id > sections_.size() ? nullptr : &sections_[id - 1U];
}

const Profiler::SectionRecord* Profiler::section_locked(const std::uint64_t id) const noexcept {
    return id == 0U || id > sections_.size() ? nullptr : &sections_[id - 1U];
}

void Profiler::reset_counters_locked() noexcept { counters_.fill(0U); }

void Profiler::increment_saturated(std::uint64_t& value, const std::uint64_t amount) noexcept {
    value = amount > std::numeric_limits<std::uint64_t>::max() - value
        ? std::numeric_limits<std::uint64_t>::max()
        : value + amount;
}

} // namespace strata::runtime
