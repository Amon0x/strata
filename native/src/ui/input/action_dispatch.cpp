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
#include "runtime/value.hpp"
#include "ui/command.hpp"
#include "ui/input/detail.hpp"
#include "ui/status.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
using namespace input_detail;

runtime::ActionDispatchOutcome InputRouter::emit(
    JsonValue event,
    const std::shared_ptr<const runtime::ActionValue>& action,
    const RetainedNode& node,
    runtime::Value event_value,
    InputOperationResult& result
) {
    JsonValue outcome_event = event;
    result.events.push_back(std::move(event));
    if (action == nullptr || action->action == nullptr) {
        result.action_outcomes.push_back(no_action_outcome(outcome_event));
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::no_action, std::nullopt, {}, std::nullopt,
        };
    }
    const std::string event_kind =
        outcome_event.find("type") != nullptr && outcome_event.find("type")->string() != nullptr
            ? *outcome_event.find("type")->string()
            : std::string("event");
    std::optional<std::string> lifecycle_owner;
    if (event_kind == "tree-children-requested" && event_value.key() != nullptr &&
        !event_value.key()->value.empty()) {
        lifecycle_owner = "item:" + event_value.key()->value;
    }
    const runtime::ActionEvent action_event{
        event_kind,
        node.description().key,
        std::move(event_value),
        std::move(lifecycle_owner),
    };
    const runtime::ActionDispatchOutcome outcome = execute_action(
        action_event,
        *action,
        node,
        result
    );
    if (outcome.message.has_value() && !outcome.message->empty()) {
        status_feedback_.publish(*outcome.message);
    }
    report_action_outcome(*action->action, outcome, node);
    std::vector<JsonValue> owners;
    for (const std::string& owner : outcome.handler_owners) owners.emplace_back(owner);
    result.action_outcomes.push_back(object({
        {"actionId", outcome.action_id.has_value() ? JsonValue(*outcome.action_id) : JsonValue{}},
        {"event", std::move(outcome_event)},
        {"handlerOwners", array(std::move(owners))},
        {"message", outcome.message.has_value() ? JsonValue(*outcome.message) : JsonValue{}},
        {"status", JsonValue(std::string(status_name(outcome.status)))},
    }));
    return outcome;
}

runtime::ActionDispatchOutcome InputRouter::emit(
    JsonValue event,
    const std::shared_ptr<const runtime::Action>& action,
    const RetainedNode& node,
    runtime::Value event_value,
    InputOperationResult& result
) {
    return emit(
        std::move(event),
        action != nullptr
            ? std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
                  action,
                  std::nullopt,
                  {},
              })
            : nullptr,
        node,
        std::move(event_value),
        result
    );
}

InjectedActionResult InputRouter::dispatch_action(
    std::string action_id,
    runtime::Value payload,
    std::string event_kind,
    std::optional<std::string> source_key,
    runtime::Value event_value,
    const bool dynamic
) {
    if (tree_ == nullptr || tree_->root() == nullptr) {
        throw std::invalid_argument("action injection requires a prepared retained tree");
    }
    if (action_id.empty() || event_kind.empty()) {
        throw std::invalid_argument("action injection requires an action id and event kind");
    }
    RetainedNode* source_node = source_key.has_value()
        ? tree_->find_key(*source_key)
        : tree_->root();
    if (source_node == nullptr) {
        throw std::invalid_argument("action injection source key is not retained by the surface");
    }
    std::shared_ptr<const runtime::Action> action;
    if (dynamic) {
        action = std::make_shared<const runtime::Action>(
            runtime::dynamic_action(std::move(action_id), std::move(payload))
        );
    } else {
        const std::shared_ptr<const runtime::ActionContract> contract =
            application_.bundle()->action_registry().contract(action_id);
        if (contract == nullptr) {
            throw std::invalid_argument("injected typed action is not declared by the application");
        }
        action = std::make_shared<const runtime::Action>(contract, std::move(payload));
    }
    InputOperationResult input;
    JsonValue event = object({
        {"action", canonical_action(*action, *source_node)},
        {"source", source(*source_node)},
        {"type", JsonValue(event_kind)},
        {"value", runtime::value_to_json(event_value)},
    });
    runtime::ActionDispatchOutcome outcome = emit(
        std::move(event), action, *source_node, std::move(event_value), input
    );
    input.injected_events = 1U;
    input.processed_events = 1U;
    return InjectedActionResult{std::move(input), std::move(outcome)};
}

runtime::ActionDispatchOutcome InputRouter::execute_action(
    const runtime::ActionEvent& event,
    const runtime::ActionValue& action,
    const RetainedNode& node,
    InputOperationResult& result
) {
    if (action.action == nullptr) {
        return runtime::ActionDispatchOutcome{
            runtime::ActionDispatchStatus::failed,
            std::nullopt,
            {"strata.surface.declarative"},
            "Composed action contains an empty step.",
        };
    }
    return action.composition.has_value()
               ? execute_composition(event, action, node, result)
               : execute_action(
                     event,
                     *action.action,
                     node,
                     result,
                     action.lexical_state_binding.has_value()
                         ? &*action.lexical_state_binding
                         : nullptr
                 );
}

runtime::ActionDispatchOutcome InputRouter::execute_action(
    const runtime::ActionEvent& event,
    const runtime::Action& action,
    const RetainedNode& node,
    InputOperationResult& result,
    const runtime::LexicalStateBinding* const lexical_state_binding
) {
    if (action.contract->dispatch_policy == runtime::ActionDispatchPolicy::framework) {
        if (action.id() == "form.validate" || action.id() == "form.submit") {
            return execute_form_action(action, result);
        }
        if (std::optional<runtime::ActionDispatchOutcome> outcome =
                execute_surface_action(event, action, node, result);
            outcome.has_value()) {
            return std::move(*outcome);
        }
    }
    return application_.dispatch(
        event,
        action,
        node.description().state_scope,
        lexical_state_binding,
        public_surface_id_,
        frame_time_nanos_
    );
}

runtime::ActionDispatchOutcome InputRouter::execute_composition(
    const runtime::ActionEvent& event,
    const runtime::ActionValue& composition,
    const RetainedNode& node,
    InputOperationResult& result
) {
    application_.undo().begin_group(public_surface_id_);
    std::vector<runtime::ActionDispatchOutcome> outcomes;
    outcomes.reserve(composition.children.size());
    for (std::size_t index = 0U; index < composition.children.size(); ++index) {
        const auto& child = composition.children[index];
        const runtime::ActionDispatchOutcome outcome = child != nullptr
            ? execute_action(event, *child, node, result)
            : runtime::ActionDispatchOutcome{
                  runtime::ActionDispatchStatus::failed,
                  std::nullopt,
                  {},
                  "Composed action contains an empty step.",
              };
        outcomes.push_back(outcome);
        if (*composition.composition == runtime::ActionCompositionMode::sequence &&
            (outcome.status == runtime::ActionDispatchStatus::failed ||
             outcome.status == runtime::ActionDispatchStatus::unhandled)) {
            const std::string id = child != nullptr && child->action != nullptr
                                       ? child->action->id()
                                       : std::string("unknown");
            application_.undo().end_group(public_surface_id_);
            return runtime::ActionDispatchOutcome{
                runtime::ActionDispatchStatus::failed,
                composition.action->id(),
                {"strata.surface.declarative"},
                "Step " + std::to_string(index + 1U) + " '" + id + "' " +
                    std::string(status_name(outcome.status)) + ": " +
                    outcome.message.value_or("no recovery detail"),
            };
        }
    }
    if (*composition.composition == runtime::ActionCompositionMode::parallel) {
        for (std::size_t index = 0U; index < outcomes.size(); ++index) {
            const auto& outcome = outcomes[index];
            if (outcome.status != runtime::ActionDispatchStatus::failed &&
                outcome.status != runtime::ActionDispatchStatus::unhandled) {
                continue;
            }
            const auto& child = composition.children[index];
            const std::string id = child != nullptr && child->action != nullptr
                                       ? child->action->id()
                                       : std::string("unknown");
            application_.undo().end_group(public_surface_id_);
            return runtime::ActionDispatchOutcome{
                runtime::ActionDispatchStatus::failed,
                composition.action->id(),
                {"strata.surface.declarative"},
                "Parallel step " + std::to_string(index + 1U) + " '" + id + "' " +
                    std::string(status_name(outcome.status)) + ": " +
                    outcome.message.value_or("no recovery detail"),
            };
        }
    }
    const bool handled = std::ranges::any_of(outcomes, [](const auto& outcome) {
        return outcome.status == runtime::ActionDispatchStatus::handled ||
               outcome.status == runtime::ActionDispatchStatus::forwarded;
    });
    application_.undo().end_group(public_surface_id_);
    return runtime::ActionDispatchOutcome{
        handled ? runtime::ActionDispatchStatus::handled : runtime::ActionDispatchStatus::ignored,
        composition.action->id(),
        {"strata.surface.declarative"},
        std::nullopt,
    };
}
} // namespace strata::ui
