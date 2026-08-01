#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <strata/contracts/demo_surface.hpp>
#include <strata/host.hpp>

namespace strata::desktop {

/** Typed application model for the bundled showcase; independent from Win32 and rendering. */
class ShowcaseModel final {
  public:
    explicit ShowcaseModel(std::string instance_label);

    [[nodiscard]] const strata::host::Revision& demo_revision() const noexcept;
    [[nodiscard]] const strata::host::Revision& tree_revision() const noexcept;
    [[nodiscard]] const strata::host::Revision& table_revision() const noexcept;
    [[nodiscard]] const strata::host::Revision& grid_revision() const noexcept;

    [[nodiscard]] strata::host::Value demo_snapshot() const;
    [[nodiscard]] strata::host::Value tree_snapshot() const;
    [[nodiscard]] strata::host::Value table_snapshot() const;
    [[nodiscard]] strata::host::Value grid_snapshot() const;
    [[nodiscard]] std::vector<contracts::demo_surface::DataTreeItemsItem> tree_items() const;

    [[nodiscard]] strata::host::ActionResult handle(const strata::host::ActionEvent& event);
    [[nodiscard]] bool load_tree_children(std::string_view key);
    void data_activity(std::string value);

  private:
    struct TreeItem final {
        std::string key;
        std::string label;
        std::optional<std::string> parent;
        std::optional<std::size_t> folder_group;
        bool folder = false;

        [[nodiscard]] bool operator==(const TreeItem&) const = default;
    };

    struct GridLocation final {
        std::size_t group = 0U;
        std::size_t index = 0U;
    };

    void initialize_tree();
    void initialize_grid();
    [[nodiscard]] bool handle_reorder(const strata::host::DragEvent& event);
    [[nodiscard]] bool handle_tree_drop(const strata::host::DragEvent& event);
    [[nodiscard]] bool handle_grid_drop(const strata::host::ActionEvent& action,
                                        const strata::host::DragEvent& event);
    [[nodiscard]] bool move_tree_item(std::string_view source_key, std::string_view target_key,
                                      strata::host::DropPlacement placement);
    [[nodiscard]] bool tree_descends_from(std::string key, std::string_view ancestor) const;
    [[nodiscard]] std::optional<GridLocation> grid_location(std::size_t item) const noexcept;
    [[nodiscard]] static std::optional<std::size_t> indexed_key(std::string_view key,
                                                                std::string_view prefix) noexcept;
    [[nodiscard]] static std::string reorder_key(std::string_view id);
    [[nodiscard]] static std::string collection_activity(const strata::host::ActionEvent& event);
    void changed_demo();

    std::string instance_label_;
    std::vector<std::string> reorder_items_{"alpha", "beta", "gamma", "delta"};
    std::vector<TreeItem> tree_items_;
    std::set<std::size_t> loaded_tree_groups_;
    std::vector<std::vector<std::size_t>> grid_groups_;
    std::string data_activity_ = "none yet";
    std::string host_value_ = "desktop-v0";
    std::string host_message_;
    std::string combo_query_;
    std::optional<std::string> combo_selection_;
    double controlled_split_ratio_ = 0.35;
    std::uint64_t host_generation_ = 0U;
    strata::host::Revision demo_revision_;
    strata::host::Revision tree_revision_;
    strata::host::Revision table_revision_;
    strata::host::Revision grid_revision_;
};

} // namespace strata::desktop
