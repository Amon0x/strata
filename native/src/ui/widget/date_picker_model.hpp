#pragma once

#include <optional>
#include <string>

#include "ui/layout.hpp"
#include "ui/widget/calendar_model.hpp"

namespace strata::ui {

class RetainedNode;
class WidgetRegistry;

/** The dropdown a DatePicker opens: its surface, and the month laid out inside it. */
struct DatePickerPopup final {
    Rect surface;
    Rect month;
};

/** Where a DatePicker's dropdown opens beside its field `anchor`, kept inside `viewport`. */
[[nodiscard]] DatePickerPopup date_picker_popup(Rect anchor, Rect viewport);

/** The picked day as the field shows it: in `format` when given, else like "Mar 15, 2024". */
[[nodiscard]] std::optional<std::string> date_picker_text(const RetainedNode& node,
                                                          const CalendarState& state);

/** Registers DatePicker's description, input, semantics, subtarget and presentation phases. */
void register_date_picker_widget(WidgetRegistry& registry);

} // namespace strata::ui
