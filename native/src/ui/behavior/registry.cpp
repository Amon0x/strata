#include "ui/behavior/registry.hpp"

#include <stdexcept>
#include <utility>

#include "ui/behavior/collection_marquee.hpp"

namespace strata::ui {

BehaviorRegistry::BehaviorRegistry() {
    register_input_phase("strata.focusable", BehaviorInputPhase{.focusable = true});
    register_input_phase("strata.hoverable", BehaviorInputPhase{.accepts_pointer = true});
    register_input_phase("strata.disabled", BehaviorInputPhase{.disabled = true});
    register_input_phase(
        "strata.drag-source",
        BehaviorInputPhase{.focusable = true, .accepts_pointer = true}
    );
    register_input_phase("strata.drop-target", BehaviorInputPhase{.accepts_pointer = true});
    register_input_phase("strata.reorder-target", BehaviorInputPhase{.accepts_pointer = true});
    register_builtin_behavior_inputs(*this);
    register_collection_behavior_inputs(*this);
    register_builtin_behavior_presenters(*this);
}

const BehaviorLifecycle* BehaviorRegistry::find(const std::string_view id) const noexcept {
    const auto found = lifecycles_.find(id);
    return found != lifecycles_.end() ? &found->second : nullptr;
}

void BehaviorRegistry::register_lifecycle(BehaviorLifecycle lifecycle_value) {
    if (lifecycle_value.id.empty()) {
        throw std::invalid_argument("behavior lifecycle id must not be empty");
    }
    const std::string id = lifecycle_value.id;
    if (!lifecycles_.emplace(id, std::move(lifecycle_value)).second) {
        throw std::invalid_argument("duplicate behavior lifecycle for '" + id + "'");
    }
}

BehaviorLifecycle& BehaviorRegistry::lifecycle(std::string id) {
    if (id.empty()) throw std::invalid_argument("behavior lifecycle id must not be empty");
    auto [found, inserted] = lifecycles_.try_emplace(id);
    if (inserted) found->second.id = std::move(id);
    return found->second;
}

void BehaviorRegistry::register_input_phase(std::string id, BehaviorInputPhase phase) {
    lifecycle(std::move(id)).input = std::move(phase);
}

void BehaviorRegistry::register_present_phase(std::string id, BehaviorPresentPhase phase) {
    lifecycle(std::move(id)).present = std::move(phase);
}

} // namespace strata::ui
