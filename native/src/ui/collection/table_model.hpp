#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.hpp"
#include "ui/layout.hpp"

namespace strata::ui::collection {

struct TableColumn final {
    const runtime::Value* source = nullptr;
    std::string id;
    std::string header;
    double width = 0.0;
    double minimum = 0.0;
    double maximum = 0.0;
    bool pinned = false;
    bool resizable = true;
};

struct TableTrack final {
    TableColumn column;
    double start = 0.0;
    double visible_start = 0.0;
    double visible_end = 0.0;

    [[nodiscard]] double end() const noexcept;
    [[nodiscard]] bool visible_contains(double x) const noexcept;
};

struct TableGeometry final {
    Rect viewport;
    double pinned_end = 0.0;
    std::vector<TableTrack> tracks;

    [[nodiscard]] const TableTrack* track(std::string_view id) const noexcept;
    [[nodiscard]] const TableTrack* column_at(double x) const noexcept;
    [[nodiscard]] const TableTrack* resize_at(double x, double hit_width = 5.0) const noexcept;
};

[[nodiscard]] std::vector<TableColumn> resolve_table_columns(
    const runtime::Value* columns,
    const runtime::Value* controlled_widths,
    const runtime::Value* retained_widths,
    const runtime::Value* default_widths,
    const runtime::Value* controlled_order,
    const runtime::Value* retained_order,
    const runtime::Value* default_order,
    double viewport_width
);

[[nodiscard]] TableGeometry table_geometry(
    std::vector<TableColumn> columns,
    Rect viewport,
    double scroll_x
);

[[nodiscard]] runtime::Value encode_widths(const std::vector<TableColumn>& columns);
[[nodiscard]] runtime::Value encode_order(const std::vector<TableColumn>& columns);

} // namespace strata::ui::collection
