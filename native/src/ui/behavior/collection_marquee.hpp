#pragma once

namespace strata::ui {

class BehaviorRegistry;
class WidgetInputScope;

/** Registers the reusable collection-marquee pointer/time/presentation-state lifecycle. */
void register_collection_behavior_inputs(BehaviorRegistry& registry);
[[nodiscard]] bool cancel_collection_marquee(WidgetInputScope& scope);

} // namespace strata::ui
