#include "runtime/unit.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "compiler/portable_ir.hpp"

namespace strata::runtime {
namespace {

using JsonValue = data::JsonView;
using JsonArray = data::JsonArrayView;

[[nodiscard]] JsonValue required(const JsonValue value, const std::string_view field) {
    const JsonValue result = value.find(field);
    if (!result) throw std::runtime_error("portable IR is missing field '" + std::string(field) + "'");
    return result;
}

[[nodiscard]] std::string string_field(const JsonValue value, const std::string_view field) {
    const std::optional<std::string_view> result = required(value, field).string();
    if (!result.has_value() || result->empty()) {
        throw std::runtime_error("portable IR field '" + std::string(field) + "' must be a non-empty string");
    }
    return std::string(*result);
}

[[nodiscard]] std::optional<std::string> optional_string_field(
    const JsonValue value,
    const std::string_view field
) {
    const JsonValue child = required(value, field);
    if (child.is_null()) return std::nullopt;
    const std::optional<std::string_view> result = child.string();
    if (!result.has_value() || result->empty()) {
        throw std::runtime_error(
            "portable IR field '" + std::string(field) + "' must be a non-empty string or null"
        );
    }
    return std::string(*result);
}

[[nodiscard]] JsonArray array_field(const JsonValue value, const std::string_view field) {
    const std::optional<JsonArray> result = required(value, field).array();
    if (!result.has_value()) throw std::runtime_error("portable IR field '" + std::string(field) + "' must be an array");
    return *result;
}

[[nodiscard]] std::uint64_t unsigned_field(
    const JsonValue value,
    const std::string_view field
) {
    const std::optional<std::int64_t> result = required(value, field).integer();
    if (!result.has_value() || *result < 0) {
        throw std::runtime_error(
            "portable IR field '" + std::string(field) + "' must be a non-negative integer"
        );
    }
    return static_cast<std::uint64_t>(*result);
}

[[nodiscard]] DiagnosticPosition diagnostic_position(const JsonValue& value) {
    const std::uint64_t line = unsigned_field(value, "line");
    const std::uint64_t column = unsigned_field(value, "column");
    if (line == 0U || column == 0U || line > UINT32_MAX || column > UINT32_MAX) {
        throw std::runtime_error("portable IR diagnostic position is outside the supported range");
    }
    return DiagnosticPosition{
        static_cast<std::uint32_t>(line),
        static_cast<std::uint32_t>(column),
        unsigned_field(value, "offset"),
    };
}

[[nodiscard]] DiagnosticRange diagnostic_range(const JsonValue& value) {
    return DiagnosticRange{
        string_field(value, "sourceId"),
        diagnostic_position(required(value, "start")),
        diagnostic_position(required(value, "end")),
    };
}

[[nodiscard]] std::string runtime_type_id(const JsonValue statement) {
    const JsonValue declared = statement.find("declaredType");
    if (const std::optional<std::string_view> encoded = declared.string(); encoded.has_value()) {
        const std::string_view type = *encoded;
        if (type == "boolean") return "dsl.boolean";
        if (type == "number") return "dsl.number";
        if (type == "duration") return "dsl.duration";
        if (type == "string") return "dsl.string";
        if (type == "color") return "dsl.color";
        if (type.starts_with("list")) return "dsl.list";
        if (type.starts_with("map") || type == "record") return "dsl.map";
        if (type == "null") return "dsl.null";
        return "dsl." + std::string(type);
    }
    const JsonValue literal = statement.find("initializer").find("value");
    const std::optional<std::string_view> kind = literal.find("kind").string();
    if (!kind.has_value()) return "dsl.unknown";
    if (*kind == "themeToken") return "dsl.theme-token";
    return "dsl." + std::string(*kind);
}

[[nodiscard]] ValueSchemaPtr portable_value_schema(const JsonValue value) {
    const std::string kind = string_field(value, "kind");
    if (kind == "null") return ValueSchema::scalar(ValueSchemaKind::null_value);
    if (kind == "boolean") return ValueSchema::scalar(ValueSchemaKind::boolean);
    if (kind == "number") return ValueSchema::scalar(ValueSchemaKind::number);
    if (kind == "duration") return ValueSchema::scalar(ValueSchemaKind::duration);
    if (kind == "string" || kind == "stringLiteral" || kind == "enum") {
        return ValueSchema::scalar(ValueSchemaKind::string);
    }
    if (kind == "color") return ValueSchema::scalar(ValueSchemaKind::color);
    if (kind == "image") return ValueSchema::scalar(ValueSchemaKind::image);
    if (kind == "key") return ValueSchema::scalar(ValueSchemaKind::key);
    if (kind == "list" || kind == "collection") {
        const JsonValue element = value.find(kind == "list" ? "element" : "item");
        const JsonValue nullable = value.find("elementNullable");
        const JsonValue maximum = value.find("maximumItems");
        std::optional<std::size_t> maximum_items;
        if (const std::optional<std::int64_t> count = maximum.integer();
            count.has_value() && *count >= 0) {
            maximum_items = static_cast<std::size_t>(*count);
        }
        return ValueSchema::list(
            element ? portable_value_schema(element) : ValueSchema::any(),
            nullable.boolean().value_or(false),
            maximum_items
        );
    }
    if (kind == "map" || kind == "hostObject" || kind == "async") {
        std::vector<ValueSchemaField> fields;
        const std::optional<JsonArray> encoded_fields = value.find("fields").array();
        if (encoded_fields.has_value()) {
            fields.reserve(encoded_fields->size());
            for (const JsonValue field : *encoded_fields) {
                const JsonValue required_value = field.find("required");
                const JsonValue nullable_value = field.find("nullable");
                fields.push_back(ValueSchemaField{
                    string_field(field, "name"),
                    portable_value_schema(required(field, "type")),
                    required_value.boolean().value_or(true),
                    nullable_value.boolean().value_or(false),
                });
            }
        }
        const JsonValue allow_unknown = value.find("allowUnknownFields");
        const JsonValue unknown = value.find("value");
        return ValueSchema::object(
            std::move(fields),
            allow_unknown.boolean().value_or(false),
            unknown ? portable_value_schema(unknown) : ValueSchema::any()
        );
    }
    if (kind == "union") {
        std::vector<ValueSchemaPtr> options;
        const std::optional<JsonArray> encoded = value.find("options").array();
        if (encoded.has_value()) {
            options.reserve(encoded->size());
            for (const JsonValue option : *encoded) {
                options.push_back(portable_value_schema(option));
            }
        }
        return options.empty() ? ValueSchema::any() : ValueSchema::union_of(std::move(options));
    }
    return ValueSchema::any();
}

[[nodiscard]] std::optional<std::string> persistence_key(const JsonValue initializer) {
    if (initializer.find("kind").string() != std::optional<std::string_view>("helper") ||
        initializer.find("name").string() != std::optional<std::string_view>("persisted")) {
        return std::nullopt;
    }
    const std::optional<JsonArray> arguments = initializer.find("arguments").array();
    if (!arguments.has_value() || arguments->empty()) return std::nullopt;
    for (std::size_t index = 0U; index < arguments->size(); ++index) {
        const JsonValue argument = (*arguments)[index];
        if (index != 0U &&
            argument.find("name").string() != std::optional<std::string_view>("key")) {
            continue;
        }
        const JsonValue literal_value = argument.find("value").find("value").find("value");
        if (const std::optional<std::string_view> value = literal_value.string(); value.has_value()) {
            return std::string(*value);
        }
    }
    return std::nullopt;
}

void collect_state(
    const JsonValue block,
    const std::string& scope,
    std::vector<UnitStateDeclaration>& declarations
) {
    for (const JsonValue statement : array_field(block, "statements")) {
        const std::string kind = string_field(statement, "kind");
        if (kind == "state") {
            declarations.push_back(UnitStateDeclaration{
                scope,
                string_field(statement, "name"),
                runtime_type_id(statement),
                optional_string_field(statement, "declaredType"),
                required(statement, "declaredSchema").is_null()
                    ? ValueSchema::any()
                    : portable_value_schema(required(statement, "declaredSchema")),
                string_field(statement, "path"),
                required(statement, "initializer"),
                persistence_key(required(statement, "initializer")),
            });
        } else if (kind == "node") {
            const JsonValue children = required(statement, "call").find("children");
            if (children && !children.is_null()) collect_state(children, scope, declarations);
        } else if (kind == "if") {
            collect_state(required(statement, "then"), scope, declarations);
            const JsonValue otherwise = required(statement, "else");
            if (!otherwise.is_null()) collect_state(otherwise, scope, declarations);
        } else if (kind == "when") {
            for (const JsonValue branch : array_field(statement, "branches")) {
                collect_state(required(branch, "block"), scope, declarations);
            }
        } else if (kind == "for") {
            collect_state(required(statement, "block"), scope, declarations);
        }
    }
}

} // namespace

std::shared_ptr<const RuntimeUnit> RuntimeUnit::create(
    PortableIrStorage portable_ir,
    compiler::CompiledSourceMap source_map
) {
    std::visit([](const auto& stored) {
        compiler::validate_portable_ir(stored);
    }, portable_ir);
    return std::shared_ptr<const RuntimeUnit>(new RuntimeUnit(
        std::move(portable_ir),
        std::move(source_map)
    ));
}

RuntimeUnit::RuntimeUnit(
    PortableIrStorage portable_ir,
    compiler::CompiledSourceMap source_map
) : portable_ir_(std::move(portable_ir)), source_map_(std::move(source_map)) {
    build_indexes();
    if (source_map_.source_id.empty()) source_map_.source_id = source_id_;
}

data::JsonView RuntimeUnit::portable_ir() const noexcept {
    return std::visit([](const auto& stored) {
        using Stored = std::remove_cvref_t<decltype(stored)>;
        if constexpr (std::is_same_v<Stored, data::JsonValue>) {
            return data::JsonView(stored);
        } else {
            return stored.root();
        }
    }, portable_ir_);
}
const std::string& RuntimeUnit::source_id() const noexcept { return source_id_; }
const std::vector<std::string>& RuntimeUnit::screens() const noexcept { return screens_; }
const std::vector<std::string>& RuntimeUnit::overlays() const noexcept { return overlays_; }
const std::vector<std::string>& RuntimeUnit::referenced_actions() const noexcept { return referenced_actions_; }
const std::vector<UnitActionReference>& RuntimeUnit::action_references() const noexcept {
    return action_references_;
}
const std::vector<UnitStateDeclaration>& RuntimeUnit::state_declarations() const noexcept {
    return state_declarations_;
}

const compiler::CompiledSourceMap& RuntimeUnit::source_map() const noexcept { return source_map_; }

const compiler::CompiledSourceMapEntry* RuntimeUnit::source_map_entry(
    const std::string_view path
) const {
    std::call_once(source_map_path_index_once_, [this] {
        for (std::size_t index = 0U; index < source_map_.entries.size(); ++index) {
            source_map_path_indexes_.insert_or_assign(source_map_.entries[index].path, index);
        }
    });
    const auto found = source_map_path_indexes_.find(path);
    return found != source_map_path_indexes_.end()
        ? &source_map_.entries[found->second]
        : nullptr;
}

std::vector<const compiler::CompiledSourceMapEntry*> RuntimeUnit::source_map_entries_at(
    const std::string_view source_id,
    const std::uint32_t line,
    const std::uint32_t column
) const {
    std::vector<const compiler::CompiledSourceMapEntry*> result;
    for (const compiler::CompiledSourceMapEntry& entry : source_map_.entries) {
        if (entry.span.source_id != source_id) continue;
        const compiler::SourcePosition& start = entry.span.start;
        const compiler::SourcePosition& end = entry.span.end;
        const bool after_start = line > start.line ||
                                 (line == start.line && column >= start.column);
        const bool at_zero_width = entry.span.length == 0U &&
                                   line == start.line && column == start.column;
        const bool before_end = line < end.line ||
                                (line == end.line && column < end.column);
        if (at_zero_width || (after_start && before_end)) result.push_back(&entry);
    }
    return result;
}

const UnitStateDeclaration* RuntimeUnit::state_declaration(
    const std::string_view scope,
    const std::string_view name
) const noexcept {
    const auto found = std::ranges::lower_bound(
        state_declarations_,
        std::pair{scope, name},
        {},
        [](const UnitStateDeclaration& value) {
            return std::pair<std::string_view, std::string_view>{value.scope, value.name};
        }
    );
    return found != state_declarations_.end() && found->scope == scope && found->name == name
               ? &*found
               : nullptr;
}

data::JsonView RuntimeUnit::screen(const std::string_view name) const noexcept {
    const auto found = screen_indexes_.find(name);
    return found != screen_indexes_.end()
        ? array_field(portable_ir(), "screens")[found->second]
        : data::JsonView{};
}

data::JsonView RuntimeUnit::overlay(const std::string_view name) const noexcept {
    const auto found = overlay_indexes_.find(name);
    return found != overlay_indexes_.end()
        ? array_field(portable_ir(), "overlays")[found->second]
        : data::JsonView{};
}

data::JsonView RuntimeUnit::component(const std::string_view name) const noexcept {
    const auto found = component_indexes_.find(name);
    return found != component_indexes_.end()
        ? array_field(portable_ir(), "components")[found->second]
        : data::JsonView{};
}

data::JsonView RuntimeUnit::style(const std::string_view name) const noexcept {
    const auto found = style_indexes_.find(name);
    return found != style_indexes_.end()
        ? array_field(portable_ir(), "styles")[found->second]
        : data::JsonView{};
}

data::JsonView RuntimeUnit::animation(const std::string_view name) const noexcept {
    const auto found = animation_indexes_.find(name);
    return found != animation_indexes_.end()
        ? array_field(portable_ir(), "animations")[found->second]
        : data::JsonView{};
}

void RuntimeUnit::build_indexes() {
    const JsonValue root = portable_ir();
    source_id_ = string_field(root, "sourceId");
    const auto index_declarations = [this](
                                        const JsonArray declarations,
                                        const std::string_view role,
                                        std::vector<std::string>& names,
                                        std::map<std::string, std::size_t, std::less<>>& indexes
                                    ) {
        for (std::size_t index = 0U; index < declarations.size(); ++index) {
            const JsonValue declaration = declarations[index];
            const std::string name = string_field(declaration, "name");
            if (!indexes.emplace(name, index).second) {
                throw std::runtime_error("portable IR contains duplicate " + std::string(role) + " '" + name + "'");
            }
            names.push_back(name);
            collect_state(required(declaration, "body"), std::string(role) + " " + name, state_declarations_);
        }
    };
    index_declarations(array_field(root, "screens"), "screen", screens_, screen_indexes_);
    index_declarations(array_field(root, "overlays"), "overlay", overlays_, overlay_indexes_);
    std::vector<std::string> component_names;
    index_declarations(
        array_field(root, "components"),
        "component",
        component_names,
        component_indexes_
    );

    const JsonArray styles = array_field(root, "styles");
    for (std::size_t index = 0U; index < styles.size(); ++index) {
        const std::string name = string_field(styles[index], "name");
        if (!style_indexes_.emplace(name, index).second) {
            throw std::runtime_error("portable IR contains duplicate style '" + name + "'");
        }
    }

    const JsonArray animations = array_field(root, "animations");
    for (std::size_t index = 0U; index < animations.size(); ++index) {
        const std::string name = string_field(animations[index], "name");
        if (!animation_indexes_.emplace(name, index).second) {
            throw std::runtime_error("portable IR contains duplicate animation '" + name + "'");
        }
    }

    std::set<std::string, std::less<>> actions;
    for (const JsonValue reference : array_field(root, "actionReferences")) {
        const std::string id = string_field(reference, "id");
        actions.insert(id);
        action_references_.push_back(UnitActionReference{
            id,
            string_field(reference, "componentPath"),
            diagnostic_range(required(reference, "range")),
        });
    }
    referenced_actions_.assign(actions.begin(), actions.end());
    std::ranges::sort(state_declarations_, {}, [](const UnitStateDeclaration& value) {
        return std::pair{value.scope, value.name};
    });
    const auto duplicate = std::ranges::adjacent_find(
        state_declarations_,
        [](const UnitStateDeclaration& left, const UnitStateDeclaration& right) {
            return left.scope == right.scope && left.name == right.name;
        }
    );
    if (duplicate != state_declarations_.end()) {
        throw std::runtime_error(
            "portable IR state declaration '" + duplicate->scope + "/" + duplicate->name + "' is ambiguous"
        );
    }
}

} // namespace strata::runtime
