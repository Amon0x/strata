#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.hpp"
#include "runtime/value_schema.hpp"

namespace strata::runtime {

enum class ActionDispatchPolicy { required, optional, broadcast, forwarded, framework };

struct ActionContract final {
    std::string id;
    ValueSchemaPtr payload_schema;
    ActionDispatchPolicy dispatch_policy = ActionDispatchPolicy::required;
    std::string payload_contract;
    std::string summary;

    [[nodiscard]] bool compatible_with(const ActionContract& other) const;
};

struct ActionOrigin final {
    std::string source_id;
    std::optional<std::uint32_t> line;
    std::optional<std::uint32_t> column;
    std::optional<std::uint32_t> end_line;
    std::optional<std::uint32_t> end_column;
    std::optional<std::string> component_path;
};

struct Action final {
    std::shared_ptr<const ActionContract> contract;
    Value payload;
    std::optional<ActionOrigin> origin;
    bool dynamic = false;

    Action(
        std::shared_ptr<const ActionContract> contract,
        Value payload = Value{},
        std::optional<ActionOrigin> origin = std::nullopt,
        bool dynamic = false
    );

    [[nodiscard]] const std::string& id() const noexcept;
};

/** Deliberately unsafe scripting/remote bridge action; unhandled dispatch remains diagnostic. */
[[nodiscard]] Action dynamic_action(
    std::string id,
    Value payload = Value{},
    std::optional<ActionOrigin> origin = std::nullopt
);

struct ActionEvent final {
    std::string kind;
    std::optional<std::string> source_key;
    Value value;
    /** Optional retained lifecycle suffix selected by the UI event producer. */
    std::optional<std::string> lifecycle_owner;
};

enum class ActionHandlerResult { handled, forwarded, ignored };
enum class ActionDispatchStatus { no_action, handled, forwarded, ignored, unhandled, failed };

struct ActionDispatchOutcome final {
    ActionDispatchStatus status;
    std::optional<std::string> action_id;
    std::vector<std::string> handler_owners;
    std::optional<std::string> message;
};

struct ActionContext final {
    const ActionEvent& event;
    const Action& action;
};

struct ActionDispatcherState;

class ActionRegistration final {
public:
    ActionRegistration() noexcept = default;
    ActionRegistration(const ActionRegistration&) = delete;
    ActionRegistration& operator=(const ActionRegistration&) = delete;
    ActionRegistration(ActionRegistration&& other) noexcept;
    ActionRegistration& operator=(ActionRegistration&& other) noexcept;
    ~ActionRegistration();

    void close() noexcept;

private:
    friend class ActionDispatcher;
    ActionRegistration(std::weak_ptr<ActionDispatcherState> state, std::uint64_t token) noexcept;

    std::weak_ptr<ActionDispatcherState> state_;
    std::uint64_t token_ = 0U;
};

/** Host-owned typed dispatcher. Handler lifetime is represented by move-only RAII registrations. */
class ActionDispatcher final {
public:
    using Handler = std::function<ActionHandlerResult(const ActionContext&)>;

    ActionDispatcher();
    explicit ActionDispatcher(std::vector<std::shared_ptr<const ActionContract>> contracts);

    void declare(std::shared_ptr<const ActionContract> contract);
    [[nodiscard]] std::shared_ptr<const ActionContract> contract(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> handler_owners(std::string_view id) const;
    [[nodiscard]] ActionRegistration register_handler(
        std::shared_ptr<const ActionContract> contract,
        std::string owner,
        Handler handler
    );
    [[nodiscard]] ActionDispatchOutcome dispatch(const ActionEvent& event, const Action& action) const;
    [[nodiscard]] std::vector<std::shared_ptr<const ActionContract>> missing_required_handlers(
        const std::vector<std::shared_ptr<const ActionContract>>& contracts
    ) const;

private:
    std::shared_ptr<ActionDispatcherState> state_;
};

} // namespace strata::runtime
