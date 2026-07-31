#include "runtime/action.hpp"

#include <algorithm>
#include <limits>
#include <map>
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

[[nodiscard]] bool schema_equal(const ValueSchemaPtr& left, const ValueSchemaPtr& right) {
    return left == right || (left != nullptr && right != nullptr && *left == *right);
}

[[nodiscard]] std::string_view policy_name(const ActionDispatchPolicy policy) noexcept {
    switch (policy) {
    case ActionDispatchPolicy::required: return "required";
    case ActionDispatchPolicy::optional: return "optional";
    case ActionDispatchPolicy::broadcast: return "broadcast";
    case ActionDispatchPolicy::forwarded: return "forwarded";
    case ActionDispatchPolicy::framework: return "framework";
    }
    return "required";
}

} // namespace

struct ActionDispatcherState final {
    struct HandlerEntry final {
        std::uint64_t token;
        std::shared_ptr<const ActionContract> contract;
        std::string owner;
        ActionDispatcher::Handler invoke;
    };

    std::map<std::string, std::shared_ptr<const ActionContract>, std::less<>> contracts;
    std::map<std::string, std::vector<HandlerEntry>, std::less<>> handlers;
    std::uint64_t next_token = 1U;

    void remove(const std::uint64_t token) noexcept {
        for (auto iterator = handlers.begin(); iterator != handlers.end(); ++iterator) {
            auto& entries = iterator->second;
            const auto found = std::ranges::find(entries, token, &HandlerEntry::token);
            if (found == entries.end()) continue;
            entries.erase(found);
            if (entries.empty()) handlers.erase(iterator);
            return;
        }
    }
};

bool ActionContract::compatible_with(const ActionContract& other) const {
    return id == other.id && dispatch_policy == other.dispatch_policy &&
           payload_contract == other.payload_contract && schema_equal(payload_schema, other.payload_schema);
}

Action::Action(
    std::shared_ptr<const ActionContract> source_contract,
    Value source_payload,
    std::optional<ActionOrigin> source_origin,
    const bool source_dynamic
)
    : contract(std::move(source_contract)),
      payload(std::move(source_payload)),
      origin(std::move(source_origin)),
      dynamic(source_dynamic) {
    if (contract == nullptr) throw std::invalid_argument("action contract must not be null");
    validate_text(contract->id, "action id");
    if (contract->payload_schema == nullptr || !contract->payload_schema->accepts(payload)) {
        throw std::invalid_argument("action payload does not satisfy its contract");
    }
    if (origin.has_value()) {
        validate_text(origin->source_id, "action origin source id");
        if (origin->line.has_value() && *origin->line == 0U) {
            throw std::invalid_argument("action origin line must be positive");
        }
        if (origin->column.has_value() && *origin->column == 0U) {
            throw std::invalid_argument("action origin column must be positive");
        }
        if (origin->component_path.has_value()) {
            validate_text(*origin->component_path, "action origin component path");
        }
    }
}

const std::string& Action::id() const noexcept { return contract->id; }

Action dynamic_action(
    std::string id,
    Value payload,
    std::optional<ActionOrigin> origin
) {
    const std::string summary = "Dynamic action '" + id + "'";
    return Action(
        std::make_shared<const ActionContract>(ActionContract{
            std::move(id),
            ValueSchema::any(),
            ActionDispatchPolicy::required,
            "dynamic JSON value",
            summary,
        }),
        std::move(payload),
        std::move(origin),
        true
    );
}

ActionRegistration::ActionRegistration(
    std::weak_ptr<ActionDispatcherState> state,
    const std::uint64_t token
) noexcept
    : state_(std::move(state)), token_(token) {}

ActionRegistration::ActionRegistration(ActionRegistration&& other) noexcept
    : state_(std::move(other.state_)), token_(std::exchange(other.token_, 0U)) {}

ActionRegistration& ActionRegistration::operator=(ActionRegistration&& other) noexcept {
    if (this == &other) return *this;
    close();
    state_ = std::move(other.state_);
    token_ = std::exchange(other.token_, 0U);
    return *this;
}

ActionRegistration::~ActionRegistration() { close(); }

void ActionRegistration::close() noexcept {
    if (token_ == 0U) return;
    if (const auto state = state_.lock()) state->remove(token_);
    token_ = 0U;
    state_.reset();
}

ActionDispatcher::ActionDispatcher() : state_(std::make_shared<ActionDispatcherState>()) {}

ActionDispatcher::ActionDispatcher(std::vector<std::shared_ptr<const ActionContract>> contracts)
    : ActionDispatcher() {
    for (auto& next : contracts) declare(std::move(next));
}

void ActionDispatcher::declare(std::shared_ptr<const ActionContract> next_contract) {
    if (next_contract == nullptr || next_contract->payload_schema == nullptr) {
        throw std::invalid_argument("declared action contract and payload schema must not be null");
    }
    validate_text(next_contract->id, "action id");
    validate_text(next_contract->payload_contract, "action payload contract");
    validate_text(next_contract->summary, "action summary");
    const auto found = state_->contracts.find(next_contract->id);
    if (found != state_->contracts.end()) {
        if (!found->second->compatible_with(*next_contract)) {
            throw std::invalid_argument("duplicate action id has an incompatible contract");
        }
        return;
    }
    state_->contracts.emplace(next_contract->id, std::move(next_contract));
}

std::shared_ptr<const ActionContract> ActionDispatcher::contract(const std::string_view id) const {
    const auto found = state_->contracts.find(id);
    return found != state_->contracts.end() ? found->second : nullptr;
}

std::vector<std::string> ActionDispatcher::handler_owners(const std::string_view id) const {
    std::vector<std::string> owners;
    const auto found = state_->handlers.find(id);
    if (found == state_->handlers.end()) return owners;
    owners.reserve(found->second.size());
    for (const ActionDispatcherState::HandlerEntry& handler : found->second) {
        owners.push_back(handler.owner);
    }
    return owners;
}

ActionRegistration ActionDispatcher::register_handler(
    std::shared_ptr<const ActionContract> handler_contract,
    std::string owner,
    Handler handler
) {
    if (handler_contract == nullptr || !handler) {
        throw std::invalid_argument("action handler contract and callback are required");
    }
    validate_text(owner, "action handler owner");
    declare(handler_contract);
    if (handler_contract->dispatch_policy == ActionDispatchPolicy::framework) {
        throw std::invalid_argument("framework actions cannot have host handlers");
    }
    auto& handlers = state_->handlers[handler_contract->id];
    if (handler_contract->dispatch_policy != ActionDispatchPolicy::broadcast && !handlers.empty()) {
        throw std::invalid_argument("non-broadcast action already has a handler");
    }
    if (state_->next_token == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("action registration identity exhausted");
    }
    const std::uint64_t token = state_->next_token++;
    handlers.push_back(ActionDispatcherState::HandlerEntry{
        token,
        std::move(handler_contract),
        std::move(owner),
        std::move(handler),
    });
    return ActionRegistration(state_, token);
}

ActionDispatchOutcome ActionDispatcher::dispatch(
    const ActionEvent& event,
    const Action& action
) const {
    if (action.contract->dispatch_policy == ActionDispatchPolicy::framework) {
        return ActionDispatchOutcome{
            ActionDispatchStatus::failed,
            action.id(),
            {},
            "Framework action '" + action.id() + "' reached host dispatch without being consumed.",
        };
    }
    const auto declared = state_->contracts.find(action.id());
    if (!action.dynamic &&
        (declared == state_->contracts.end() || !declared->second->compatible_with(*action.contract))) {
        return ActionDispatchOutcome{
            ActionDispatchStatus::failed,
            action.id(),
            {},
            "Action '" + action.id() + "' does not match the active host contract.",
        };
    }
    const auto found = state_->handlers.find(action.id());
    if (found == state_->handlers.end() || found->second.empty()) {
        if (!action.dynamic &&
            (action.contract->dispatch_policy == ActionDispatchPolicy::optional ||
             action.contract->dispatch_policy == ActionDispatchPolicy::broadcast)) {
            return ActionDispatchOutcome{
                ActionDispatchStatus::ignored,
                action.id(),
                {},
                "No observer is registered; '" + action.id() + "' is " +
                    std::string(policy_name(action.contract->dispatch_policy)) + ".",
            };
        }
        return ActionDispatchOutcome{
            ActionDispatchStatus::unhandled,
            action.id(),
            {},
            "No handler is registered for required action '" + action.id() + "'.",
        };
    }

    std::vector<std::string> owners;
    std::vector<ActionHandlerResult> results;
    owners.reserve(found->second.size());
    results.reserve(found->second.size());
    try {
        for (const ActionDispatcherState::HandlerEntry& handler : found->second) {
            owners.push_back(handler.owner);
            if (!handler.contract->payload_schema->accepts(action.payload)) {
                return ActionDispatchOutcome{
                    ActionDispatchStatus::failed,
                    action.id(),
                    owners,
                    "Action '" + action.id() + "' payload does not satisfy the handler contract.",
                };
            }
            results.push_back(handler.invoke(ActionContext{event, action}));
        }
    } catch (const std::exception& error) {
        return ActionDispatchOutcome{
            ActionDispatchStatus::failed,
            action.id(),
            owners,
            "Handler for '" + action.id() + "' failed: " + error.what(),
        };
    } catch (...) {
        return ActionDispatchOutcome{
            ActionDispatchStatus::failed,
            action.id(),
            owners,
            "Handler for '" + action.id() + "' failed with a non-standard exception.",
        };
    }

    if (action.contract->dispatch_policy == ActionDispatchPolicy::forwarded &&
        !std::ranges::contains(results, ActionHandlerResult::forwarded)) {
        return ActionDispatchOutcome{
            ActionDispatchStatus::failed,
            action.id(),
            owners,
            "Forwarded action '" + action.id() + "' did not cross its declared boundary.",
        };
    }
    const ActionDispatchStatus status = std::ranges::contains(results, ActionHandlerResult::handled)
                                            ? ActionDispatchStatus::handled
                                        : std::ranges::contains(results, ActionHandlerResult::forwarded)
                                            ? ActionDispatchStatus::forwarded
                                            : ActionDispatchStatus::ignored;
    return ActionDispatchOutcome{status, action.id(), std::move(owners), std::nullopt};
}

std::vector<std::shared_ptr<const ActionContract>> ActionDispatcher::missing_required_handlers(
    const std::vector<std::shared_ptr<const ActionContract>>& contracts
) const {
    std::vector<std::shared_ptr<const ActionContract>> missing;
    for (const auto& required_contract : contracts) {
        if (required_contract == nullptr ||
            (required_contract->dispatch_policy != ActionDispatchPolicy::required &&
             required_contract->dispatch_policy != ActionDispatchPolicy::forwarded)) {
            continue;
        }
        const auto handlers = state_->handlers.find(required_contract->id);
        if (handlers == state_->handlers.end() || handlers->second.empty()) {
            const bool already_added = std::ranges::any_of(missing, [&required_contract](const auto& current) {
                return current->id == required_contract->id;
            });
            if (!already_added) missing.push_back(required_contract);
        }
    }
    return missing;
}

} // namespace strata::runtime
