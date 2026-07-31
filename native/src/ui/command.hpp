#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/expression.hpp"
#include "ui/tree.hpp"

namespace strata::runtime {
class ApplicationContext;
class DurableState;
}

namespace strata::ui {

class WidgetRegistry;

struct CommandSnapshot final {
    std::string id;
    std::string label;
    std::string category;
    bool enabled = true;
    std::optional<std::string> owning_scope;
    std::optional<std::string> shortcut_key;
    bool shortcut_shift = false;
    bool shortcut_control = false;
    bool shortcut_alt = false;
    bool shortcut_super = false;
    bool accepts_repeat = false;
    int priority = 0;
    bool allowed_while_modal = false;
    bool allowed_while_text_editing = false;
    std::shared_ptr<const runtime::ActionValue> action;
    std::optional<bool> checked;
};

struct CommandActivationBinding final {
    std::string_view id;
    /** Null means the authoring reference is present but does not resolve in this Surface. */
    const CommandSnapshot* command = nullptr;
};

/** Resolved references plus whether the widget explicitly constrained the command set. */
struct CommandReferenceProjection final {
    bool constrained = false;
    std::vector<const CommandSnapshot*> commands;
};

/** Canonical platform shortcut text shared by command surfaces and command tooltips. */
[[nodiscard]] std::string format_command_shortcut(const CommandSnapshot& command);

/** Surface-local command registry derived from retained Command declarations. */
class CommandIndex final {
public:
    explicit CommandIndex(
        const WidgetRegistry& widgets,
        runtime::ApplicationContext* application = nullptr,
        runtime::DurableState* durability = nullptr,
        std::string persistence_scope = {}
    );
    void rebuild(const RetainedTree& tree);
    [[nodiscard]] const CommandSnapshot* find(std::string_view id) const noexcept;
    /** Resolves the lifecycle-declared scalar command binding for an actionable control. */
    [[nodiscard]] std::optional<CommandActivationBinding> activation_binding(
        const RetainedNode& node
    ) const noexcept;
    [[nodiscard]] std::vector<const CommandSnapshot*> referenced_by(
        const RetainedNode& node
    ) const;
    [[nodiscard]] CommandReferenceProjection reference_projection(
        const RetainedNode& node
    ) const;
    /** Stable retained declaration/traversal order, independent of keyed lookup ordering. */
    [[nodiscard]] const std::vector<const CommandSnapshot*>& ordered_entries() const noexcept;
    [[nodiscard]] const std::map<std::string, CommandSnapshot, std::less<>>& entries() const noexcept;
    /** Records only a successfully resolved execution in surface-local recency order. */
    void record_execution(std::string_view id);
    /** Unseen commands sort behind every executed command. */
    [[nodiscard]] std::int64_t recent_rank(std::string_view id) const noexcept;
    void clear() noexcept;

private:
    const WidgetRegistry& widgets_;
    runtime::ApplicationContext* application_ = nullptr;
    std::map<std::string, CommandSnapshot, std::less<>> entries_;
    std::vector<const CommandSnapshot*> declaration_order_;
    std::map<std::string, std::uint64_t, std::less<>> recent_;
    std::uint64_t execution_serial_ = 0U;
    runtime::DurableState* durability_ = nullptr;
    std::string persistence_scope_;
    const RetainedTree* observed_tree_ = nullptr;
    std::uint64_t observed_tree_generation_ = 0U;
    std::uint64_t observed_undo_generation_ = 0U;
};

} // namespace strata::ui
