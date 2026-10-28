#include "ui/input.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/action.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/command.hpp"
#include "ui/input/detail.hpp"
#include "ui/status.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;
namespace {

/** Characters in valid UTF-8 text: every byte that is not a continuation byte starts one. */
[[nodiscard]] std::size_t code_point_count(const std::string_view text) noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(text, [](const char byte) {
        return (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U;
    }));
}

/** A minLength/maxLength bound, ignored when it is not a usable count (as TextBox does). */
[[nodiscard]] std::optional<std::size_t> length_bound(const runtime::Value* value) noexcept {
    if (value == nullptr || value->number() == nullptr || !std::isfinite(*value->number()) ||
        *value->number() < 0.0 ||
        *value->number() > static_cast<double>(std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(*value->number());
}

[[nodiscard]] std::string validation_message(
    const RetainedNode& field,
    const std::string_view property,
    const std::string_view fallback
) {
    const std::string* authored = string_value(scalar_property(field, property));
    return authored != nullptr && !authored->empty() ? *authored : std::string(fallback);
}

[[nodiscard]] const runtime::Value* effective_control_value(
    const RetainedNode& input,
    const std::string_view controlled,
    const std::string_view retained,
    const std::string_view uncontrolled
) {
    const runtime::Value* value = scalar_property(input, controlled);
    if (value == nullptr || value->kind() == runtime::ValueKind::null_value) {
        value = input.retained_value(retained);
    }
    if (value == nullptr || value->kind() == runtime::ValueKind::null_value) {
        value = scalar_property(input, uncontrolled);
    }
    return value;
}

} // namespace

bool InputRouter::framework_action_target(const RetainedNode& node,
                                          const std::string_view action_namespace) const noexcept {
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    return lifecycle != nullptr && lifecycle->input.framework_actions == action_namespace;
}

WidgetFormRole InputRouter::form_role(const RetainedNode& node) const noexcept {
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    return lifecycle != nullptr ? lifecycle->input.form_role : WidgetFormRole::none;
}

RetainedNode* InputRouter::form_ancestor(RetainedNode& node,
                                         const WidgetFormRole role) const noexcept {
    for (RetainedNode* current = &node; current != nullptr; current = current->parent()) {
        if (form_role(*current) == role) return current;
    }
    return nullptr;
}

bool InputRouter::validate_field(RetainedNode& field, const bool mark_touched) {
    const runtime::Value* enabled = scalar_property(field, "enabled");
    if (enabled != nullptr && enabled->boolean() != nullptr && !*enabled->boolean()) {
        const bool validation_changed = tree_->set_retained_value(
            field.identity(), "strata.form.validated", runtime::Value(false),
            DirtyReason::semantics
        );
        const bool error_changed = tree_->set_retained_value(
            field.identity(), "strata.form.error", runtime::Value{}, DirtyReason::semantics
        );
        if ((validation_changed || error_changed) && description_invalidator_) {
            description_invalidator_(nullptr, {});
        }
        return true;
    }
    const std::string* input_key = string_value(scalar_property(field, "inputKey"));
    RetainedNode* input = input_key != nullptr ? tree_->find_key(*input_key) : nullptr;
    std::optional<std::string> text;
    std::optional<double> number;
    if (input != nullptr) {
        if (const WidgetLifecycle* lifecycle = widgets_.find(input->description().type);
            lifecycle != nullptr &&
            lifecycle->input.text_edit_mode == WidgetTextEditMode::numeric) {
            const runtime::Value* value = effective_control_value(
                *input, "value", "$value", "defaultValue"
            );
            if (value != nullptr && value->number() != nullptr) number = *value->number();
        } else if (const auto editor = editors_.find(input->identity()); editor != editors_.end()) {
            text = editor->second.text();
        } else {
            const runtime::Value* value = effective_control_value(*input, "text", "$text", "text");
            if (value != nullptr && value->string() != nullptr) text = *value->string();
        }
    }

    std::optional<std::string> error;
    const runtime::Value* required_value = scalar_property(field, "required");
    if (required_value != nullptr && required_value->boolean() != nullptr && *required_value->boolean() &&
        ((!text.has_value() && !number.has_value()) ||
         (text.has_value() && (text->empty() || blank(*text))))) {
        error = validation_message(field, "requiredMessage", "This field is required.");
    }
    // Lengths count characters, matching the TextBox maxLength that limits what can be typed.
    const std::optional<std::size_t> minimum = length_bound(scalar_property(field, "minLength"));
    if (!error.has_value() && minimum.has_value() && text.has_value() && !text->empty() &&
        code_point_count(*text) < *minimum) {
        error = validation_message(field, "minLengthMessage", "Value is too short.");
    }
    const std::optional<std::size_t> maximum = length_bound(scalar_property(field, "maxLength"));
    if (!error.has_value() && maximum.has_value() && text.has_value() &&
        code_point_count(*text) > *maximum) {
        error = validation_message(field, "maxLengthMessage", "Value is too long.");
    }
    const runtime::Value* minimum_number = scalar_property(field, "min");
    const runtime::Value* maximum_number = scalar_property(field, "max");
    if (!error.has_value() && number.has_value() &&
        ((minimum_number != nullptr && minimum_number->number() != nullptr &&
          *number < *minimum_number->number()) ||
         (maximum_number != nullptr && maximum_number->number() != nullptr &&
          *number > *maximum_number->number()))) {
        error = validation_message(field, "rangeMessage", "Value is outside the allowed range.");
    }
    bool description_changed = tree_->set_retained_value(
        field.identity(), "strata.form.validated", runtime::Value(true), DirtyReason::semantics
    );
    if (mark_touched) {
        description_changed = tree_->set_retained_value(
            field.identity(), "strata.form.touched", runtime::Value(true), DirtyReason::semantics
        ) || description_changed;
    }
    description_changed = tree_->set_retained_value(
        field.identity(), "strata.form.error",
        error.has_value() ? runtime::Value(*error) : runtime::Value{}, DirtyReason::semantics
    ) || description_changed;
    if (description_changed && description_invalidator_) {
        description_invalidator_(nullptr, {});
    }
    return !error.has_value();
}

std::size_t InputRouter::validate_form(
    RetainedNode& form,
    const bool focus_first_invalid,
    InputOperationResult& result
) {
    std::vector<RetainedNode*> fields;
    const auto collect = [this, &fields](const auto& self, RetainedNode& node) -> void {
        if (form_role(node) == WidgetFormRole::field) fields.push_back(&node);
        for (const auto& child : node.children()) self(self, *child);
    };
    collect(collect, form);
    RetainedNode* first_invalid = nullptr;
    std::size_t invalid_count = 0U;
    for (RetainedNode* field : fields) {
        if (validate_field(*field, true)) continue;
        ++invalid_count;
        if (first_invalid == nullptr) first_invalid = field;
    }
    if (focus_first_invalid && first_invalid != nullptr) {
        const std::string* input_key = string_value(scalar_property(*first_invalid, "inputKey"));
        if (input_key != nullptr) {
            if (RetainedNode* input = tree_->find_key(*input_key); input != nullptr) {
                focus(*input, "programmatic", result);
            }
        }
    }
    return invalid_count;
}

void InputRouter::note_field_change(RetainedNode& node) {
    RetainedNode* field = form_ancestor(node, WidgetFormRole::field);
    if (field == nullptr) return;
    static_cast<void>(tree_->set_retained_value(
        field->identity(), "strata.form.dirty", runtime::Value(true), DirtyReason::semantics
    ));
    RetainedNode* form = form_ancestor(*field, WidgetFormRole::form);
    if (form == nullptr) return;
    const runtime::Value* validated = field->retained_value("strata.form.validated");
    const bool previously_validated = validated != nullptr && validated->boolean() != nullptr &&
                                      *validated->boolean();
    const runtime::Value* validate_on_change = scalar_property(*form, "validateOnChange");
    const runtime::Value* revalidate = scalar_property(*form, "revalidateAfterFirstValidation");
    if ((validate_on_change != nullptr && validate_on_change->boolean() != nullptr &&
         *validate_on_change->boolean()) ||
        (previously_validated && (revalidate == nullptr || revalidate->boolean() == nullptr ||
                                  *revalidate->boolean()))) {
        static_cast<void>(validate_field(*field, false));
    }
}

void InputRouter::note_field_blur(RetainedNode& node) {
    RetainedNode* field = form_ancestor(node, WidgetFormRole::field);
    if (field == nullptr) return;
    static_cast<void>(tree_->set_retained_value(
        field->identity(), "strata.form.touched", runtime::Value(true), DirtyReason::semantics
    ));
    RetainedNode* form = form_ancestor(*field, WidgetFormRole::form);
    const runtime::Value* validate_on_blur = form != nullptr ? scalar_property(*form, "validateOnBlur") : nullptr;
    if (validate_on_blur == nullptr || validate_on_blur->boolean() == nullptr ||
        *validate_on_blur->boolean()) {
        static_cast<void>(validate_field(*field, true));
    }
}

runtime::ActionDispatchOutcome InputRouter::execute_form_action(
    const runtime::Action& action,
    InputOperationResult& result
) {
    const std::string* form_key = string_value(action.payload.field("key"));
    RetainedNode* form = form_key != nullptr ? tree_->find_key(*form_key) : nullptr;
    if (form == nullptr || form_role(*form) != WidgetFormRole::form) {
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::failed,
            action.id(),
            {"strata.surface.declarative"},
            form_key != nullptr ? std::optional("Form '" + *form_key + "' is not attached.")
                                : std::optional<std::string>("Form action requires a keyed form."),
        };
    }
    if (action.id() == "form.validate") {
        const runtime::Value* focus_value = action.payload.field("focusFirstInvalid");
        const bool focus_first = focus_value == nullptr || focus_value->boolean() == nullptr ||
                                 *focus_value->boolean();
        const std::size_t invalid = validate_form(*form, focus_first, result);
        // The summary is published as status feedback, so it is only produced when asked for.
        const runtime::Value* notify_value = action.payload.field("notify");
        const bool notify = notify_value != nullptr && notify_value->boolean() != nullptr &&
                            *notify_value->boolean();
        std::optional<std::string> message;
        if (notify) {
            message = invalid == 0U ? std::string("Form is valid.")
                      : invalid == 1U
                          ? std::string("1 validation error requires attention.")
                          : std::to_string(invalid) + " validation errors require attention.";
        }
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::handled,
            action.id(),
            {"strata.surface.declarative"},
            std::move(message),
        };
    }

    const runtime::Value* validate_on_submit = scalar_property(*form, "validateOnSubmit");
    const bool should_validate = validate_on_submit == nullptr || validate_on_submit->boolean() == nullptr ||
                                 *validate_on_submit->boolean();
    const bool valid = !should_validate || validate_form(*form, true, result) == 0U;
    if (valid) {
        std::shared_ptr<const runtime::ActionValue> submit;
        const auto authored = form->description().properties.find("onSubmit");
        if (authored != form->description().properties.end() &&
            authored->second.action() != nullptr) {
            submit = *authored->second.action();
        } else {
            const std::shared_ptr<const runtime::ActionContract> observation =
                application_.bundle()->action_registry().contract("form-submit");
            if (observation != nullptr) {
                runtime::Value payload = application_.bundle()->action_registry().decode_payload(
                    "form-submit",
                    runtime::Value{}
                );
                submit = std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
                    std::make_shared<const runtime::Action>(observation, std::move(payload)),
                    std::nullopt,
                    {},
                });
            }
        }
        JsonValue event = object({
            {"action", submit != nullptr ? canonical_action(*submit, *form) : JsonValue{}},
            {"source", source(*form)},
            {"type", JsonValue("activated")},
        });
        emit(std::move(event), submit, *form, runtime::Value{}, result);
    }
    return runtime::ActionDispatchOutcome{
        runtime::ActionDispatchStatus::handled,
        action.id(),
        {"strata.surface.declarative"},
        std::nullopt,
    };
}
} // namespace strata::ui
