#include "ui/input.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/action.hpp"
#include "runtime/expression.hpp"
#include "runtime/unit.hpp"
#include "runtime/value.hpp"
#include "ui/command.hpp"
#include "ui/input/detail.hpp"
#include "ui/status.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;

JsonValue InputRouter::canonical_action(
    const runtime::ActionValue& value,
    const RetainedNode& node
) const {
    if (value.action == nullptr) return JsonValue{};
    if (!value.composition.has_value()) return canonical_action(*value.action, node);
    std::vector<JsonValue> actions;
    actions.reserve(value.children.size());
    for (const auto& child : value.children) {
        if (child == nullptr || child->action == nullptr) continue;
        JsonValue encoded = child->composition.has_value()
                                ? canonical_action(*child, node)
                                : canonical_action(*child->action, node, false);
        if (child->composition.has_value() && encoded.object() != nullptr) {
            JsonValue::Object without_origin;
            for (const auto& [name, field_value] : *encoded.object()) {
                if (name != "origin") without_origin.emplace_back(name, field_value);
            }
            encoded = JsonValue(std::move(without_origin));
        }
        actions.push_back(std::move(encoded));
    }
    JsonValue::Object fields;
    fields.emplace_back("dispatchPolicy", JsonValue(std::string(policy_name(value.action->contract->dispatch_policy))));
    fields.emplace_back("dynamic", JsonValue(value.action->dynamic));
    fields.emplace_back("id", JsonValue(value.action->id()));
    fields.emplace_back(
        "origin",
        value.action->origin.has_value() ? origin(*value.action->origin) : JsonValue{}
    );
    fields.emplace_back("payload", object({
        {"actions", array(std::move(actions))},
        {"kind", JsonValue("action-composition")},
        {"mode", JsonValue(
            *value.composition == runtime::ActionCompositionMode::sequence ? "sequence" : "parallel"
        )},
    }));
    fields.emplace_back("payloadContract", JsonValue(value.action->contract->payload_contract));
    return JsonValue(std::move(fields));
}

JsonValue InputRouter::canonical_action(
    const runtime::Action& action,
    const RetainedNode& node,
    const bool include_origin
) const {
    JsonValue payload;
    if (action.id().starts_with("state.")) {
        const runtime::Value* name_value = action.payload.field("name");
        const std::string* name = name_value != nullptr ? name_value->string() : nullptr;
        const std::optional<runtime::StateScopeResolution> binding = name != nullptr
            ? application_.resolve_state_scope(node.description().state_scope, *name)
            : std::nullopt;
        if (binding.has_value()) {
            const std::optional<runtime::Value> declared_initial = application_.state_initial_value(
                *binding,
                node.description().state_scope
            );
            const JsonValue initial = declared_initial.has_value()
                                          ? runtime::value_to_json(*declared_initial)
                                          : JsonValue{};
            if (action.id() == "state.setFromEvent") {
                payload = object({
                    {"address", object({
                        {"name", JsonValue(binding->address.name)},
                        {"scope", JsonValue(binding->address.scope)},
                    })},
                    {"initialValue", initial},
                    {"kind", JsonValue("event-state-mutation")},
                });
            } else {
                std::vector<std::pair<std::string, runtime::Value>> arguments;
                if (action.payload.object() != nullptr) arguments = action.payload.object()->fields;
                payload = object({
                    {"address", object({
                        {"name", JsonValue(binding->address.name)},
                        {"scope", JsonValue(binding->address.scope)},
                    })},
                    {"arguments", runtime::value_to_json(runtime::Value(std::move(arguments)))},
                    {"declaredType", binding->declaration->declared_type.has_value()
                                         ? JsonValue(*binding->declaration->declared_type)
                                         : JsonValue{}},
                    {"initialValue", initial},
                    {"kind", JsonValue("state-mutation")},
                    {"operation", JsonValue(std::string(state_operation(action.id())))},
                });
            }
        }
    }
    if (action.id() == "form.validate" || action.id() == "form.submit") {
        const std::string* key = string_value(action.payload.field("key"));
        if (key != nullptr) {
            if (action.id() == "form.validate") {
                const runtime::Value* focus_value = action.payload.field("focusFirstInvalid");
                const runtime::Value* notify_value = action.payload.field("notify");
                payload = object({
                    {"focusFirstInvalid", JsonValue(
                        focus_value == nullptr || focus_value->boolean() == nullptr || *focus_value->boolean()
                    )},
                    {"key", JsonValue(*key)},
                    {"kind", JsonValue("form-validate-request")},
                    {"notify", JsonValue(
                        notify_value != nullptr && notify_value->boolean() != nullptr && *notify_value->boolean()
                    )},
                });
            } else {
                payload = object({
                    {"key", JsonValue(*key)},
                    {"kind", JsonValue("form-submit-request")},
                });
            }
        }
    }
    if (action.id() == "layer.pop" || action.id() == "layer.push" ||
        action.id() == "layer.replace" || action.id() == "overlay.show" ||
        action.id() == "overlay.hide") {
        std::string operation;
        if (action.id() == "layer.pop") operation = "pop";
        else if (action.id() == "layer.push") operation = "push";
        else if (action.id() == "layer.replace") operation = "replace";
        else if (action.id() == "overlay.show") operation = "show";
        else operation = "hide";
        const std::string* name = string_value(action.payload.field("name"));
        const std::string* transition_name = string_value(action.payload.field("transition"));
        JsonValue transition;
        if (transition_name != nullptr && application_.active_unit() != nullptr) {
            const data::JsonView declaration =
                application_.active_unit()->animation(*transition_name);
            const data::JsonView timing = declaration.find("animation").find("timing");
            const std::optional<std::int64_t> delay = timing.find("delayNanos").integer();
            const std::optional<std::int64_t> duration =
                timing.find("durationNanos").integer();
            if (delay.has_value() && duration.has_value()) {
                transition = object({
                    {"delayNanos", JsonValue(*delay)},
                    {"durationNanos", JsonValue(*duration)},
                    {"name", JsonValue(*transition_name)},
                });
            }
        }
        payload = object({
            {"kind", JsonValue("layer-request")},
            {"name", name != nullptr ? JsonValue(*name) : JsonValue{}},
            {"operation", JsonValue(std::move(operation))},
            {"transition", std::move(transition)},
        });
    }
    if (action.id() == "focus.request") {
        const std::string* key = string_value(action.payload.field("key"));
        payload = object({
            {"key", key != nullptr ? JsonValue(*key) : JsonValue{}},
            {"kind", JsonValue("focus-request")},
        });
    } else if (action.id() == "focus.clear") {
        payload = object({{"kind", JsonValue("focus-clear")}});
    } else if (action.id() == "reveal.request") {
        const std::string* key = string_value(action.payload.field("key"));
        const std::string* scroll = string_value(action.payload.field("scroll"));
        payload = object({
            {"focus", JsonValue(boolean_value(action.payload.field("focus"), false))},
            {"key", key != nullptr ? JsonValue(*key) : JsonValue{}},
            {"kind", JsonValue("reveal-request")},
            {"padding", JsonValue(number_value(action.payload.field("padding"), 6.0))},
            {"scrollKey", scroll != nullptr ? JsonValue(*scroll) : JsonValue{}},
        });
    } else if (action.id() == "environment.set") {
        const std::string* density_value = string_value(action.payload.field("density"));
        const runtime::Value* reduced = action.payload.field("reducedMotion");
        const bool has_safe_insets = action.payload.field("safeLeft") != nullptr ||
                                     action.payload.field("safeTop") != nullptr ||
                                     action.payload.field("safeRight") != nullptr ||
                                     action.payload.field("safeBottom") != nullptr;
        payload = object({
            {"density", density_value != nullptr
                            ? JsonValue(lower_ascii(*density_value))
                            : JsonValue{}},
            {"kind", JsonValue("environment-request")},
            {"reducedMotion", reduced != nullptr && reduced->boolean() != nullptr
                                  ? JsonValue(*reduced->boolean())
                                  : JsonValue{}},
            {"safeInsets", has_safe_insets
                               ? object({
                                     {"bottom", JsonValue(number_value(action.payload.field("safeBottom"), 0.0))},
                                     {"left", JsonValue(number_value(action.payload.field("safeLeft"), 0.0))},
                                     {"right", JsonValue(number_value(action.payload.field("safeRight"), 0.0))},
                                     {"top", JsonValue(number_value(action.payload.field("safeTop"), 0.0))},
                                 })
                               : JsonValue{}},
        });
    } else if (action.id().starts_with("tree.")) {
        const std::string* tree = string_value(action.payload.field("tree"));
        const std::string* item = string_value(action.payload.field("item"));
        payload = object({
            {"itemKey", item != nullptr ? JsonValue(*item) : JsonValue{}},
            {"kind", JsonValue("tree-control-request")},
            {"operation", JsonValue(action.id().substr(std::string("tree.").size()))},
            {"treeKey", tree != nullptr ? JsonValue(*tree) : JsonValue{}},
        });
    } else if (action.id() == "command.execute") {
        const std::string* id = string_value(action.payload.field("id"));
        payload = object({
            {"id", id != nullptr ? JsonValue(*id) : JsonValue{}},
            {"kind", JsonValue("command-execute-request")},
        });
    } else if (action.id() == "palette.set") {
        const std::string* key = string_value(action.payload.field("key"));
        payload = object({
            {"key", key != nullptr ? JsonValue(*key) : JsonValue{}},
            {"kind", JsonValue("palette-request")},
            {"open", JsonValue(boolean_value(action.payload.field("open"), true))},
        });
    } else if (action.id() == "notification.raise") {
        const std::string* message = string_value(action.payload.field("message"));
        const std::string* severity = string_value(action.payload.field("severity"));
        const runtime::Value* timeout = action.payload.field("timeoutMillis");
        const bool persistent = boolean_value(action.payload.field("persistent"), false);
        payload = object({
            {"action", JsonValue{}},
            {"actionLabel", JsonValue{}},
            {"kind", JsonValue("notification-request")},
            {"message", message != nullptr ? JsonValue(*message) : JsonValue{}},
            {"severity", JsonValue(severity != nullptr ? lower_ascii(*severity) : "info")},
            {"timeoutMillis", timeout != nullptr && timeout->number() != nullptr
                                  ? JsonValue(static_cast<std::int64_t>(*timeout->number()))
                                  : JsonValue{}},
            {"timeoutPolicy", JsonValue(
                persistent ? "persistent" : timeout != nullptr ? "after_millis" : "default"
            )},
        });
    }
    if (payload.is_null()) {
        payload = action.contract->payload_contract == "no payload"
                      ? object({{"kind", JsonValue("unit")}})
                      : runtime::value_to_json(action.payload);
    }
    JsonValue::Object fields;
    fields.emplace_back("dispatchPolicy", JsonValue(std::string(policy_name(action.contract->dispatch_policy))));
    fields.emplace_back("dynamic", JsonValue(action.dynamic));
    fields.emplace_back("id", JsonValue(action.id()));
    if (include_origin) {
        fields.emplace_back(
            "origin",
            action.origin.has_value() ? origin(*action.origin) : JsonValue{}
        );
    }
    fields.emplace_back("payload", std::move(payload));
    fields.emplace_back("payloadContract", JsonValue(action.contract->payload_contract));
    return JsonValue(std::move(fields));
}
} // namespace strata::ui
