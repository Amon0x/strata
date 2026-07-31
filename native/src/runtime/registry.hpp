#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/schema.hpp"
#include "runtime/action.hpp"
#include "runtime/value_schema.hpp"

namespace strata::runtime {

[[nodiscard]] ValueSchemaPtr runtime_schema(const compiler::SemanticType& type);

/** Host boundary which turns the portable authored payload into the host's runtime value. */
using ActionPayloadDecoder = std::function<Value(Value)>;
using ActionPayloadDecoders = std::map<std::string, ActionPayloadDecoder, std::less<>>;

/** Runtime projection of the same neutral action declarations consumed by semantic validation. */
class RuntimeActionRegistry final {
public:
    static RuntimeActionRegistry from_schema(
        const compiler::SchemaRegistry& schema,
        ActionPayloadDecoders decoders = {}
    );

    [[nodiscard]] std::shared_ptr<const ActionContract> contract(std::string_view id) const;
    [[nodiscard]] std::vector<std::shared_ptr<const ActionContract>> contracts() const;
    [[nodiscard]] Value decode_payload(std::string_view id, Value authored_payload) const;
    [[nodiscard]] ActionDispatcher dispatcher() const;

private:
    std::map<std::string, std::shared_ptr<const ActionContract>, std::less<>> contracts_;
    ActionPayloadDecoders decoders_;
};

} // namespace strata::runtime
