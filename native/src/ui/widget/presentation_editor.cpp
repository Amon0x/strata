#include "ui/widget/presentation_editor.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "ui/input.hpp"
#include "ui/text.hpp"
#include "ui/text_geometry.hpp"
#include "ui/widget/presentation.hpp"

namespace strata::ui {

void present_editable_text(
    WidgetRenderScope& scope,
    EditableTextPresentation presentation
) {
    if (scope.text_engine() == nullptr || presentation.viewport.empty()) return;
    const bool focused = scope.input().focused(scope.node().identity());
    const std::optional<TextEditorSnapshot> editor =
        scope.input().editor_snapshot(scope.node().identity());
    std::string value = editor.has_value()
        ? std::string(editor->text)
        : std::move(presentation.fallback);
    if (editor.has_value() && editor->preedit.has_value() && !editor->preedit->empty()) {
        value.insert(std::min(editor->caret, value.size()), *editor->preedit);
    }
    const bool placeholder = value.empty() && !presentation.placeholder.empty() &&
        (presentation.placeholder_when_focused || !focused);
    if (placeholder) value = std::move(presentation.placeholder);

    const TextLayout text_layout = scope.text_engine()->layout(scope.node(), value);
    const Point origin = text_input_origin(
        presentation.viewport, text_layout, presentation.multiline
    );
    scope.push_clip(presentation.viewport);
    if (editor.has_value() && !placeholder &&
        editor->selection_start != editor->selection_end) {
        const std::size_t start = utf16_offset_for_utf8_byte(
            editor->text, editor->selection_start
        );
        const std::size_t end = utf16_offset_for_utf8_byte(
            editor->text, editor->selection_end
        );
        for (const Rect rect : text_layout_selection_rects(text_layout, origin, start, end)) {
            scope.solid_rect(rect, scope.visual().selection);
        }
    }

    std::optional<std::pair<std::size_t, std::size_t>> composition_range;
    if (editor.has_value() && editor->preedit.has_value() && !editor->preedit->empty()) {
        const std::size_t composition_start = utf16_offset_for_utf8_byte(
            editor->text, editor->caret
        );
        const std::size_t composition_end = composition_start + utf16_offset_for_utf8_byte(
            *editor->preedit, editor->preedit->size()
        );
        composition_range = std::pair(composition_start, composition_end);
        const std::size_t selection_start = composition_start + utf16_offset_for_utf8_byte(
            *editor->preedit, editor->preedit_selection_start
        );
        const std::size_t selection_end = composition_start + utf16_offset_for_utf8_byte(
            *editor->preedit, editor->preedit_selection_end
        );
        for (const Rect rect : text_layout_selection_rects(
                 text_layout, origin, selection_start, selection_end
             )) {
            scope.solid_rect(rect, RenderColor{255U, 255U, 255U, 72U});
        }
    }
    if (!value.empty()) {
        scope.text(
            value,
            origin,
            placeholder ? scope.visual().text_hint : scope.visual().foreground
        );
    }
    if (composition_range.has_value()) {
        for (Rect rect : text_layout_selection_rects(
                 text_layout, origin, composition_range->first, composition_range->second
             )) {
            rect.y += std::max(0.0, rect.height - 1.0);
            rect.height = 1.0;
            scope.solid_rect(rect, RenderColor{255U, 255U, 255U, 160U});
        }
    }
    if (editor.has_value() && focused && !placeholder && !text_layout.lines.empty()) {
        scope.solid_rect(
            text_layout_caret_rect(text_layout, origin, editor->text, editor->caret),
            scope.visual().caret
        );
    }
    scope.pop_clip();
}

} // namespace strata::ui
