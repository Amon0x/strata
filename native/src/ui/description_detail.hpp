#pragma once

// Shared by the description builder's translation units; not part of its interface.

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ui/description.hpp"

namespace strata::ui {

/**
 * What one generator's rows are described from: the enclosing body's scope and either a Repeater
 * generation and its row block, or a widget's row factory. Providers hold it; rows realized after
 * the builder is gone are described by a builder of their own.
 */
struct DescriptionBuilder::RowSource final {
    enum class Kind : std::uint8_t { repeater, widget };

    Kind kind = Kind::repeater;
    std::shared_ptr<DescriptionBuilder*> builder;
    InstanceId generator = 0U;
    runtime::ApplicationContext* application = nullptr;
    std::shared_ptr<const WidgetRegistry> widgets;
    std::shared_ptr<const runtime::RuntimeUnit> unit;
    /** The retained values a builder of the rows' own reads. */
    std::shared_ptr<const RetainedDescriptionSnapshot> retained;
    Scope scope;
    std::size_t count = 0U;
    // A Repeater's rows.
    std::shared_ptr<const runtime::IndexableSequence> sequence;
    runtime::Symbol item;
    std::optional<runtime::Symbol> index;
    runtime::BlockId block = runtime::no_program_id;
    // A widget's rows.
    WidgetGeneratedChildHook factory;
};

namespace description_detail {

/**
 * An authored identity as one instance path segment. Paths are walked segment by segment ('/')
 * and loop items are told apart by a ':' suffix, so neither may appear unescaped.
 */
[[nodiscard]] std::string path_segment(std::string_view identity);

/** A loop's item (and index) over the scope it repeats in. */
[[nodiscard]] runtime::ScopeFrame item_frame(runtime::Symbol item, const runtime::Value& value,
                                             std::optional<runtime::Symbol> index,
                                             std::size_t position);

/** The slots a component's output declares itself (see own_slot_name). */
void declared_slot_names(const std::shared_ptr<const DescriptionNode>& root,
                         std::string_view component_path,
                         std::set<std::string, std::less<>>& names);

/** A component's output with caller content projected into the slots it declares itself. */
[[nodiscard]] std::shared_ptr<const DescriptionNode> project_slot_content(
    const std::shared_ptr<const DescriptionNode>& root, std::string_view component_path,
    const std::map<std::string, std::vector<std::shared_ptr<const DescriptionNode>>, std::less<>>&
        projected);

} // namespace description_detail

} // namespace strata::ui
