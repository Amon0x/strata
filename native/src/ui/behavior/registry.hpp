#pragma once

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace strata::ui {

class InputPresentation;
class BehaviorInputScope;
class RetainedNode;
class WidgetRenderScope;
struct DescriptionBehavior;

using BehaviorInputHook = std::function<bool(BehaviorInputScope& scope)>;
using BehaviorOverlayPredicate = std::function<bool(
    const RetainedNode& node,
    const DescriptionBehavior& attachment,
    const InputPresentation& input
)>;
using BehaviorOverlayHook = std::function<void(
    const DescriptionBehavior& attachment,
    WidgetRenderScope& scope
)>;

struct BehaviorInputPhase final {
    /** Generic capture/target/bubble hook sharing the widget dispatch context. */
    BehaviorInputHook event = nullptr;
    /** `event` may consume a wheel, so a wheel over its node has to reach it on the UI thread. */
    bool takes_wheel = false;
    BehaviorInputHook pointer = nullptr;
    BehaviorInputHook key = nullptr;
    BehaviorInputHook advance = nullptr;
    BehaviorInputHook after_layout = nullptr;
    bool focusable = false;
    bool accepts_pointer = false;
    bool disabled = false;
};

struct BehaviorPresentPhase final {
    BehaviorOverlayPredicate has_overlay = nullptr;
    BehaviorOverlayHook overlay = nullptr;
    bool detached_overlay = false;
};

struct BehaviorLifecycle final {
    std::string id;
    BehaviorInputPhase input;
    BehaviorPresentPhase present;
};

/** Surface-owned behavior lifecycle table shared by input and presentation engines. */
class BehaviorRegistry final {
public:
    BehaviorRegistry();

    [[nodiscard]] const BehaviorLifecycle* find(std::string_view id) const noexcept;
    /** Only extensions can take the wheel in a behavior, so most surfaces never look for one. */
    [[nodiscard]] bool any_takes_wheel() const noexcept {
        return std::ranges::any_of(lifecycles_, [](const auto& entry) {
            return entry.second.input.takes_wheel;
        });
    }
    void register_lifecycle(BehaviorLifecycle lifecycle);
    void register_input_phase(std::string id, BehaviorInputPhase phase);
    void register_present_phase(std::string id, BehaviorPresentPhase phase);

private:
    [[nodiscard]] BehaviorLifecycle& lifecycle(std::string id);
    std::map<std::string, BehaviorLifecycle, std::less<>> lifecycles_;
};

void register_builtin_behavior_presenters(BehaviorRegistry& registry);
void register_builtin_behavior_inputs(BehaviorRegistry& registry);

} // namespace strata::ui
