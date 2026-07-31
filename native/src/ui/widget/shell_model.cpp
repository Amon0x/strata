#include "ui/widget/shell_model.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "ui/command.hpp"
#include "ui/tree.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] const runtime::Value* property(
    const RetainedNode& node,
    const std::string_view name
) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.data_value() : nullptr;
}

[[nodiscard]] const std::string* text(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    return value->key() != nullptr ? &value->key()->value : nullptr;
}

[[nodiscard]] bool boolean(const runtime::Value* value, const bool fallback) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] double number(const runtime::Value* value, const double fallback) noexcept {
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

[[nodiscard]] std::size_t count(
    const runtime::Value* value,
    const std::size_t fallback
) noexcept {
    const double parsed = number(value, static_cast<double>(fallback));
    return static_cast<std::size_t>(std::clamp(
        parsed,
        1.0,
        4'096.0
    ));
}

[[nodiscard]] Rect root_bounds(const LayoutResult& layout, const Rect fallback) noexcept {
    const LayoutRecord* root = layout.find(layout.root_identity);
    return root != nullptr ? root->bounds : fallback;
}

[[nodiscard]] std::int64_t duration_nanos(
    const RetainedNode& node,
    const std::string_view name,
    const std::int64_t fallback
) noexcept {
    const runtime::Value* value = property(node, name);
    if (value != nullptr && value->duration() != nullptr) {
        return std::max<std::int64_t>(0, value->duration()->nanoseconds);
    }
    return fallback;
}

[[nodiscard]] std::vector<PaletteEntryModel> palette_entries(
    const RetainedNode& node,
    const CommandIndex* commands
) {
    std::vector<PaletteEntryModel> result;
    const runtime::Value* items = property(node, "items");
    if (items != nullptr && items->list() != nullptr && !items->list()->values.empty()) {
        result.reserve(items->list()->values.size());
        for (std::size_t index = 0U; index < items->list()->values.size(); ++index) {
            const runtime::Value& item = items->list()->values[index];
            if (!boolean(item.field("enabled"), true)) continue;
            const std::string* id = text(item.field("id"));
            if (id == nullptr || id->empty()) continue;
            const std::string* label = text(item.field("label"));
            const std::string* detail = text(item.field("detail"));
            result.push_back(PaletteEntryModel{
                *id,
                label != nullptr && !label->empty() ? *label : *id,
                detail != nullptr ? *detail : std::string{},
                detail != nullptr ? *detail : std::string{},
                {},
                index,
                std::numeric_limits<std::int64_t>::min(),
                0,
                {},
            });
        }
        return result;
    }
    if (commands == nullptr) return result;
    CommandReferenceProjection references = commands->reference_projection(node);
    const std::vector<const CommandSnapshot*>& selected = references.commands;
    result.reserve(selected.size());
    for (std::size_t index = 0U; index < selected.size(); ++index) {
        const CommandSnapshot& command = *selected[index];
        if (!command.enabled) continue;
        std::string detail = command.category;
        const std::string shortcut = format_command_shortcut(command);
        if (!shortcut.empty()) {
            if (!detail.empty()) detail += "  ·  ";
            detail += shortcut;
        }
        result.push_back(PaletteEntryModel{
            command.id,
            command.label,
            detail,
            command.category,
            command.id,
            index,
            commands->recent_rank(command.id),
            0,
            {},
        });
    }
    return result;
}

} // namespace

BannerProjection project_banner(const RetainedNode& node) noexcept {
    const bool active = !boolean(node.retained_value("$dismissed"), false);
    const auto found = node.description().properties.find("action");
    const bool has_action = found != node.description().properties.end() &&
                            found->second.action() != nullptr &&
                            *found->second.action() != nullptr &&
                            (*found->second.action())->action != nullptr;
    return BannerProjection{
        active,
        active && has_action,
        active && boolean(property(node, "dismissible"), false),
    };
}

std::optional<bool> tooltip_controlled_visible(const RetainedNode& node) noexcept {
    const runtime::Value* visible = property(node, "visible");
    return visible != nullptr && visible->boolean() != nullptr
        ? std::optional<bool>(*visible->boolean())
        : std::nullopt;
}

bool tooltip_disclosure_visible(const RetainedNode& node) noexcept {
    if (const std::optional<bool> controlled = tooltip_controlled_visible(node);
        controlled.has_value()) {
        return *controlled;
    }
    return boolean(node.retained_value(tooltip_shown_state), false);
}

std::int64_t tooltip_show_delay_nanos(const RetainedNode& node) noexcept {
    return duration_nanos(node, "showDelay", tooltip_default_show_delay_nanos);
}

std::int64_t tooltip_hide_delay_nanos(const RetainedNode& node) noexcept {
    return duration_nanos(node, "hideDelay", tooltip_default_hide_delay_nanos);
}

std::optional<TooltipProjection> project_tooltip(
    const RetainedNode& node,
    const LayoutResult& layout,
    const ShellTextLayoutResolver& text_layout
) {
    if (!tooltip_disclosure_visible(node) || !text_layout) return std::nullopt;
    const runtime::Value* content = property(node, "text");
    if (content == nullptr || content->string() == nullptr || content->string()->empty()) {
        return std::nullopt;
    }
    const LayoutRecord* record = layout.find(node.identity());
    if (record == nullptr) return std::nullopt;
    Rect anchor = record->bounds;
    if (!node.children().empty()) {
        if (const LayoutRecord* child = layout.find(node.children().front()->identity());
            child != nullptr) {
            anchor = child->bounds;
        }
    }
    const TextLayout resolved = text_layout(node, *content->string(), TextLayoutOptions{});
    const Rect viewport = root_bounds(layout, record->bounds);
    const double width = std::min(
        std::max(1.0, resolved.shaped.metrics.width + 16.0), viewport.width
    );
    const double height = std::min(
        std::max(1.0, resolved.shaped.metrics.height + 10.0), viewport.height
    );
    const double x = std::clamp(
        anchor.x + (anchor.width - width) * 0.5,
        viewport.x,
        std::max(viewport.x, viewport.right() - width)
    );
    double y = anchor.bottom() + 6.0;
    if (y + height > viewport.bottom()) y = anchor.y - 6.0 - height;
    const Rect bounds{
        x,
        std::clamp(y, viewport.y, std::max(viewport.y, viewport.bottom() - height)),
        width,
        height,
    };
    return TooltipProjection{
        anchor,
        bounds,
        Point{bounds.x + 8.0, bounds.y + 5.0},
        resolved,
    };
}

std::size_t PaletteProjection::visible_count() const noexcept {
    return std::min(max_visible_rows, matches.size() - std::min(window_start, matches.size()));
}

Rect PaletteProjection::row_bounds(const std::size_t local_index) const noexcept {
    return Rect{
        bounds.x + 8.0,
        bounds.y + 56.0 + row_height * static_cast<double>(local_index),
        std::max(0.0, bounds.width - 16.0),
        row_height,
    };
}

const PaletteEntryModel* PaletteProjection::active() const noexcept {
    return active_index < matches.size() ? &matches[active_index] : nullptr;
}

PaletteProjection project_command_palette(
    const RetainedNode& node,
    const LayoutResult& layout,
    const CommandIndex* commands,
    const std::string_view query
) {
    PaletteProjection result;
    const LayoutRecord* record = layout.find(node.identity());
    const Rect viewport = root_bounds(layout, record != nullptr ? record->bounds : Rect{});
    result.max_visible_rows = count(property(node, "maxVisibleRows"), 10U);
    result.row_height = std::max(18.0, number(property(node, "rowHeight"), 32.0));
    const double available_width = std::max(0.0, viewport.width - 24.0);
    const double preferred_width = std::clamp(viewport.width * 0.62, 360.0, 720.0);
    const double width = std::min(preferred_width, available_width);
    const double preferred_height = 64.0 + result.row_height *
        static_cast<double>(result.max_visible_rows);
    const double height = std::min(preferred_height, std::max(0.0, viewport.height - 24.0));
    result.bounds = Rect{
        viewport.x + (viewport.width - width) * 0.5,
        viewport.y + std::max(12.0, (viewport.height - height) * 0.25),
        width,
        height,
    };
    result.input_bounds = Rect{
        result.bounds.x + 12.0,
        result.bounds.y + 10.0,
        std::max(0.0, result.bounds.width - 24.0),
        38.0,
    };

    std::vector<PaletteEntryModel> entries = palette_entries(node, commands);
    std::vector<QuickPickCandidate> candidates;
    candidates.reserve(entries.size());
    for (const PaletteEntryModel& entry : entries) {
        candidates.push_back(QuickPickCandidate{
            entry.label,
            entry.searchable_detail,
            entry.source_index,
            entry.recent_rank,
        });
    }
    std::vector<QuickPickMatch> ranked = rank_quick_pick(query, candidates);
    result.matches.reserve(ranked.size());
    for (QuickPickMatch& match : ranked) {
        PaletteEntryModel entry = std::move(entries[match.candidate_index]);
        entry.score = match.score;
        entry.label_spans = std::move(match.label_spans);
        result.matches.push_back(std::move(entry));
    }
    const runtime::Value* active = node.retained_value("$paletteActive");
    if (!result.matches.empty()) {
        result.active_index = active != nullptr && active->number() != nullptr &&
            std::isfinite(*active->number()) && *active->number() >= 0.0 &&
            *active->number() <= static_cast<double>(result.matches.size() - 1U)
            ? static_cast<std::size_t>(*active->number())
            : 0U;
        const std::size_t maximum_start = result.matches.size() > result.max_visible_rows
            ? result.matches.size() - result.max_visible_rows
            : 0U;
        const std::size_t centered = result.active_index >= result.max_visible_rows
            ? result.active_index + 1U - result.max_visible_rows
            : 0U;
        result.window_start = std::min(centered, maximum_start);
    }
    return result;
}

ToastProjection project_toasts(
    const RetainedNode& node,
    const LayoutResult& layout,
    const NotificationService& notifications,
    const ToastTextLayoutResolver& text_layout
) {
    ToastProjection result;
    const LayoutRecord* record = layout.find(node.identity());
    const Rect viewport = root_bounds(layout, record != nullptr ? record->bounds : Rect{});
    const std::size_t max_visible = count(property(node, "maxVisible"), 3U);
    const NotificationSnapshot snapshot = notifications.snapshot(max_visible);
    result.overflow_count = snapshot.overflow_count;
    if (snapshot.visible.empty()) return result;
    const double width = std::min(
        std::max(120.0, number(property(node, "width"), 320.0)),
        std::max(0.0, viewport.width - 32.0)
    );
    const double gap = std::max(0.0, number(property(node, "gap"), 8.0));
    const double minimum_height = std::max(32.0, number(property(node, "minHeight"), 68.0));
    const std::size_t maximum_lines = count(property(node, "maxMessageLines"), 3U);
    const double message_width = std::max(1.0, width - 58.0);
    struct CardDraft final {
        TextLayoutOptions message_options;
        TextLayout message_layout;
        TextLayoutOptions action_options;
        TextLayout action_layout;
        double action_height = 0.0;
        double height = 0.0;
    };
    std::vector<CardDraft> drafts;
    drafts.reserve(snapshot.visible.size());
    for (const Notification& notification : snapshot.visible) {
        CardDraft draft;
        draft.message_options = TextLayoutOptions{
            message_width,
            std::string("WORD"),
            std::string("ELLIPSIS"),
            maximum_lines,
            std::string("START"),
        };
        if (text_layout) {
            draft.message_layout = text_layout(
                node, notification.request.message, draft.message_options
            );
        }
        if (notification.request.action != nullptr) {
            draft.action_options = TextLayoutOptions{
                std::max(1.0, width - 24.0),
                std::string("NONE"),
                std::string("ELLIPSIS"),
                1U,
                std::string("END"),
            };
            if (text_layout) {
                draft.action_layout = text_layout(
                    node, notification.request.action_label, draft.action_options
                );
            }
            draft.action_height = std::max(
                24.0,
                draft.action_layout.shaped.metrics.height + 6.0
            );
        }
        draft.height = std::max(
            minimum_height,
            20.0 + draft.message_layout.shaped.metrics.height + draft.action_height
        );
        drafts.push_back(std::move(draft));
    }
    result.cards.resize(snapshot.visible.size());
    double bottom = viewport.bottom() - 16.0;
    for (std::size_t reverse = snapshot.visible.size(); reverse > 0U; --reverse) {
        const std::size_t index = reverse - 1U;
        CardDraft& draft = drafts[index];
        const Rect bounds{
            viewport.right() - width - 16.0,
            bottom - draft.height,
            width,
            draft.height,
        };
        const Rect message_bounds{
            bounds.x + 16.0,
            bounds.y + 10.0,
            message_width,
            std::max(1.0, bounds.height - 20.0 - draft.action_height),
        };
        const Point message_origin{
            message_bounds.x,
            message_bounds.y + std::max(
                0.0,
                (message_bounds.height - draft.message_layout.shaped.metrics.height) * 0.5
            ),
        };
        const Rect action_bounds = snapshot.visible[index].request.action != nullptr
            ? Rect{
                  bounds.x + 12.0,
                  bounds.bottom() - draft.action_height - 4.0,
                  std::max(0.0, bounds.width - 24.0),
                  draft.action_height,
              }
            : Rect{};
        const Point action_origin{
            action_bounds.x,
            action_bounds.y + std::max(
                0.0,
                (action_bounds.height - draft.action_layout.shaped.metrics.height) * 0.5
            ),
        };
        result.cards[index] = ToastCardModel{
            snapshot.visible[index],
            bounds,
            message_bounds,
            message_origin,
            std::move(draft.message_options),
            std::move(draft.message_layout),
            Rect{bounds.right() - 36.0, bounds.y, 36.0, 34.0},
            action_bounds,
            action_origin,
            std::move(draft.action_options),
            std::move(draft.action_layout),
        };
        bottom = bounds.y - gap;
    }
    return result;
}

} // namespace strata::ui
