#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "data/json.hpp"
#include "runtime/diagnostic.hpp"
#include "ui/command.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {

class InputRouter;

/** Retained semantic projection with publish-on-change generations. */
class SemanticsEngine final {
public:
    SemanticsEngine(const WidgetRegistry& widgets, const BehaviorRegistry& behaviors);
    [[nodiscard]] bool update(
        const RetainedTree& tree,
        const CommandIndex& commands,
        const InputRouter& input,
        std::string_view surface_id
    );
    [[nodiscard]] const data::JsonValue* find(std::uint64_t identity) const;
    [[nodiscard]] data::JsonValue snapshot(std::string_view surface_id) const;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] std::uint64_t publish_count() const noexcept;
    [[nodiscard]] std::vector<runtime::RuntimeDiagnostic> take_diagnostics();
    void clear_diagnostics() noexcept;
    void clear() noexcept;

private:
    [[nodiscard]] data::JsonValue build(const RetainedNode& node) const;
    void append_children(const RetainedNode& node, data::JsonValue::Array& children) const;
    void materialize() const;

    const WidgetRegistry& widgets_;
    const BehaviorRegistry& behaviors_;
    std::uint64_t generation_ = 0U;
    std::uint64_t publish_count_ = 0U;
    std::optional<std::uint64_t> root_identity_;
    std::optional<DirtyGenerationSnapshot> dirty_generations_;
    const RetainedTree* tree_ = nullptr;
    const CommandIndex* commands_ = nullptr;
    const InputRouter* input_ = nullptr;
    mutable std::uint64_t materialized_generation_ = 0U;
    mutable data::JsonValue root_;
    mutable std::map<std::uint64_t, data::JsonValue> nodes_;
    mutable std::vector<runtime::RuntimeDiagnostic> diagnostics_;
    mutable std::set<std::string, std::less<>> reported_diagnostics_;
};

} // namespace strata::ui
