#include "data/json_view.hpp"

#include <stdexcept>
#include <string>

namespace strata::data {

FrozenJsonDocument::FrozenJsonDocument(
    std::shared_ptr<const std::vector<std::uint8_t>> storage,
    std::vector<std::string_view> strings,
    std::vector<FrozenJsonNode> nodes,
    std::vector<std::uint32_t> array_items,
    std::vector<FrozenJsonObjectEntry> object_items,
    const std::uint32_t root
) : storage_(std::move(storage)),
    strings_(std::move(strings)),
    nodes_(std::move(nodes)),
    array_items_(std::move(array_items)),
    object_items_(std::move(object_items)),
    root_(root) {
    if (storage_ == nullptr || nodes_.empty() || root_ >= nodes_.size()) {
        throw std::invalid_argument("frozen JSON document requires valid owned storage and root");
    }
}

JsonValue materialize_json(const JsonView value) {
    switch (value.kind()) {
    case JsonViewKind::null_value: return JsonValue(JsonValue::Null{});
    case JsonViewKind::boolean: return JsonValue(*value.boolean());
    case JsonViewKind::integer: return JsonValue(*value.integer());
    case JsonViewKind::number: return JsonValue(*value.number());
    case JsonViewKind::string: return JsonValue(std::string(*value.string()));
    case JsonViewKind::array: {
        JsonValue::Array result;
        const JsonArrayView source = *value.array();
        result.reserve(source.size());
        for (const JsonView child : source) result.push_back(materialize_json(child));
        return JsonValue(std::move(result));
    }
    case JsonViewKind::object: {
        JsonValue::Object result;
        const JsonObjectView source = *value.object();
        result.reserve(source.size());
        for (const auto [name, child] : source) {
            result.emplace_back(std::string(name), materialize_json(child));
        }
        return JsonValue(std::move(result));
    }
    case JsonViewKind::invalid: break;
    }
    throw std::invalid_argument("cannot materialize an invalid JSON view");
}

} // namespace strata::data
