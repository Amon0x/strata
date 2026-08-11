#include "showcase.hpp"

#include <cstdint>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using strata::desktop::ShowcaseModel;
using strata::host::ActionEvent;
using strata::host::Value;

void check(const bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] Value drag(const std::string& payload_type, const std::string& payload,
                         const std::string& target, const std::string& placement, Value before = {},
                         Value after = {}) {
    return Value::object({
        {"phase", "drop"},
        {"payload", Value::object({{"type", payload_type}, {"value", payload}})},
        {"targetKey", target},
        {"operation", "move"},
        {"placement", placement},
        {"beforeKey", std::move(before)},
        {"afterKey", std::move(after)},
        {"insertionIndex", 0},
    });
}

[[nodiscard]] const Value& field(const Value& root, const std::string_view first,
                                 const std::string_view second) {
    return root.require(first).require(second);
}

void cards_reorder_through_typed_action() {
    ShowcaseModel model("tests");
    static_cast<void>(model.handle(ActionEvent{
        "demo.reorder",
        Value::object({}),
        "drag",
        std::string("manipulate.reorder.alpha"),
        drag("demo.reorder", "alpha", "manipulate.reorder.container", "after", {},
             "manipulate.reorder.delta"),
    }));
    const Value snapshot = model.demo_snapshot();
    const Value& items = field(snapshot, "demo", "reorderItems");
    check(items.array() != nullptr && items.array()->size() == 4U &&
              items.array()->back().require_string("id") == "alpha",
          "showcase card reorder did not update the watched typed model");
}

void tree_reparents_without_json_plumbing() {
    ShowcaseModel model("tests");
    static_cast<void>(model.handle(ActionEvent{
        "demo.data.collection",
        Value::object({}),
        "drag",
        std::string("data.tree.folder.0"),
        drag("strata.tree-row", "data.tree.folder.1", "data.tree.folder.0", "on"),
    }));
    const Value snapshot = model.tree_snapshot();
    const Value& items = field(snapshot, "data", "treeItems");
    const auto moved = std::ranges::find_if(*items.array(), [](const Value& item) {
        return item.require_string("key") == "data.tree.folder.1";
    });
    check(moved != items.array()->end() &&
              moved->require_string("parentKey") == "data.tree.folder.0",
          "showcase tree drop did not reparent its typed node");
}

void grid_reorders_at_the_target_callback_only() {
    ShowcaseModel model("tests");
    static_cast<void>(model.handle(ActionEvent{
        "demo.data.collection",
        Value::object({}),
        "drag",
        std::string("data.grid.item.1"),
        drag("demo.data-grid-item", "data.grid.item.0", "data.grid.item.1", "after"),
    }));
    const Value snapshot = model.grid_snapshot();
    const Value& entries = field(snapshot, "data", "gridEntries");
    check(entries.array() != nullptr && entries.array()->size() > 2U &&
              (*entries.array())[1].require_string("key") == "data.grid.item.1" &&
              (*entries.array())[2].require_string("key") == "data.grid.item.0",
          "showcase grid drop did not commit its typed item order");
}

void surface_visibility_preserves_the_model() {
    ShowcaseModel model("tests");
    const Value initial = model.demo_snapshot();
    const bool* const initially_visible = field(initial, "demo", "surfaceVisible").boolean();
    check(
        initially_visible != nullptr && *initially_visible,
        "showcase must begin presented"
    );
    const std::uint64_t before = model.demo_revision().value();
    model.surface_visible(false);
    const Value hidden = model.demo_snapshot();
    const bool* const currently_visible = field(hidden, "demo", "surfaceVisible").boolean();
    check(
        currently_visible != nullptr && !*currently_visible &&
            model.demo_revision().value() > before,
        "showcase visibility did not publish through the retained typed model"
    );
}

} // namespace

int strata_test_showcase_model() {
    try {
        cards_reorder_through_typed_action();
        tree_reparents_without_json_plumbing();
        grid_reorders_at_the_target_callback_only();
        surface_visibility_preserves_the_model();
        std::cout << "strata_showcase_model_tests: typed showcase mutations OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_showcase_model_tests: " << error.what() << '\n';
        return 1;
    }
}
