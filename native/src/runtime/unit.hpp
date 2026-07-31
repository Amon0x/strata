#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "compiler/source_map.hpp"
#include "data/json.hpp"
#include "data/json_view.hpp"
#include "runtime/diagnostic.hpp"
#include "runtime/value.hpp"
#include "runtime/value_schema.hpp"

namespace strata::runtime {

struct UnitStateDeclaration final {
    std::string scope;
    std::string name;
    std::string type_id;
    std::optional<std::string> declared_type;
    ValueSchemaPtr schema;
    std::string declaration_path;
    data::JsonView initializer;
    std::optional<std::string> persistence_key;

    [[nodiscard]] friend bool operator==(const UnitStateDeclaration&, const UnitStateDeclaration&) = default;
};

struct UnitActionReference final {
    std::string id;
    std::string component_path;
    DiagnosticRange range;
};

/** Immutable, validated, parser-independent runtime program. */
class RuntimeUnit final {
public:
    using PortableIrStorage = std::variant<data::JsonValue, data::FrozenJsonDocument>;

    static std::shared_ptr<const RuntimeUnit> create(
        PortableIrStorage portable_ir,
        compiler::CompiledSourceMap source_map = {}
    );

    [[nodiscard]] data::JsonView portable_ir() const noexcept;
    [[nodiscard]] const std::string& source_id() const noexcept;
    [[nodiscard]] const std::vector<std::string>& screens() const noexcept;
    [[nodiscard]] const std::vector<std::string>& overlays() const noexcept;
    [[nodiscard]] const std::vector<std::string>& referenced_actions() const noexcept;
    [[nodiscard]] const std::vector<UnitActionReference>& action_references() const noexcept;
    [[nodiscard]] const std::vector<UnitStateDeclaration>& state_declarations() const noexcept;
    [[nodiscard]] const compiler::CompiledSourceMap& source_map() const noexcept;
    [[nodiscard]] const compiler::CompiledSourceMapEntry* source_map_entry(
        std::string_view path
    ) const;
    [[nodiscard]] std::vector<const compiler::CompiledSourceMapEntry*> source_map_entries_at(
        std::string_view source_id,
        std::uint32_t line,
        std::uint32_t column
    ) const;
    [[nodiscard]] const UnitStateDeclaration* state_declaration(
        std::string_view scope,
        std::string_view name
    ) const noexcept;
    [[nodiscard]] data::JsonView screen(std::string_view name) const noexcept;
    [[nodiscard]] data::JsonView overlay(std::string_view name) const noexcept;
    [[nodiscard]] data::JsonView component(std::string_view name) const noexcept;
    [[nodiscard]] data::JsonView style(std::string_view name) const noexcept;
    [[nodiscard]] data::JsonView animation(std::string_view name) const noexcept;

private:
    RuntimeUnit(PortableIrStorage portable_ir, compiler::CompiledSourceMap source_map);
    void build_indexes();

    PortableIrStorage portable_ir_;
    compiler::CompiledSourceMap source_map_;
    mutable std::once_flag source_map_path_index_once_;
    mutable std::map<std::string, std::size_t, std::less<>> source_map_path_indexes_;
    std::string source_id_;
    std::vector<std::string> screens_;
    std::vector<std::string> overlays_;
    std::vector<std::string> referenced_actions_;
    std::vector<UnitActionReference> action_references_;
    std::vector<UnitStateDeclaration> state_declarations_;
    std::map<std::string, std::size_t, std::less<>> screen_indexes_;
    std::map<std::string, std::size_t, std::less<>> overlay_indexes_;
    std::map<std::string, std::size_t, std::less<>> component_indexes_;
    std::map<std::string, std::size_t, std::less<>> style_indexes_;
    std::map<std::string, std::size_t, std::less<>> animation_indexes_;
};

} // namespace strata::runtime
