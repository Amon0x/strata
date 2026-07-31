#include "runtime/registry.hpp"

#include <stdexcept>
#include <utility>

namespace strata::runtime {
namespace {

[[nodiscard]] ActionDispatchPolicy dispatch_policy(const std::string_view value) {
    if (value == "required") return ActionDispatchPolicy::required;
    if (value == "optional") return ActionDispatchPolicy::optional;
    if (value == "broadcast") return ActionDispatchPolicy::broadcast;
    if (value == "forwarded") return ActionDispatchPolicy::forwarded;
    if (value == "framework") return ActionDispatchPolicy::framework;
    throw std::runtime_error("unknown action dispatch policy '" + std::string(value) + "'");
}

} // namespace

ValueSchemaPtr runtime_schema(const compiler::SemanticType& type) {
    using compiler::SemanticTypeKind;
    switch (type.kind) {
    case SemanticTypeKind::null_value:
        return ValueSchema::scalar(ValueSchemaKind::null_value);
    case SemanticTypeKind::string:
    case SemanticTypeKind::string_literal:
    case SemanticTypeKind::enumeration:
        return ValueSchema::scalar(ValueSchemaKind::string);
    case SemanticTypeKind::number:
        return ValueSchema::scalar(ValueSchemaKind::number);
    case SemanticTypeKind::duration:
        return ValueSchema::scalar(ValueSchemaKind::duration);
    case SemanticTypeKind::boolean:
        return ValueSchema::scalar(ValueSchemaKind::boolean);
    case SemanticTypeKind::color:
        return ValueSchema::scalar(ValueSchemaKind::color);
    case SemanticTypeKind::texture:
        return ValueSchema::scalar(ValueSchemaKind::texture);
    case SemanticTypeKind::key:
        return ValueSchema::scalar(ValueSchemaKind::key);
    case SemanticTypeKind::list:
    case SemanticTypeKind::collection:
        return ValueSchema::list(
            type.element != nullptr ? runtime_schema(*type.element) : ValueSchema::any(),
            type.element_nullable,
            type.maximum_items
        );
    case SemanticTypeKind::map:
    case SemanticTypeKind::host_object:
    case SemanticTypeKind::async_value: {
        std::vector<ValueSchemaField> fields;
        fields.reserve(type.fields.size());
        for (const compiler::ObjectField& field : type.fields) {
            fields.push_back(ValueSchemaField{
                field.name,
                field.type != nullptr ? runtime_schema(*field.type) : ValueSchema::any(),
                field.required,
                field.nullable,
            });
        }
        return ValueSchema::object(
            std::move(fields),
            type.allow_unknown_fields,
            type.value != nullptr ? runtime_schema(*type.value) : ValueSchema::any()
        );
    }
    case SemanticTypeKind::union_value: {
        std::vector<ValueSchemaPtr> options;
        options.reserve(type.options.size());
        for (const compiler::SemanticTypePtr& option : type.options) {
            options.push_back(option != nullptr ? runtime_schema(*option) : ValueSchema::any());
        }
        return options.empty() ? ValueSchema::any() : ValueSchema::union_of(std::move(options));
    }
    case SemanticTypeKind::unknown:
    case SemanticTypeKind::any:
    case SemanticTypeKind::unsafe_component_parameter:
    case SemanticTypeKind::style:
    case SemanticTypeKind::layout:
    case SemanticTypeKind::animation:
    case SemanticTypeKind::effect:
    case SemanticTypeKind::material:
    case SemanticTypeKind::action:
    case SemanticTypeKind::component:
    case SemanticTypeKind::lambda:
    case SemanticTypeKind::component_template:
        return ValueSchema::any();
    }
    throw std::logic_error("invalid semantic type kind");
}

RuntimeActionRegistry RuntimeActionRegistry::from_schema(
    const compiler::SchemaRegistry& schema,
    ActionPayloadDecoders decoders
) {
    RuntimeActionRegistry registry;
    for (const std::string& id : schema.action_names()) {
        const compiler::ActionSchema* action = schema.action(id);
        if (action == nullptr) throw std::logic_error("action inventory returned a missing schema");
        ValueSchemaPtr payload_schema;
        if (action->payload_contract == "no payload") {
            payload_schema = ValueSchema::scalar(ValueSchemaKind::null_value);
        } else {
            std::vector<ValueSchemaField> fields;
            fields.reserve(action->parameters.size());
            for (const compiler::SchemaParameter& parameter : action->parameters) {
                fields.push_back(ValueSchemaField{
                    parameter.name,
                    parameter.type != nullptr ? runtime_schema(*parameter.type) : ValueSchema::any(),
                    parameter.required,
                    parameter.nullable,
                });
            }
            payload_schema = ValueSchema::object(std::move(fields));
        }
        auto contract = std::make_shared<const ActionContract>(ActionContract{
            action->id,
            std::move(payload_schema),
            dispatch_policy(action->dispatch_policy),
            action->payload_contract,
            action->summary,
        });
        if (!registry.contracts_.emplace(contract->id, contract).second) {
            throw std::runtime_error("runtime action ids must be unique");
        }
    }
    for (auto& [id, decoder] : decoders) {
        if (registry.contracts_.find(id) == registry.contracts_.end()) {
            throw std::invalid_argument("action payload decoder references unknown action '" + id + "'");
        }
        if (!decoder) {
            throw std::invalid_argument("action payload decoder for '" + id + "' must be callable");
        }
    }
    registry.decoders_ = std::move(decoders);
    return registry;
}

std::shared_ptr<const ActionContract> RuntimeActionRegistry::contract(const std::string_view id) const {
    const auto found = contracts_.find(id);
    return found != contracts_.end() ? found->second : nullptr;
}

std::vector<std::shared_ptr<const ActionContract>> RuntimeActionRegistry::contracts() const {
    std::vector<std::shared_ptr<const ActionContract>> result;
    result.reserve(contracts_.size());
    for (const auto& [id, contract] : contracts_) {
        static_cast<void>(id);
        result.push_back(contract);
    }
    return result;
}

Value RuntimeActionRegistry::decode_payload(
    const std::string_view id,
    Value authored_payload
) const {
    const auto found = decoders_.find(id);
    return found != decoders_.end() ? found->second(std::move(authored_payload))
                                    : std::move(authored_payload);
}

ActionDispatcher RuntimeActionRegistry::dispatcher() const {
    return ActionDispatcher(contracts());
}

} // namespace strata::runtime
