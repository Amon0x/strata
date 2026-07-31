#include "ui/command.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "runtime/application.hpp"
#include "runtime/durability.hpp"
#include "ui/widget/registry.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] const runtime::Value* property(
    const RetainedNode& node,
    const std::string_view name
) {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.data_value() : nullptr;
}

[[nodiscard]] std::shared_ptr<const runtime::ActionValue> action_property(
    const RetainedNode& node,
    const std::string_view name
) {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() && found->second.action() != nullptr
               ? *found->second.action()
               : nullptr;
}

[[nodiscard]] const std::string* text(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    if (value->key() != nullptr) return &value->key()->value;
    return nullptr;
}

[[nodiscard]] bool boolean(const runtime::Value* value, const bool fallback) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] std::optional<bool> optional_boolean(const runtime::Value* value) noexcept {
    return value != nullptr && value->boolean() != nullptr
        ? std::optional<bool>(*value->boolean())
        : std::nullopt;
}

[[nodiscard]] int integer(const runtime::Value* value) noexcept {
    if (value == nullptr || value->number() == nullptr || !std::isfinite(*value->number())) return 0;
    return static_cast<int>(std::clamp(
        *value->number(),
        static_cast<double>(std::numeric_limits<int>::min()),
        static_cast<double>(std::numeric_limits<int>::max())
    ));
}

struct ParsedShortcut final {
    std::optional<std::string> key;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super_key = false;
};

[[nodiscard]] ParsedShortcut shortcut(const runtime::Value* value) {
    const std::string* authored = text(value);
    if (authored == nullptr || authored->empty()) return {};
    ParsedShortcut result;
    std::size_t start = 0U;
    while (start <= authored->size()) {
        const std::size_t separator = authored->find('+', start);
        std::string token = authored->substr(
            start,
            separator == std::string::npos ? std::string::npos : separator - start
        );
        token.erase(token.begin(), std::ranges::find_if(token, [](const unsigned char character) {
            return !std::isspace(character);
        }));
        token.erase(std::ranges::find_if(token.rbegin(), token.rend(), [](const unsigned char character) {
            return !std::isspace(character);
        }).base(), token.end());
        std::ranges::transform(token, token.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (token == "ctrl" || token == "control") result.control = true;
        else if (token == "shift") result.shift = true;
        else if (token == "alt" || token == "option") result.alt = true;
        else if (token == "cmd" || token == "command" || token == "meta" || token == "super") {
            result.super_key = true;
        } else if (!token.empty() && !result.key.has_value()) {
            if (token == "esc") token = "escape";
            else if (token == "return") token = "enter";
            else if (token == "pgup") token = "page_up";
            else if (token == "pgdn") token = "page_down";
            result.key = std::move(token);
        } else {
            return {};
        }
        if (separator == std::string::npos) break;
        start = separator + 1U;
    }
    return result.key.has_value() ? result : ParsedShortcut{};
}

[[nodiscard]] std::string shortcut_key_name(const std::string_view key) {
#if defined(__APPLE__)
    if (key == "escape") return "⎋";
    if (key == "enter") return "↩";
    if (key == "backspace") return "⌫";
    if (key == "delete") return "⌦";
#else
    if (key == "escape") return "Esc";
    if (key == "enter") return "Enter";
    if (key == "backspace") return "Backspace";
    if (key == "delete") return "Delete";
#endif
    if (key == "up") return "↑";
    if (key == "down") return "↓";
    if (key == "left") return "←";
    if (key == "right") return "→";
    if (key == "page_up") return "PageUp";
    if (key == "page_down") return "PageDown";
    std::string result(key);
    std::ranges::transform(result, result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

} // namespace

std::string format_command_shortcut(const CommandSnapshot& command) {
    if (!command.shortcut_key.has_value()) return {};
#if defined(__APPLE__)
    std::string result;
    if (command.shortcut_control) result += "⌃";
    if (command.shortcut_alt) result += "⌥";
    if (command.shortcut_shift) result += "⇧";
    if (command.shortcut_super) result += "⌘";
    result += shortcut_key_name(*command.shortcut_key);
    return result;
#else
    std::vector<std::string> parts;
    if (command.shortcut_control) parts.emplace_back("Ctrl");
    if (command.shortcut_alt) parts.emplace_back("Alt");
    if (command.shortcut_shift) parts.emplace_back("Shift");
    if (command.shortcut_super) parts.emplace_back("Meta");
    parts.push_back(shortcut_key_name(*command.shortcut_key));
    std::string result;
    for (const std::string& part : parts) {
        if (!result.empty()) result += '+';
        result += part;
    }
    return result;
#endif
}

CommandIndex::CommandIndex(
    const WidgetRegistry& widgets,
    runtime::ApplicationContext* const application,
    runtime::DurableState* const durability,
    std::string persistence_scope
)
    : widgets_(widgets),
      application_(application),
      durability_(durability),
      persistence_scope_(std::move(persistence_scope)) {
    if (durability_ == nullptr || persistence_scope_.empty()) return;
    std::vector<std::string> restored = durability_->command_recency(persistence_scope_);
    for (auto entry = restored.rbegin(); entry != restored.rend(); ++entry) {
        recent_.insert_or_assign(*entry, ++execution_serial_);
    }
}

void CommandIndex::rebuild(const RetainedTree& tree) {
    const std::uint64_t undo_generation = application_ != nullptr
        ? application_->undo().generation() : 0U;
    if (observed_tree_ == &tree && observed_tree_generation_ == tree.generation() &&
        observed_undo_generation_ == undo_generation) {
        return;
    }
    entries_.clear();
    declaration_order_.clear();
    observed_tree_ = &tree;
    observed_tree_generation_ = tree.generation();
    observed_undo_generation_ = undo_generation;
    if (tree.root() == nullptr) return;
    const auto visit = [this](auto&& self, const RetainedNode& node) -> void {
        const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
        if (lifecycle != nullptr && lifecycle->command.declaration) {
            const std::string* id = text(property(node, "id"));
            if (id != nullptr && !id->empty()) {
                const std::string* label = text(property(node, "label"));
                const runtime::Value* enabled_value = property(node, "enabled");
                const bool enabled = enabled_value == nullptr || enabled_value->boolean() == nullptr ||
                                     *enabled_value->boolean();
                const ParsedShortcut chord = shortcut(property(node, "shortcut"));
                CommandSnapshot command{
                    *id,
                    label != nullptr && !label->empty() ? *label : *id,
                    text(property(node, "category")) != nullptr &&
                            !text(property(node, "category"))->empty()
                        ? *text(property(node, "category"))
                        : std::string("Commands"),
                    enabled,
                    text(property(node, "scope")) != nullptr
                        ? std::optional<std::string>(*text(property(node, "scope")))
                        : std::nullopt,
                    chord.key,
                    chord.shift,
                    chord.control,
                    chord.alt,
                    chord.super_key,
                    boolean(property(node, "acceptsRepeat"), false),
                    integer(property(node, "priority")),
                    boolean(property(node, "allowedWhileModal"), false),
                    boolean(property(node, "allowedWhileTextEditing"), false),
                    action_property(node, "action"),
                    optional_boolean(property(node, "checked")),
                };
                const auto [inserted, unique] = entries_.emplace(command.id, std::move(command));
                if (!unique) {
                    throw std::invalid_argument(
                        "surface contains duplicate command id '" + *id + "'"
                    );
                }
                declaration_order_.push_back(&inserted->second);
            }
        }
        for (const auto& child : node.children()) self(self, *child);
    };
    visit(visit, *tree.root());

    if (application_ != nullptr) {
        const runtime::UndoStackStatus status = application_->undo().status(persistence_scope_);
        const auto add_undo_command = [this](
                                          const std::string_view id,
                                          std::string label,
                                          const bool enabled,
                                          const std::string_view shortcut_key
                                      ) {
            if (entries_.contains(id)) return;
            const std::shared_ptr<const runtime::ActionContract> contract =
                application_->bundle()->action_registry().contract(id);
            if (contract == nullptr) return;
            auto action = std::make_shared<const runtime::ActionValue>(runtime::ActionValue{
                std::make_shared<const runtime::Action>(contract),
                std::nullopt,
                {},
                std::nullopt,
            });
            CommandSnapshot command{
                std::string(id),
                std::move(label),
                "Edit",
                enabled,
                std::nullopt,
                std::string(shortcut_key),
                false,
                true,
                false,
                false,
                false,
                1'000,
                false,
                false,
                std::move(action),
                std::nullopt,
            };
            const auto [inserted, unique] = entries_.emplace(command.id, std::move(command));
            if (unique) declaration_order_.push_back(&inserted->second);
        };
        add_undo_command(
            "application.undo",
            status.undo_label.has_value() ? "Undo " + *status.undo_label : "Undo",
            status.can_undo,
            "z"
        );
        add_undo_command(
            "application.redo",
            status.redo_label.has_value() ? "Redo " + *status.redo_label : "Redo",
            status.can_redo,
            "y"
        );
    }
}

const CommandSnapshot* CommandIndex::find(const std::string_view id) const noexcept {
    const auto found = entries_.find(id);
    return found != entries_.end() ? &found->second : nullptr;
}

std::optional<CommandActivationBinding> CommandIndex::activation_binding(
    const RetainedNode& node
) const noexcept {
    const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
    if (lifecycle == nullptr || lifecycle->command.activation_reference_property.empty()) {
        return std::nullopt;
    }
    const std::string* id = text(property(
        node,
        lifecycle->command.activation_reference_property
    ));
    if (id == nullptr || id->empty()) return std::nullopt;
    return CommandActivationBinding{*id, find(*id)};
}

std::vector<const CommandSnapshot*> CommandIndex::referenced_by(const RetainedNode& node) const {
    return reference_projection(node).commands;
}

CommandReferenceProjection CommandIndex::reference_projection(
    const RetainedNode& node
) const {
    CommandReferenceProjection result;
    WidgetCommandPhase phase;
    if (const WidgetLifecycle* lifecycle = widgets_.find(node.description().type);
        lifecycle != nullptr) {
        phase = lifecycle->command;
    }
    if (phase.references_self) {
        result.constrained = true;
        if (const std::string* id = text(property(node, "id")); id != nullptr) {
            if (const CommandSnapshot* command = find(*id); command != nullptr) {
                result.commands.push_back(command);
            }
        }
        return result;
    }
    if (!phase.item_collection_property.empty() && !phase.item_reference_property.empty()) {
        const runtime::Value* items = property(node, phase.item_collection_property);
        result.constrained = items != nullptr;
        if (items != nullptr && items->list() != nullptr) {
            const auto collect = [this, &phase, &result](
                                     const auto& self,
                                     const runtime::ValueList& level
                                 ) -> void {
                for (const runtime::Value& item : level.values) {
                    const std::string* id = text(item.field(phase.item_reference_property));
                    if (id != nullptr) {
                        if (const CommandSnapshot* command = find(*id); command != nullptr) {
                            result.commands.push_back(command);
                        }
                    }
                    const runtime::Value* children = item.field("children");
                    if (children != nullptr && children->list() != nullptr) {
                        self(self, *children->list());
                    }
                }
            };
            collect(collect, *items->list());
        }
        return result;
    }
    if (phase.references_property.empty()) return result;
    const runtime::Value* references = property(node, phase.references_property);
    const bool empty_list = references != nullptr && references->list() != nullptr &&
        references->list()->values.empty();
    if (references == nullptr || empty_list) {
        if (phase.all_when_unreferenced) result.commands = declaration_order_;
        return result;
    }
    result.constrained = true;
    if (const std::string* id = text(references); id != nullptr) {
        if (const CommandSnapshot* command = find(*id); command != nullptr) {
            result.commands.push_back(command);
        }
        return result;
    }
    if (references->list() != nullptr) {
        for (const runtime::Value& reference : references->list()->values) {
            const std::string* id = text(&reference);
            if (id == nullptr) continue;
            if (const CommandSnapshot* command = find(*id); command != nullptr) {
                result.commands.push_back(command);
            }
        }
    }
    return result;
}

const std::vector<const CommandSnapshot*>& CommandIndex::ordered_entries() const noexcept {
    return declaration_order_;
}

const std::map<std::string, CommandSnapshot, std::less<>>& CommandIndex::entries() const noexcept {
    return entries_;
}

void CommandIndex::record_execution(const std::string_view id) {
    if (!entries_.contains(id)) return;
    if (execution_serial_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("surface command execution serial exhausted");
    }
    ++execution_serial_;
    recent_.insert_or_assign(std::string(id), execution_serial_);
    if (durability_ != nullptr && !persistence_scope_.empty()) {
        std::vector<std::pair<std::string, std::uint64_t>> ordered(recent_.begin(), recent_.end());
        std::ranges::sort(ordered, std::greater{}, &decltype(ordered)::value_type::second);
        std::vector<std::string> ids;
        ids.reserve(ordered.size());
        for (const auto& [command_id, serial] : ordered) {
            static_cast<void>(serial);
            ids.push_back(command_id);
        }
        durability_->set_command_recency(persistence_scope_, std::move(ids));
    }
}

std::int64_t CommandIndex::recent_rank(const std::string_view id) const noexcept {
    const auto found = recent_.find(id);
    if (found == recent_.end()) return std::numeric_limits<std::int64_t>::min();
    return found->second > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
        ? std::numeric_limits<std::int64_t>::max()
        : static_cast<std::int64_t>(found->second);
}

void CommandIndex::clear() noexcept {
    entries_.clear();
    declaration_order_.clear();
    recent_.clear();
    execution_serial_ = 0U;
    observed_tree_ = nullptr;
    observed_tree_generation_ = 0U;
    observed_undo_generation_ = 0U;
}

} // namespace strata::ui
