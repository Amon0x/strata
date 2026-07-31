#pragma once

#include <string>

#include "ui/layout.hpp"

namespace strata::ui {

class WidgetRenderScope;

struct EditableTextPresentation final {
    Rect viewport;
    std::string fallback;
    std::string placeholder;
    bool multiline = false;
    bool placeholder_when_focused = true;
};

/** Shared selection, preedit, caret, clipping, and text projection for editable composites. */
void present_editable_text(
    WidgetRenderScope& scope,
    EditableTextPresentation presentation
);

} // namespace strata::ui
