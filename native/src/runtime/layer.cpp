#include "runtime/layer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::runtime {
namespace {

void validate_text(const std::string_view value, const std::string_view label) {
    if (value.empty() || !core::valid_utf8(value)) {
        throw std::invalid_argument(std::string(label) + " must be non-empty valid UTF-8");
    }
}

[[nodiscard]] LayerOperationResult failed(std::string message) {
    return LayerOperationResult{LayerOperationStatus::failed, std::move(message)};
}

} // namespace

LayerStack::LayerStack(Invalidation invalidation) : invalidation_(std::move(invalidation)) {}

void LayerStack::push_screen(std::string id, std::optional<std::string> transition) {
    validate_id(id);
    if (transition.has_value()) validate_text(*transition, "layer transition");
    if (std::ranges::find(screens_, id, &Entry::id) != screens_.end()) {
        throw std::invalid_argument("screen layer is already in the stack");
    }
    screens_.push_back(Entry{std::move(id), std::move(transition)});
    changed();
}

void LayerStack::replace_screen(std::string id, std::optional<std::string> transition) {
    validate_id(id);
    if (transition.has_value()) validate_text(*transition, "layer transition");
    if (screens_.empty()) root_replaced_ = true;
    else screens_.pop_back();
    if (std::ranges::find(screens_, id, &Entry::id) != screens_.end()) {
        throw std::invalid_argument("replacement screen layer is already in the stack");
    }
    screens_.push_back(Entry{std::move(id), std::move(transition)});
    changed();
}

std::optional<std::string> LayerStack::pop_screen() {
    if (screens_.empty()) return std::nullopt;
    std::string removed = std::move(screens_.back().id);
    screens_.pop_back();
    if (screens_.empty() && root_replaced_) root_replaced_ = false;
    changed();
    return removed;
}

bool LayerStack::show_overlay(std::string id, std::optional<std::string> transition) {
    validate_id(id);
    if (transition.has_value()) validate_text(*transition, "layer transition");
    const auto found = std::ranges::find(overlays_, id, &Entry::id);
    if (found == overlays_.end()) {
        overlays_.push_back(Entry{std::move(id), std::move(transition)});
    } else {
        found->transition = std::move(transition);
    }
    changed();
    return true;
}

bool LayerStack::hide_overlay(const std::string_view id) {
    const auto found = std::ranges::find(overlays_, id, &Entry::id);
    if (found == overlays_.end()) return false;
    overlays_.erase(found);
    changed();
    return true;
}

bool LayerStack::clear() {
    if (screens_.empty() && overlays_.empty() && !root_replaced_) return false;
    screens_.clear();
    overlays_.clear();
    root_replaced_ = false;
    changed();
    return true;
}

bool LayerStack::retain(const std::set<std::string, std::less<>>& ids) {
    const auto remove_missing = [&ids](std::vector<Entry>& values) {
        const std::size_t previous = values.size();
        std::erase_if(values, [&ids](const Entry& value) { return !ids.contains(value.id); });
        return previous != values.size();
    };
    const bool screens_changed = remove_missing(screens_);
    const bool overlays_changed = remove_missing(overlays_);
    const bool root_changed = root_replaced_ && screens_.empty();
    if (root_changed) root_replaced_ = false;
    const bool did_change = screens_changed || overlays_changed || root_changed;
    if (did_change) changed();
    return did_change;
}

std::optional<std::string_view> LayerStack::active_screen() const noexcept {
    return screens_.empty() ? std::nullopt : std::optional<std::string_view>(screens_.back().id);
}

bool LayerStack::root_replaced() const noexcept { return root_replaced_; }

std::vector<LayerSnapshot> LayerStack::snapshot() const {
    std::vector<LayerSnapshot> result;
    result.reserve(screens_.size() + overlays_.size());
    for (const Entry& entry : screens_) {
        result.push_back(LayerSnapshot{entry.id, LayerRole::screen, entry.transition});
    }
    for (const Entry& entry : overlays_) {
        result.push_back(LayerSnapshot{entry.id, LayerRole::overlay, entry.transition});
    }
    return result;
}

std::uint64_t LayerStack::generation() const noexcept { return generation_; }

void LayerStack::changed() {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("layer generation exhausted");
    }
    ++generation_;
    if (invalidation_) invalidation_();
}

void LayerStack::validate_id(const std::string_view id) { validate_text(id, "layer id"); }

void DeclarativeLayerRegistry::register_screen(std::string name, std::string id) {
    validate_text(name, "declarative screen name");
    validate_text(id, "declarative screen id");
    if (!screens_.emplace(std::move(name), std::move(id)).second) {
        throw std::invalid_argument("duplicate declarative screen");
    }
}

void DeclarativeLayerRegistry::register_overlay(std::string name, std::string id) {
    validate_text(name, "declarative overlay name");
    validate_text(id, "declarative overlay id");
    if (!overlays_.emplace(std::move(name), std::move(id)).second) {
        throw std::invalid_argument("duplicate declarative overlay");
    }
}

bool DeclarativeLayerRegistry::unregister_screen(const std::string_view name) {
    return screens_.erase(std::string(name)) != 0U;
}

bool DeclarativeLayerRegistry::unregister_overlay(const std::string_view name) {
    return overlays_.erase(std::string(name)) != 0U;
}

LayerOperationResult DeclarativeLayerRegistry::execute(
    LayerStack& stack,
    const DeclarativeLayerOperation operation,
    const std::optional<std::string_view> name,
    const std::optional<std::string_view> transition
) const {
    if (operation == DeclarativeLayerOperation::pop) {
        return LayerOperationResult{
            stack.pop_screen().has_value() ? LayerOperationStatus::handled : LayerOperationStatus::ignored,
            std::nullopt,
        };
    }
    if (!name.has_value() || name->empty()) {
        return failed("declarative layer operation requires a registered layer name");
    }
    if (operation == DeclarativeLayerOperation::push ||
        operation == DeclarativeLayerOperation::replace) {
        const auto definition = screens_.find(*name);
        if (definition == screens_.end()) {
            return failed("Screen '" + std::string(*name) + "' is not registered.");
        }
        if (operation == DeclarativeLayerOperation::push) {
            const bool already_attached = std::ranges::any_of(
                stack.snapshot(),
                [&definition](const LayerSnapshot& layer) { return layer.id == definition->second; }
            );
            if (already_attached) {
                return LayerOperationResult{LayerOperationStatus::ignored, std::nullopt};
            }
            stack.push_screen(
                definition->second,
                transition.has_value() ? std::optional<std::string>(*transition) : std::nullopt
            );
        } else {
            stack.replace_screen(
                definition->second,
                transition.has_value() ? std::optional<std::string>(*transition) : std::nullopt
            );
        }
        return LayerOperationResult{LayerOperationStatus::handled, std::nullopt};
    }

    const auto definition = overlays_.find(*name);
    if (definition == overlays_.end()) {
        return failed("Overlay '" + std::string(*name) + "' is not registered.");
    }
    const bool changed = operation == DeclarativeLayerOperation::show
                             ? stack.show_overlay(
                                   definition->second,
                                   transition.has_value()
                                       ? std::optional<std::string>(*transition)
                                       : std::nullopt
                               )
                             : stack.hide_overlay(definition->second);
    return LayerOperationResult{
        changed ? LayerOperationStatus::handled : LayerOperationStatus::ignored,
        std::nullopt,
    };
}

} // namespace strata::runtime
