#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>

#include "ui/motion/config.hpp"
#include "ui/motion/runtime_state.hpp"

namespace strata::ui {

MotionRuntime::MotionRuntime() : implementation_(std::make_unique<Impl>()) {}
MotionRuntime::~MotionRuntime() = default;

void MotionRuntime::bind(const std::shared_ptr<const runtime::RuntimeUnit>& unit) {
    if (implementation_->unit == unit) return;
    implementation_->unit = unit;
    implementation_->catalog.bind(unit);
    implementation_->nodes.clear();
    implementation_->active_nodes.clear();
    implementation_->disclosure_players.clear();
    implementation_->disclosure_samples.clear();
    implementation_->reduced_motion.reset();
}

void MotionRuntime::set_supplemental(
    std::map<std::string, CompiledMotion, std::less<>> motions
) {
    implementation_->catalog.set_supplemental(std::move(motions));
    implementation_->nodes.clear();
    implementation_->active_nodes.clear();
    implementation_->disclosure_players.clear();
    implementation_->disclosure_samples.clear();
    implementation_->reduced_motion.reset();
}

bool MotionRuntime::should_retain_for_exit(const RetainedNode& node) {
    bool retain = false;
    const auto visit = [&](auto&& self, const RetainedNode& candidate) -> void {
        const motion_detail::NodeMotionConfig config =
            motion_detail::node_motion_config(candidate, implementation_->catalog);
        if (std::ranges::any_of(config.triggers, [](const auto& binding) {
                return binding.trigger == MotionTrigger::exit && binding.animation != nullptr &&
                       binding.cancel_on_detach &&
                       binding.animation->timing.repeat.kind != MotionRepeatKind::forever;
            })) {
            retain = true;
            return;
        }
        for (const auto& child : candidate.children()) {
            if (!retain) self(self, *child);
        }
    };
    visit(visit, node);
    return retain;
}

bool MotionRuntime::exit_finished(const RetainedNode& node) {
    bool saw_exit = false;
    bool finished = true;
    const auto visit = [&](auto&& self, const RetainedNode& candidate) -> void {
        const motion_detail::NodeMotionConfig config =
            motion_detail::node_motion_config(candidate, implementation_->catalog);
        const auto state = implementation_->nodes.find(candidate.identity());
        for (const motion_detail::TriggerBinding& binding : config.triggers) {
            if (binding.trigger != MotionTrigger::exit || binding.animation == nullptr ||
                !binding.cancel_on_detach ||
                binding.animation->timing.repeat.kind == MotionRepeatKind::forever) {
                continue;
            }
            saw_exit = true;
            if (state == implementation_->nodes.end()) {
                finished = false;
                continue;
            }
            const auto player = state->second.trigger_players.find(MotionTrigger::exit);
            if (player == state->second.trigger_players.end() || !player->second.initialized() ||
                player->second.running()) {
                finished = false;
                continue;
            }
            const double terminal = motion_terminal_progress(
                *binding.animation, binding.active_direction
            );
            if (std::abs(player->second.progress() - terminal) > 1.0e-9) finished = false;
        }
        for (const auto& child : candidate.children()) self(self, *child);
    };
    visit(visit, node);
    return saw_exit && finished;
}

void MotionRuntime::clear() noexcept {
    implementation_->catalog.clear();
    implementation_->unit.reset();
    implementation_->nodes.clear();
    implementation_->active_nodes.clear();
    implementation_->disclosure_players.clear();
    implementation_->disclosure_samples.clear();
    implementation_->reduced_motion.reset();
}

} // namespace strata::ui
