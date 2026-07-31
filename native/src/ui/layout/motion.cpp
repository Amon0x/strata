#include "ui/layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/layout/detail_algorithms.hpp"
#include "ui/theme.hpp"

namespace strata::ui {
using namespace layout_detail;

namespace {

[[nodiscard]] const runtime::Value* motion_property(
    const DescriptionNode& description,
    const std::string_view name
) noexcept {
    const runtime::Value* layout = scalar_property(description, "$layout");
    if (layout != nullptr && layout->object() != nullptr) {
        if (const runtime::Value* value = layout->field(name); value != nullptr) return value;
    }
    return scalar_property(description, name);
}

} // namespace

std::optional<ContentSizeMotionSpec> content_size_motion(const DescriptionNode& description) {
    const runtime::Value* theme_set = motion_property(description, "$themeAnimationSet");
    if (theme_set != nullptr && theme_set->string() != nullptr &&
        (*theme_set->string() == "present-empty" || *theme_set->string() == "none")) {
        return std::nullopt;
    }
    const runtime::Value* authored = motion_property(description, "animateContentSize");
    const runtime::Value* defaults = motion_property(description, "$contentSizeMotionDefaults");
    const runtime::Value* disclosure = motion_property(description, "disclosure");
    const runtime::Value* disclosure_defaults = motion_property(description, "$disclosureDefaults");
    const bool has_disclosure =
        (disclosure != nullptr && disclosure->kind() != runtime::ValueKind::null_value) ||
        (disclosure_defaults != nullptr &&
         disclosure_defaults->kind() != runtime::ValueKind::null_value);
    const runtime::Value* value = authored != nullptr ? authored : defaults;
    if ((value == nullptr || value->kind() == runtime::ValueKind::null_value) && !has_disclosure) {
        return std::nullopt;
    }
    ContentSizeMotionSpec spec;
    spec.timing = theme_motion_timing(description, default_motion_timing_name);
    if (value != nullptr && value->boolean() != nullptr && !*value->boolean()) {
        return has_disclosure ? std::optional(spec) : std::nullopt;
    }
    if (value == nullptr || value->object() == nullptr) return spec;
    const auto configured = [value, defaults](const std::string_view name) {
        const runtime::Value* result = value->field(name);
        return result == nullptr && defaults != nullptr ? defaults->field(name) : result;
    };
    spec.animate_width = boolean(
        configured("width"), boolean(configured("animateWidth"), false)
    );
    spec.animate_height = boolean(
        configured("height"), boolean(configured("animateHeight"), true)
    );
    spec.clip = boolean(configured("clip"), true);
    const runtime::Value* duration_value = configured("durationNanos");
    const runtime::Value* timing_value = configured("timing");
    if (timing_value == nullptr) timing_value = configured("policy");
    const std::string_view timing_name = timing_value != nullptr && timing_value->string() != nullptr
                                             ? std::string_view(*timing_value->string())
                                             : default_motion_timing_name;
    spec.timing = theme_motion_timing(description, timing_name);
    const double duration = number(duration_value, static_cast<double>(spec.timing.duration_nanos));
    if (std::isfinite(duration) && duration > 0.0 &&
        duration <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        spec.timing.duration_nanos = static_cast<std::int64_t>(duration);
    }
    if (!spec.animate_width && !spec.animate_height) return std::nullopt;
    return spec;
}

ContentSizeMotionSample ContentSizeTransitions::sample(
    const std::uint64_t identity,
    const std::int64_t now_nanos
) {
    const auto found = active_.find(identity);
    if (found == active_.end()) {
        const auto target = targets_.find(identity);
        const Size settled = target != targets_.end() ? target->second : Size{};
        return ContentSizeMotionSample{.size = settled, .target = settled};
    }
    const Active active = found->second;
    const double elapsed = std::max(
        0.0,
        static_cast<double>(now_nanos) - static_cast<double>(active.started_nanos)
    );
    const double active_elapsed = std::max(
        0.0,
        elapsed - static_cast<double>(active.spec.timing.delay_nanos)
    );
    const double linear = std::clamp(
        active_elapsed / static_cast<double>(active.spec.timing.duration_nanos),
        0.0,
        1.0
    );
    const double eased = motion_easing(active.spec.timing.easing, linear);
    ContentSizeMotionSample result{
        .size = Size{
            active.from.width + (active.target.width - active.from.width) * eased,
            active.from.height + (active.target.height - active.from.height) * eased,
        },
        .target = active.target,
        .progress = linear,
        .running = linear < 1.0,
        .clip = active.spec.clip,
        .snapped_by_reduced_motion = false,
    };
    if (!result.running) active_.erase(found);
    return result;
}

ContentSizeMotionSample ContentSizeTransitions::retarget(
    const std::uint64_t identity,
    const Size target,
    const std::int64_t now_nanos,
    const ContentSizeMotionSpec& spec,
    const bool reduced_motion
) {
    if (!finite_non_negative(target.width) || !finite_non_negative(target.height) ||
        spec.timing.duration_nanos <= 0 || spec.timing.delay_nanos < 0 ||
        spec.timing.repeat.kind == MotionRepeatKind::count &&
            spec.timing.repeat.iterations == 0U) {
        throw std::invalid_argument("content-size motion requires finite target geometry and positive duration");
    }
    const auto active = active_.find(identity);
    if (active != active_.end() && active->second.target == target && active->second.spec == spec &&
        !reduced_motion) {
        return sample(identity, now_nanos);
    }
    Size current = target;
    if (active != active_.end()) current = sample(identity, now_nanos).size;
    else if (const auto previous = targets_.find(identity); previous != targets_.end()) current = previous->second;
    targets_.insert_or_assign(identity, target);
    const bool width_changed = spec.animate_width && current.width != target.width;
    const bool height_changed = spec.animate_height && current.height != target.height;
    if (reduced_motion || (!width_changed && !height_changed)) {
        active_.erase(identity);
        return ContentSizeMotionSample{
            .size = target,
            .target = target,
            .progress = 1.0,
            .running = false,
            .clip = false,
            .snapped_by_reduced_motion = reduced_motion && (width_changed || height_changed),
        };
    }
    if (!spec.animate_width) current.width = target.width;
    if (!spec.animate_height) current.height = target.height;
    active_.insert_or_assign(identity, Active{current, target, now_nanos, spec});
    return sample(identity, now_nanos);
}

std::size_t ContentSizeTransitions::active_count() const noexcept { return active_.size(); }

std::vector<std::uint64_t> ContentSizeTransitions::active_identities() const {
    std::vector<std::uint64_t> result;
    result.reserve(active_.size());
    for (const auto& [identity, active] : active_) {
        static_cast<void>(active);
        result.push_back(identity);
    }
    return result;
}

void ContentSizeTransitions::remove(const std::uint64_t identity) noexcept {
    targets_.erase(identity);
    active_.erase(identity);
}

void ContentSizeTransitions::retain(const std::map<std::uint64_t, LayoutRecord>& records) {
    std::erase_if(targets_, [&records](const auto& value) { return !records.contains(value.first); });
    std::erase_if(active_, [&records](const auto& value) { return !records.contains(value.first); });
}

void ContentSizeTransitions::clear() noexcept {
    targets_.clear();
    active_.clear();
}
} // namespace strata::ui
