#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui/tree.hpp"

namespace strata::ui {

class WidgetRegistry;
class WidgetDescriptionScope;

/**
 * Source text as a CodeView shows it: line endings normalized to "\n", tabs expanded to the next
 * multiple of `tab_size` columns, and one final newline dropped (a file's last line ends there,
 * it does not start another). `lines` are the byte ranges of each line, without its newline.
 */
struct CodeViewSource final {
    std::string text;
    std::vector<std::pair<std::size_t, std::size_t>> lines;
};

[[nodiscard]] CodeViewSource code_view_source(std::string_view text, std::size_t tab_size);

/**
 * What a CodeView described by `properties` (its text, language and other arguments, and its
 * style, layout and text style) shows, as its own describe step expands it. Widgets that generate
 * a CodeView (Markdown's fenced code) build it with this, since generated nodes are not described
 * again. An expansion is kept and reused while nothing it depends on changes, so a parent
 * re-described every second does not tokenize its code again.
 */
[[nodiscard]] std::vector<std::shared_ptr<const DescriptionNode>>
expand_code_view(WidgetDescriptionScope& scope, const std::string& key,
                 const DescriptionNode::Properties& properties);

/** Registers CodeView's description, presentation and semantics phases. */
void register_code_view_widget(WidgetRegistry& registry);

} // namespace strata::ui
