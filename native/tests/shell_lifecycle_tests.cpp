#include <algorithm>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

#include "compiler/source.hpp"
#include "data/json.hpp"
#include "resource/resource.hpp"
#include "runtime/application.hpp"
#include "ui/command.hpp"
#include "ui/notification.hpp"
#include "ui/surface.hpp"
#include "ui/text.hpp"
#include "ui/widget/shell_model.hpp"
#include "ui/widget/subtarget.hpp"

namespace {

using namespace strata;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::shared_ptr<const runtime::ApplicationBundle> load_bundle(
    const std::filesystem::path& registry_path
) {
    const std::string registry = resource::load_utf8_resource(
        registry_path.parent_path(),
        resource::ResourceId::parse(registry_path.filename().generic_string())
    );
    return runtime::ApplicationBundle::create(data::parse_json(registry));
}

[[nodiscard]] compiler::ModuleLoader no_imports() {
    return [](const std::string_view, const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected shell fixture import '" + std::string(path) + "'");
    };
}

[[nodiscard]] ui::SurfaceEnvironment environment() {
    ui::SurfaceEnvironment result;
    result.framebuffer_width = 800;
    result.framebuffer_height = 600;
    result.logical_width = 800.0;
    result.logical_height = 600.0;
    result.reduced_motion = true;
    result.input = ui::SurfaceInputCapabilities{
        true, ui::PointerPrecision::fine, true, false, true, true, false,
    };
    return result;
}

[[nodiscard]] runtime::Value object(
    std::initializer_list<std::pair<std::string, runtime::Value>> fields
) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>(fields));
}

[[nodiscard]] const ui::WidgetSubtarget* target(
    const std::vector<ui::WidgetSubtarget>& targets,
    const std::string_view id
) {
    const auto found = std::ranges::find(targets, id, &ui::WidgetSubtarget::id);
    return found != targets.end() ? &*found : nullptr;
}

[[nodiscard]] const data::JsonValue* child_named(
    const data::JsonValue& node,
    const std::string_view name
) {
    const data::JsonValue* children = node.find("children");
    if (children == nullptr || children->array() == nullptr) return nullptr;
    for (const data::JsonValue& child : *children->array()) {
        const data::JsonValue* child_name = child.find("name");
        if (child_name != nullptr && child_name->string() != nullptr &&
            *child_name->string() == name) {
            return &child;
        }
    }
    return nullptr;
}

[[nodiscard]] const data::JsonValue* semantic_state(
    const data::JsonValue* node,
    const std::string_view field
) {
    const data::JsonValue* state = node != nullptr ? node->find("state") : nullptr;
    return state != nullptr ? state->find(field) : nullptr;
}

[[nodiscard]] std::vector<std::string> semantic_command_ids(
    const data::JsonValue* node
) {
    std::vector<std::string> result;
    const data::JsonValue* children = node != nullptr ? node->find("children") : nullptr;
    if (children == nullptr || children->array() == nullptr) return result;
    for (const data::JsonValue& child : *children->array()) {
        const data::JsonValue* role = child.find("role");
        const data::JsonValue* id = child.find("virtualCommandId");
        if (role != nullptr && role->string() != nullptr && *role->string() == "menu_item" &&
            id != nullptr && id->string() != nullptr) {
            result.push_back(*id->string());
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::string> all_semantic_command_ids(
    const data::JsonValue* node
) {
    std::vector<std::string> result;
    const data::JsonValue* children = node != nullptr ? node->find("children") : nullptr;
    if (children == nullptr || children->array() == nullptr) return result;
    for (const data::JsonValue& child : *children->array()) {
        const data::JsonValue* id = child.find("virtualCommandId");
        if (id != nullptr && id->string() != nullptr) result.push_back(*id->string());
    }
    return result;
}

void test_notification_queue() {
    std::vector<ui::NotificationChange> changes;
    ui::NotificationService notifications(
        [&changes](const ui::NotificationChange& change) { changes.push_back(change); },
        2U
    );
    const std::uint64_t first = notifications.raise(ui::NotificationRequest{
        "first", ui::NotificationSeverity::info, true,
    });
    const std::uint64_t second = notifications.raise(ui::NotificationRequest{
        "second", ui::NotificationSeverity::warning, true,
    });
    const std::uint64_t third = notifications.raise(ui::NotificationRequest{
        "third", ui::NotificationSeverity::error, true,
    });
    check(first == 1U && second == 2U && third == 3U, "notification ids are not monotonic");
    check(
        notifications.size() == 2U && notifications.find(first) == nullptr &&
            notifications.find(second) != nullptr && notifications.find(third) != nullptr,
        "bounded notification eviction changed"
    );
    check(
        changes.size() == 3U && changes.back().kind == ui::NotificationChangeKind::raised &&
            changes.back().input && changes.back().render && changes.back().semantics,
        "notification raise did not publish targeted input/render/semantics invalidation"
    );
    check(notifications.pause(third, true), "persistent notification did not pause");
    check(
        changes.back().kind == ui::NotificationChangeKind::paused &&
            changes.back().notification_id == std::optional(third) &&
            changes.back().input && !changes.back().render && !changes.back().semantics,
        "notification pause did not publish its input-only dirty payload"
    );
    check(notifications.pause(third, false), "persistent notification did not resume");
    check(
        changes.back().kind == ui::NotificationChangeKind::resumed &&
            changes.back().notification_id == std::optional(third) &&
            changes.back().input && !changes.back().render && !changes.back().semantics,
        "notification resume did not publish its input-only dirty payload"
    );

    ui::NotificationService expiring;
    const std::uint64_t paused = expiring.raise(ui::NotificationRequest{
        "pause", ui::NotificationSeverity::info, false, 10,
    });
    expiring.advance_frame(5'000'000);
    check(expiring.pause(paused, true), "notification did not pause");
    expiring.advance_frame(100'000'000);
    check(expiring.find(paused) != nullptr, "paused notification expired");
    check(expiring.pause(paused, false), "notification did not resume");
    expiring.advance_frame(104'000'000);
    check(expiring.find(paused) != nullptr, "resumed notification lost its remaining time");
    expiring.advance_frame(106'000'000);
    check(expiring.find(paused) == nullptr, "resumed notification did not expire");
}

void test_surface_shell_lifecycle(
    const std::shared_ptr<const runtime::ApplicationBundle>& bundle,
    const std::filesystem::path& registry_path
) {
    constexpr std::string_view source = R"(
style OutsideFocus {
  focusRing: { width: 2, color: #70AAFAFF, inside: false };
}
component ShellFixture() {
  Panel(key: "shell.root", layout: { kind: "COLUMN", width: { weight: 1 }, height: { weight: 1 }, gap: 8 }) {
    Command(
      id: "shell.save", label: "Save workspace", category: "File", checked: true,
      action: action("notification.raise", message: "saved", severity: "SUCCESS")
    )
    Command(
      id: "aaa.default", label: "Default category command",
      action: action("notification.raise", message: "default")
    )
    Command(
      id: "shell.disabled", label: "Disabled command", enabled: false,
      action: action("notification.raise", message: "disabled")
    )
    Command(
      id: "shell.unicode", label: "😀Äpfel",
      action: action("notification.raise", message: "unicode")
    )
    Toolbar(key: "shell.toolbar", visualStyle: OutsideFocus, layout: { width: 300 })
    Menu(
      key: "nested.menu", label: "Actions",
      items: [
        {
          id: "file", label: "File",
          children: [
            { id: "save", command: "shell.save" },
            { separator: true },
            {
              id: "more", label: "More",
              children: [{ id: "plain", label: "Plain action" }]
            }
          ]
        }
      ]
    )
    ContextMenu(
      key: "nested.context",
      layout: { width: 180, height: 40 },
      items: [{ id: "context-root", label: "Context root", children: [
        { id: "context-leaf", label: "Context leaf" }
      ] }]
    ) {
      Text(text: "Right-click here")
    }
    CommandPalette(key: "shell.palette", maxVisibleRows: 20)
    CommandPalette(key: "shell.empty", maxVisibleRows: 20)
    CommandPalette(key: "shell.constrained", commands: ["missing"], maxVisibleRows: 4)
    CommandPalette(
      key: "shell.visual", commands: ["shell.save"],
      items: [{ id: "alpha", label: "Alpha" }, { id: "beta", label: "Beta" }]
    )
    MenuBar(key: "shell.menu-bar")
    ToastRegion(
      key: "shell.toasts", maxVisible: 2, width: 180, minHeight: 32,
      maxMessageLines: 2
    )
  }
}
overlay Main { root ShellFixture() }
)";
    runtime::ApplicationContext application("shell-lifecycle", bundle);
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{"shell-lifecycle.strata", std::string(source)},
        no_imports(),
        0U
    );
    if (!activation.activated()) {
        std::string message = "shell fixture did not activate";
        for (const runtime::ActivationDiagnostic& diagnostic : activation.diagnostics) {
            message += " [" + diagnostic.code + ": " + diagnostic.message + "]";
        }
        throw std::runtime_error(message);
    }
    ui::Surface surface(
        "shell-lifecycle",
        application,
        runtime::LayerRole::overlay,
        "Main",
        environment(),
        ui::TextEngine::load_default_fonts(registry_path.parent_path().parent_path())
    );
    static_cast<void>(surface.frame(1'000'000));

    ui::RetainedNode* palette = surface.tree().find_key("shell.palette");
    ui::RetainedNode* empty = surface.tree().find_key("shell.empty");
    ui::RetainedNode* constrained = surface.tree().find_key("shell.constrained");
    ui::RetainedNode* visual = surface.tree().find_key("shell.visual");
    ui::RetainedNode* menu_bar = surface.tree().find_key("shell.menu-bar");
    ui::RetainedNode* toolbar = surface.tree().find_key("shell.toolbar");
    check(
        palette != nullptr && empty != nullptr && constrained != nullptr && visual != nullptr &&
            menu_bar != nullptr && toolbar != nullptr,
        "command surfaces were not retained"
    );
    const data::JsonValue* closed_palette_semantics =
        surface.semantics().find(palette->identity());
    const data::JsonValue* closed_expanded = semantic_state(
        closed_palette_semantics, "expanded"
    );
    const data::JsonValue* closed_value = semantic_state(
        closed_palette_semantics, "valueText"
    );
    const std::vector<std::string> closed_palette_command_ids =
        semantic_command_ids(closed_palette_semantics);
    check(
        closed_expanded != nullptr && closed_expanded->boolean() != nullptr &&
            !*closed_expanded->boolean() && closed_value != nullptr &&
            closed_value->string() != nullptr && *closed_value->string() == "false" &&
            closed_palette_command_ids == std::vector<std::string>{
                "shell.save", "aaa.default", "shell.disabled", "shell.unicode",
                "application.undo", "application.redo",
            },
        "closed palette semantics did not freeze the CommandIndex projection"
    );
    const data::JsonValue* disabled_semantics = child_named(
        *closed_palette_semantics,
        "Disabled command"
    );
    const data::JsonValue* disabled_actions = disabled_semantics != nullptr
        ? disabled_semantics->find("actions")
        : nullptr;
    check(
        disabled_actions != nullptr && disabled_actions->array() != nullptr &&
            disabled_actions->array()->empty(),
        "disabled virtual command exposed an activate action"
    );
    const data::JsonValue* visual_semantics = surface.semantics().find(visual->identity());
    check(
        visual_semantics != nullptr &&
            semantic_command_ids(visual_semantics) ==
                std::vector<std::string>{"shell.save"} &&
            child_named(*visual_semantics, "Alpha") == nullptr &&
            child_named(*visual_semantics, "Beta") == nullptr,
        "palette semantics projected authored picker items instead of referenced commands"
    );
    const ui::PaletteProjection authored_order = ui::project_command_palette(
        *palette, surface.layout(), &surface.commands(), {}
    );
    check(
        authored_order.matches.size() == 3U &&
            authored_order.matches[0].id == "shell.save" &&
            authored_order.matches[1].id == "aaa.default" &&
            authored_order.matches[2].id == "shell.unicode",
        "unconstrained palette lost retained declaration order for equal-rank commands"
    );
    const ui::PaletteProjection empty_order = ui::project_command_palette(
        *empty, surface.layout(), &surface.commands(), {}
    );
    check(
        empty_order.matches.size() == authored_order.matches.size() &&
            empty_order.matches[0].id == authored_order.matches[0].id &&
            empty_order.matches[1].id == authored_order.matches[1].id &&
            empty_order.matches[2].id == authored_order.matches[2].id &&
            !surface.commands().reference_projection(*empty).constrained,
        "omitted command projection diverged between equivalent command surfaces"
    );
    const std::vector<std::string> all_command_ids{
        "shell.save", "aaa.default", "shell.disabled", "shell.unicode",
        "application.undo", "application.redo",
    };
    check(
        surface.commands().referenced_by(*menu_bar).size() == all_command_ids.size() &&
            surface.commands().referenced_by(*toolbar).size() == all_command_ids.size() &&
            all_semantic_command_ids(surface.semantics().find(menu_bar->identity())) ==
                all_command_ids &&
            all_semantic_command_ids(surface.semantics().find(toolbar->identity())) ==
                all_command_ids,
        "MenuBar or Toolbar bypassed canonical omitted/empty command projection"
    );
    std::vector<ui::WidgetSubtarget> toolbar_targets =
        surface.input().subtargets(toolbar->identity());
    const auto overflow = std::ranges::find(
        toolbar_targets, std::string_view("$overflow"), &ui::WidgetSubtarget::id
    );
    const std::size_t visible_toolbar_commands = std::ranges::count_if(
        toolbar_targets,
        [](const ui::WidgetSubtarget& target) {
            return !target.detached && target.kind == ui::WidgetSubtargetKind::command;
        }
    );
    check(
        overflow != toolbar_targets.end() && !overflow->detached &&
            std::ranges::all_of(toolbar_targets, [&](const ui::WidgetSubtarget& target) {
                return target.detached || target.bounds.right() <=
                    surface.layout().find(toolbar->identity())->bounds.right();
            }),
        "Toolbar did not replace out-of-bounds commands with an overflow trigger"
    );
    const auto first_visible_command = std::ranges::find_if(
        toolbar_targets,
        [](const ui::WidgetSubtarget& target) {
            return !target.detached && target.kind == ui::WidgetSubtargetKind::command;
        }
    );
    check(
        first_visible_command != toolbar_targets.end() && overflow->bounds.width <= 48.0 &&
            first_visible_command->bounds.width < 160.0,
        "Toolbar retained oversized fixed command/overflow slots without itemWidth"
    );
    const ui::Point overflow_center{
        overflow->bounds.x + overflow->bounds.width * 0.5,
        overflow->bounds.y + overflow->bounds.height * 0.5,
    };
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        overflow_center, ui::PointerEventType::press, 41, 0,
    }));
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        overflow_center, ui::PointerEventType::release, 41, 0,
    }));
    static_cast<void>(surface.input().process_queued());
    toolbar_targets = surface.input().subtargets(toolbar->identity());
    std::size_t detached_toolbar_commands = 0U;
    std::size_t previous_toolbar_index = 0U;
    bool ordered_toolbar_overflow = true;
    for (const ui::WidgetSubtarget& target : toolbar_targets) {
        if (!target.detached || target.kind != ui::WidgetSubtargetKind::command) continue;
        ordered_toolbar_overflow = ordered_toolbar_overflow &&
            target.index >= visible_toolbar_commands &&
            (detached_toolbar_commands == 0U || target.index > previous_toolbar_index);
        previous_toolbar_index = target.index;
        ++detached_toolbar_commands;
    }
    check(
        detached_toolbar_commands > 0U && ordered_toolbar_overflow,
        "Toolbar overflow trigger did not open ordered detached command rows"
    );
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        ui::Point{-1.0, -1.0}, ui::PointerEventType::press, 42, 0,
    }));
    static_cast<void>(surface.input().process_queued());
    toolbar_targets = surface.input().subtargets(toolbar->identity());
    check(
        std::ranges::none_of(toolbar_targets, &ui::WidgetSubtarget::detached) &&
            !surface.input().focused_key().has_value(),
        "blank-background press did not dismiss Toolbar overflow and clear focus"
    );
    const auto refocus_toolbar = std::ranges::find_if(
        toolbar_targets,
        [](const ui::WidgetSubtarget& target) {
            return !target.detached && target.enabled &&
                target.kind == ui::WidgetSubtargetKind::command;
        }
    );
    check(refocus_toolbar != toolbar_targets.end(), "Toolbar lost its visible focus target");
    const ui::Point refocus_toolbar_center{
        refocus_toolbar->bounds.x + refocus_toolbar->bounds.width * 0.5,
        refocus_toolbar->bounds.y + refocus_toolbar->bounds.height * 0.5,
    };
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        refocus_toolbar_center, ui::PointerEventType::press, 142, 0,
    }));
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        refocus_toolbar_center, ui::PointerEventType::cancel, 142, 0,
    }));
    static_cast<void>(surface.input().process_queued());
    static_cast<void>(surface.frame(1'250'000));
    static_cast<void>(surface.input().key("right"));
    const runtime::Value* toolbar_index = toolbar->retained_value("$shellIndex");
    check(
        toolbar_index != nullptr && toolbar_index->number() != nullptr &&
            *toolbar_index->number() == 1.0,
        "focused horizontal Toolbar did not rove to its next command with Right"
    );
    const auto focused_border_at = [&surface](const ui::Rect bounds) {
        const ui::Rect focus_bounds{
            bounds.x - 4.0,
            bounds.y - 4.0,
            bounds.width + 8.0,
            bounds.height + 8.0,
        };
        return std::ranges::any_of(surface.render_commands().commands(), [focus_bounds](
            const ui::RenderCommand& command
        ) {
            const auto* border = std::get_if<ui::BorderRenderCommand>(&command);
            return border != nullptr && border->bounds == focus_bounds &&
                border->border.width == 2.0 && border->border.color.alpha == 255U &&
                border->border.inside;
        });
    };
    toolbar_targets = surface.input().subtargets(toolbar->identity());
    std::vector<const ui::WidgetSubtarget*> enabled_toolbar_targets;
    for (const ui::WidgetSubtarget& target : toolbar_targets) {
        if (!target.detached && target.enabled &&
            (target.kind == ui::WidgetSubtargetKind::command || target.id == "$overflow")) {
            enabled_toolbar_targets.push_back(&target);
        }
    }
    static_cast<void>(surface.frame(1'300'000));
    check(
        enabled_toolbar_targets.size() > 1U &&
            focused_border_at(enabled_toolbar_targets[1]->bounds),
        "Toolbar roving state did not render focus around its active command"
    );

    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        overflow_center, ui::PointerEventType::press, 43, 0,
    }));
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        overflow_center, ui::PointerEventType::release, 43, 0,
    }));
    static_cast<void>(surface.input().process_queued());
    static_cast<void>(surface.frame(1'350'000));
    toolbar_targets = surface.input().subtargets(toolbar->identity());
    const auto first_overflow_row = std::ranges::find_if(
        toolbar_targets,
        [](const ui::WidgetSubtarget& target) {
            return target.detached && target.enabled &&
                target.kind == ui::WidgetSubtargetKind::command;
        }
    );
    check(
        first_overflow_row != toolbar_targets.end() &&
            !focused_border_at(first_overflow_row->bounds),
        "pointer-opened Toolbar overflow retained a keyboard-only focus ring"
    );
    static_cast<void>(surface.input().key("escape"));
    static_cast<void>(surface.frame(1'400'000));

    static_cast<void>(surface.dispatch_action(
        "focus.request",
        object({{"key", runtime::Value(runtime::KeyValue{"shell.menu-bar"})}}),
        "test",
        std::nullopt,
        runtime::Value{}
    ));
    static_cast<void>(surface.frame(1'410'000));
    std::vector<ui::WidgetSubtarget> menu_bar_targets =
        surface.input().subtargets(menu_bar->identity());
    std::vector<const ui::WidgetSubtarget*> menu_categories;
    for (const ui::WidgetSubtarget& target : menu_bar_targets) {
        if (!target.detached && target.enabled &&
            target.kind == ui::WidgetSubtargetKind::control) {
            menu_categories.push_back(&target);
        }
    }
    check(
        menu_categories.size() == 3U && focused_border_at(menu_categories[0]->bounds),
        "MenuBar did not render focus around its initial category"
    );
    static_cast<void>(surface.input().key("right"));
    static_cast<void>(surface.frame(1'420'000));
    const runtime::Value* menu_index = menu_bar->retained_value("$shellIndex");
    check(
        menu_index != nullptr && menu_index->number() != nullptr &&
            *menu_index->number() == 1.0 && focused_border_at(menu_categories[1]->bounds),
        "MenuBar did not visibly rove to its next category with Right"
    );
    static_cast<void>(surface.input().key("enter"));
    static_cast<void>(surface.frame(1'430'000));
    menu_bar_targets = surface.input().subtargets(menu_bar->identity());
    std::vector<const ui::WidgetSubtarget*> menu_rows;
    for (const ui::WidgetSubtarget& target : menu_bar_targets) {
        if (target.detached && target.enabled &&
            target.kind == ui::WidgetSubtargetKind::command) {
            menu_rows.push_back(&target);
        }
    }
    check(
        menu_rows.size() == 2U && focused_border_at(menu_rows[0]->bounds),
        "MenuBar popup did not render focus around its initial command"
    );
    static_cast<void>(surface.input().key("down"));
    static_cast<void>(surface.frame(1'440'000));
    check(
        focused_border_at(menu_rows[1]->bounds),
        "MenuBar popup did not visibly rove between enabled commands with Down"
    );
    static_cast<void>(surface.input().key("escape"));
    static_cast<void>(surface.frame(1'450'000));
    menu_index = menu_bar->retained_value("$shellIndex");
    const runtime::Value* closed_menu_category = menu_bar->retained_value("$menuCategory");
    check(
        surface.input().focused(menu_bar->identity()) &&
            menu_index != nullptr && menu_index->number() != nullptr &&
            *menu_index->number() == 1.0 &&
            (closed_menu_category == nullptr || closed_menu_category->string() == nullptr ||
             closed_menu_category->string()->empty()),
        "MenuBar did not retain its category index and focus while closing the popup"
    );
    menu_bar_targets = surface.input().subtargets(menu_bar->identity());
    menu_categories.clear();
    for (const ui::WidgetSubtarget& target : menu_bar_targets) {
        if (!target.detached && target.enabled &&
            target.kind == ui::WidgetSubtargetKind::control) {
            menu_categories.push_back(&target);
        }
    }
    check(
        menu_categories.size() == 3U && focused_border_at(menu_categories[1]->bounds),
        "MenuBar did not restore category focus after closing its popup"
    );

    const ui::PaletteProjection constrained_projection = ui::project_command_palette(
        *constrained, surface.layout(), &surface.commands(), {}
    );
    check(
        constrained_projection.matches.empty() &&
            surface.commands().reference_projection(*constrained).constrained,
        "explicit unresolved palette references fell back to every command"
    );
    const ui::PaletteProjection category_projection = ui::project_command_palette(
        *palette, surface.layout(), &surface.commands(), "commands"
    );
    check(
        std::ranges::any_of(category_projection.matches, [](const ui::PaletteEntryModel& entry) {
            return entry.id == "aaa.default" && entry.searchable_detail == "Commands";
        }),
        "missing command category did not default/search as Commands"
    );
    const ui::PaletteProjection unicode_projection = ui::project_command_palette(
        *palette, surface.layout(), &surface.commands(), "äp"
    );
    const auto unicode = std::ranges::find(
        unicode_projection.matches, std::string("shell.unicode"), &ui::PaletteEntryModel::id
    );
    check(
        unicode != unicode_projection.matches.end() && unicode->label_spans.size() == 1U &&
            unicode->label_spans.front() == ui::QuickPickMatchSpan{2U, 4U},
        "Unicode palette highlight spans were not projected in UTF-16 coordinates"
    );
    const std::int64_t unseen_rank = std::numeric_limits<std::int64_t>::min();
    check(
        surface.commands().recent_rank("shell.save") == unseen_rank &&
            surface.commands().recent_rank("shell.disabled") == unseen_rank,
        "commands began with invented recency"
    );
    static_cast<void>(surface.dispatch_action(
        "command.execute",
        object({{"id", runtime::Value("shell.unicode")}}),
        "test",
        std::nullopt,
        runtime::Value{}
    ));
    const ui::PaletteProjection recent_projection = ui::project_command_palette(
        *palette, surface.layout(), &surface.commands(), {}
    );
    check(
        surface.commands().recent_rank("shell.unicode") > unseen_rank &&
            !recent_projection.matches.empty() &&
            recent_projection.matches.front().id == "shell.unicode",
        "executed command recency did not feed reusable quick-picker ordering"
    );
    static_cast<void>(surface.dispatch_action(
        "command.execute",
        object({{"id", runtime::Value("shell.disabled")}}),
        "test",
        std::nullopt,
        runtime::Value{}
    ));
    check(
        surface.commands().recent_rank("shell.disabled") == unseen_rank,
        "rejected command invocation changed palette recency"
    );
    static_cast<void>(surface.dispatch_action(
        "command.execute",
        object({{"id", runtime::Value("shell.save")}}),
        "test",
        std::nullopt,
        runtime::Value{}
    ));
    check(
        surface.commands().recent_rank("shell.save") >
                surface.commands().recent_rank("shell.unicode") &&
            ui::project_command_palette(
                *palette, surface.layout(), &surface.commands(), {}
            ).matches.front().id == "shell.save",
        "newer command execution did not replace the palette recency leader"
    );
    surface.notifications().clear();

    ui::RetainedNode* menu = surface.tree().find_key("nested.menu");
    check(menu != nullptr, "nested Menu was not retained");
    check(
        surface.commands().referenced_by(*menu).size() == 1U,
        "recursive menu command discovery omitted nested command references"
    );
    static_cast<void>(surface.input().click("nested.menu"));
    std::vector<ui::WidgetSubtarget> menu_targets = surface.input().subtargets(menu->identity());
    check(
        target(menu_targets, "$menu/0") != nullptr &&
            target(menu_targets, "$menu/0/0") != nullptr &&
            target(menu_targets, "$menu/0/1") != nullptr,
        "nested menu did not project its root and child panels"
    );
    check(
        target(menu_targets, "$menu/0/1")->kind == ui::WidgetSubtargetKind::separator,
        "nested menu separator lost its presenter-independent identity"
    );
    static_cast<void>(surface.input().key("left"));
    menu_targets = surface.input().subtargets(menu->identity());
    check(
        target(menu_targets, "$menu/0") != nullptr,
        "Left at the root menu level changed the preserved consume-without-close behavior"
    );
    static_cast<void>(surface.input().key("right"));
    static_cast<void>(surface.input().key("down"));
    static_cast<void>(surface.input().key("right"));
    menu_targets = surface.input().subtargets(menu->identity());
    check(
        target(menu_targets, "$menu/0/2/0") != nullptr,
        "keyboard traversal did not open the recursive submenu"
    );
    static_cast<void>(surface.input().key("escape"));

    ui::RetainedNode* context = surface.tree().find_key("nested.context");
    const ui::LayoutRecord* context_layout = context != nullptr
        ? surface.layout().find(context->identity()) : nullptr;
    check(context != nullptr && context_layout != nullptr, "ContextMenu was not retained or laid out");
    const ui::Point context_point{
        context_layout->bounds.x + context_layout->bounds.width * 0.5,
        context_layout->bounds.y + context_layout->bounds.height * 0.5,
    };
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        context_point, ui::PointerEventType::press, 9, 1,
    }));
    static_cast<void>(surface.input().process_queued());
    const std::vector<ui::WidgetSubtarget> context_targets =
        surface.input().subtargets(context->identity());
    check(
        target(context_targets, "$menu/0") != nullptr &&
            target(context_targets, "$menu/0/0") != nullptr,
        "ContextMenu did not share recursive menu geometry after secondary press"
    );
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        context_point, ui::PointerEventType::release, 9, 1,
    }));
    static_cast<void>(surface.input().process_queued());

    static_cast<void>(surface.dispatch_action(
        "palette.set",
        object({{"key", runtime::Value(runtime::KeyValue{"shell.palette"})},
                {"open", runtime::Value(true)}}),
        "test",
        std::nullopt,
        runtime::Value{}
    ));
    check(palette != nullptr && surface.input().focused(palette->identity()),
          "palette.set did not establish modal palette focus");
    static_cast<void>(surface.input().text("save"));
    const std::vector<ui::WidgetSubtarget> palette_targets =
        surface.input().subtargets(palette->identity());
    check(
        target(palette_targets, "$scrim") != nullptr &&
            target(palette_targets, "$editor") != nullptr &&
            target(palette_targets, "$palette/shell.save") != nullptr,
        "command palette did not project its scrim, editor, and filtered command"
    );
    static_cast<void>(surface.frame(1'500'000));
    const data::JsonValue* open_palette_semantics =
        surface.semantics().find(palette->identity());
    const data::JsonValue* open_expanded = semantic_state(open_palette_semantics, "expanded");
    const data::JsonValue* open_value = semantic_state(open_palette_semantics, "valueText");
    check(
        open_expanded != nullptr && open_expanded->boolean() != nullptr &&
            *open_expanded->boolean() && open_value != nullptr &&
            open_value->string() != nullptr && *open_value->string() == "true" &&
            semantic_command_ids(open_palette_semantics) == closed_palette_command_ids,
        "open palette semantics changed its frozen command projection"
    );

    const std::string long_message =
        "Failure while saving a workspace with a deliberately long Unicode message 😀 that "
        "must wrap and truncate from actual glyph metrics";
    const ui::CommandSnapshot* save_command = surface.commands().find("shell.save");
    check(save_command != nullptr && save_command->action != nullptr,
          "toast action fixture command was not retained");
    const std::uint64_t long_toast = surface.notifications().raise(ui::NotificationRequest{
        long_message,
        ui::NotificationSeverity::error,
        true,
        std::nullopt,
        "Retry",
        save_command->action,
    });
    const ui::SurfaceFrame toast_frame = surface.frame(2'000'000);
    check(
        toast_frame.operations.rebuilds == 0U,
        "notification dirty payload triggered an unrelated description reconciliation"
    );
    ui::RetainedNode* toasts = surface.tree().find_key("shell.toasts");
    check(toasts != nullptr, "ToastRegion was not retained");
    const std::vector<ui::WidgetSubtarget> toast_targets =
        surface.input().subtargets(toasts->identity());
    const auto toast = std::ranges::find_if(toast_targets, [](const ui::WidgetSubtarget& value) {
        return value.kind == ui::WidgetSubtargetKind::notification;
    });
    check(
        toast != toast_targets.end() && toast->notification_id.has_value() &&
            target(toast_targets, toast->id + "/dismiss") != nullptr,
        "ToastRegion did not expose stable card/dismiss subtargets"
    );
    const ui::ToastProjection toast_projection = ui::project_toasts(
        *toasts,
        surface.layout(),
        surface.notifications(),
        [&surface](
            const ui::RetainedNode& owner,
            const std::string_view text,
            const ui::TextLayoutOptions& options
        ) {
            return surface.text_engine()->layout(owner, text, options);
        }
    );
    const auto long_card = std::ranges::find_if(
        toast_projection.cards,
        [long_toast](const ui::ToastCardModel& card) {
            return card.notification.id == long_toast;
        }
    );
    check(
        long_card != toast_projection.cards.end() &&
            long_card->message_layout.lines.size() == 2U &&
            long_card->message_layout.truncated &&
            long_card->message_layout.wrap_width ==
                std::optional(long_card->message_bounds.width) &&
            long_card->action_layout.lines.size() == 1U &&
            long_card->action_layout.wrap_width ==
                std::optional(long_card->action_bounds.width) &&
            long_card->bounds.height ==
                std::max(
                    32.0,
                    20.0 + long_card->message_layout.shaped.metrics.height +
                        std::max(24.0, long_card->action_layout.shaped.metrics.height + 6.0)
                ),
        "toast card/action geometry did not consume the immutable wrapped/ellipsized TextLayouts"
    );
    check(
        std::ranges::any_of(surface.render_commands().commands(), [&long_card](
            const ui::RenderCommand& command
        ) {
            const auto* clip = std::get_if<ui::ClipPushRenderCommand>(&command);
            return clip != nullptr && clip->rect == long_card->message_bounds;
        }) &&
            std::ranges::any_of(surface.render_commands().commands(), [&long_card](
                const ui::RenderCommand& command
            ) {
                const auto* clip = std::get_if<ui::ClipPushRenderCommand>(&command);
                return clip != nullptr && clip->rect == long_card->action_bounds;
            }),
        "toast renderer did not clip the exact projected message/action bounds"
    );

    const std::vector<ui::DetachedOverlayRoot> overlay_roots = ui::detached_overlay_roots(
        surface.tree(),
        surface.layout(),
        [&surface](const ui::RetainedNode& node) {
            return std::ranges::any_of(
                surface.input().subtargets(node.identity()),
                &ui::WidgetSubtarget::detached
            );
        }
    );
    check(
        !overlay_roots.empty() && overlay_roots.back().node == toasts &&
            overlay_roots.back().z_index == ui::detached_overlay_toast_z,
        "shared overlay substrate did not put ToastRegion above menu/palette paint and hit roots"
    );
    const data::JsonValue* toast_semantics = surface.semantics().find(toasts->identity());
    const data::JsonValue* failure = toast_semantics != nullptr
        ? child_named(*toast_semantics, long_message) : nullptr;
    check(
        failure != nullptr && failure->find("liveRegion") != nullptr &&
            failure->find("liveRegion")->string() != nullptr &&
            *failure->find("liveRegion")->string() == "assertive" &&
            failure->find("virtualNotificationId") != nullptr,
        "error notification semantics are not assertive/stably identified"
    );
    const std::vector<ui::DetachedOverlayRoot> author_order_roots =
        ui::detached_overlay_roots(
            surface.tree(),
            surface.layout(),
            [](const ui::RetainedNode& node) {
                return node.description().type == "Menu";
            }
        );
    const auto nested_menu_root = std::ranges::find(
        author_order_roots, menu, &ui::DetachedOverlayRoot::node
    );
    const auto context_menu_root = std::ranges::find(
        author_order_roots, context, &ui::DetachedOverlayRoot::node
    );
    check(
        nested_menu_root != author_order_roots.end() &&
            context_menu_root != author_order_roots.end() &&
            nested_menu_root->z_index == context_menu_root->z_index &&
            nested_menu_root->author_order < context_menu_root->author_order &&
            nested_menu_root < context_menu_root,
        "equal-z detached overlay roots did not preserve retained author order"
    );
    const ui::WidgetSubtarget* dismiss = target(
        toast_targets,
        "$toast/" + std::to_string(long_toast) + "/dismiss"
    );
    check(dismiss != nullptr, "long toast dismiss target was not projected");
    const ui::Point dismiss_point{
        dismiss->bounds.x + dismiss->bounds.width * 0.5,
        dismiss->bounds.y + dismiss->bounds.height * 0.5,
    };
    static_cast<void>(surface.input().enqueue(std::vector<ui::SurfaceInputEvent>{
        ui::PointerInputEvent{dismiss_point, ui::PointerEventType::press, 13, 0},
        ui::PointerInputEvent{dismiss_point, ui::PointerEventType::release, 13, 0},
    }));
    static_cast<void>(surface.input().process_queued());
    check(
        surface.notifications().find(long_toast) == nullptr,
        "topmost toast dismiss input disagreed with detached paint order"
    );
    check(
        !surface.input().focused(palette->identity()),
        "toast interaction did not dismiss the modal palette"
    );
    check(
        surface.input().focused(context->identity()),
        "toast interaction did not restore the underlying context-menu focus"
    );

    const data::JsonValue* menu_semantics = surface.semantics().find(menu->identity());
    const data::JsonValue* file = menu_semantics != nullptr
        ? child_named(*menu_semantics, "File") : nullptr;
    const data::JsonValue* more = file != nullptr ? child_named(*file, "More") : nullptr;
    const data::JsonValue* save = file != nullptr ? child_named(*file, "Save workspace") : nullptr;
    const data::JsonValue* save_state = save != nullptr ? save->find("state") : nullptr;
    const data::JsonValue* save_checked = save_state != nullptr ? save_state->find("checked") : nullptr;
    check(
        file != nullptr && save != nullptr && save_checked != nullptr &&
            save_checked->boolean() != nullptr && *save_checked->boolean() &&
            more != nullptr && child_named(*more, "Plain action") != nullptr,
        "recursive menu semantics lost its hierarchy or check state"
    );
    static_cast<void>(surface.frame(102'000'000));
    toast_semantics = surface.semantics().find(toasts->identity());
    check(
        toast_semantics == nullptr || child_named(*toast_semantics, long_message) == nullptr,
        "dismissed notification remained in semantics after the next frame"
    );
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 2) throw std::invalid_argument("expected registry path");
        const std::filesystem::path registry_path(arguments[1]);
        const auto bundle = load_bundle(registry_path);
        test_notification_queue();
        test_surface_shell_lifecycle(bundle, registry_path);
        std::cout << "strata_shell_lifecycle_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_shell_lifecycle_tests: " << error.what() << '\n';
        return 1;
    }
}
