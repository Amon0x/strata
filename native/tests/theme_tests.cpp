#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/source.hpp"
#include "core/utf8.hpp"
#include "data/json.hpp"
#include "runtime/application.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/layout.hpp"
#include "ui/layout/detail_algorithms.hpp"
#include "ui/surface.hpp"
#include "ui/theme.hpp"
#include "ui/tree.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

[[nodiscard]] std::shared_ptr<const strata::ui::DescriptionChildren>
eager(std::vector<std::shared_ptr<const strata::ui::DescriptionNode>> children) {
    return std::make_shared<const strata::ui::EagerDescriptionChildren>(std::move(children));
}

[[nodiscard]] std::shared_ptr<const strata::ui::DescriptionNode>
node(std::string type, std::string key, strata::ui::DescriptionNode::Properties properties = {},
     std::vector<std::shared_ptr<const strata::ui::DescriptionNode>> children = {}) {
    return strata::ui::DescriptionNode::create(std::move(type), std::move(key), "/theme-test",
                                               "screen Theme", std::move(properties),
                                               eager(std::move(children)));
}

[[nodiscard]] strata::ui::DescriptionNode::Properties
properties(std::optional<std::string> theme,
           std::vector<std::pair<std::string, strata::runtime::Value>> style = {},
           std::optional<std::string> variant = std::nullopt) {
    strata::ui::DescriptionNode::Properties result;
    if (theme.has_value()) {
        result.emplace("theme", strata::runtime::ExpressionValue(strata::runtime::Value(*theme)));
    }
    if (!style.empty()) {
        result.emplace("$layout",
                       strata::runtime::ExpressionValue(strata::runtime::Value(std::move(style))));
    }
    if (variant.has_value()) {
        result.emplace("variant",
                       strata::runtime::ExpressionValue(strata::runtime::Value(*variant)));
    }
    return result;
}

[[nodiscard]] const strata::runtime::Value* field(const strata::ui::DescriptionNode& node,
                                                  const std::string_view name) {
    const auto layout = node.properties.find("$layout");
    return layout != node.properties.end() && layout->second.value() != nullptr
               ? layout->second.value()->field(name)
               : nullptr;
}

void test_defaults_and_validation() {
    using namespace strata;
    ui::ThemeTokens tokens;
    check(tokens.surface == runtime::ColorValue{34U, 38U, 46U, 220U},
          "default surface token changed");
    check(tokens.surface_raised == runtime::ColorValue{24U, 24U, 42U, 240U},
          "default raised token changed");
    check(tokens.foreground == runtime::ColorValue{236U, 240U, 244U, 255U},
          "default foreground token changed");
    check(tokens.spacing_unit == 4.0 && tokens.radius == 4.0 && tokens.density == 1.0,
          "default numeric tokens changed");
    bool rejected = false;
    try {
        tokens.density = 0.0;
        tokens.validate();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "non-positive theme density was accepted");

    const ui::Theme default_theme;
    const ui::ThemedWidgetStyle text = default_theme.style("Text", "subtle");
    check(text.visual.has_value() && !text.visual->background.has_value(),
          "text semantic chrome is not transparent");
    check(text.text_layout.has_value() && text.text_layout->primary_font == "strata:fonts/default",
          "ordinary text no longer defaults to the Regular face");
    const ui::ThemedWidgetStyle raised = default_theme.style("Tooltip");
    check(raised.visual.has_value() && raised.visual->background.has_value() &&
              *raised.visual->background == default_theme.tokens().surface_raised,
          "raised widget semantic background changed");
    const ui::ThemedWidgetStyle compact = default_theme.style("Button", "compact");
    check(compact.visual->radius == 3.0 && compact.text_layout->pixel_size == 10.8 &&
              compact.text_layout->primary_font == "strata:fonts/default-medium",
          "compact control typography or theme semantics changed");
}

void test_scopes_styles_tokens_and_lazy_ranges() {
    using namespace strata;
    ui::ThemeWidgetVisualStyle inherited_visual;
    inherited_visual.radius = 9.0;
    inherited_visual.opacity = 0.75;
    std::map<ui::ThemeWidgetKey, ui::ThemedWidgetStyle> parent_styles;
    parent_styles.emplace(ui::ThemeWidgetKey{"Button", "primary"}, ui::ThemedWidgetStyle{
                                                                       inherited_visual,
                                                                       std::nullopt,
                                                                       std::nullopt,
                                                                       std::nullopt,
                                                                       std::nullopt,
                                                                   });
    auto parent = std::make_shared<const ui::Theme>("parent", ui::ThemeTokens{}, nullptr,
                                                    std::nullopt, std::move(parent_styles));
    ui::ThemeTokens child_tokens;
    child_tokens.accent = runtime::ColorValue{7U, 8U, 9U, 255U};
    child_tokens.danger = runtime::ColorValue{201U, 2U, 3U, 255U};
    child_tokens.spacing_unit = 11.0;
    child_tokens.density = 1.5;
    ui::Theme child("child", child_tokens, parent);

    ui::ThemeCatalog catalog;
    check(catalog.register_theme(*parent), "parent theme did not register");
    check(catalog.register_theme(child), "child theme did not register");

    auto button_properties =
        properties(std::nullopt,
                   {
                       {"background", runtime::Value(runtime::ColorValue{80U, 81U, 82U, 255U})},
                       {"color", runtime::Value(runtime::ThemeTokenValue{"danger"})},
                       {"padding", runtime::Value(runtime::ThemeTokenValue{"spacingUnit"})},
                   },
                   "primary");
    auto scoped = node(
        "ThemeScope", "scope",
        properties("child", {{"padding", runtime::Value(runtime::ThemeTokenValue{"spacingUnit"})}}),
        {
            node("Button", "button", std::move(button_properties)),
            node("Text", "text", properties(std::nullopt, {}, "primary")),
            node("ThemeScope", "unknown", properties("not-registered"),
                 {node("Text", "fallback", properties(std::nullopt, {}, "primary"))}),
        });
    const ui::ThemeMaterializationResult resolved = ui::materialize_theme_tree(
        scoped, catalog, [](const std::string_view type) { return type != "ThemeScope"; });
    check(field(*resolved.root, "padding") != nullptr &&
              *field(*resolved.root, "padding")->number() == 11.0,
          "structural theme scope did not resolve its symbolic layout token");
    const ui::DescriptionNode& button = *resolved.root->children->at(0U);
    check(field(button, "radius") != nullptr && *field(button, "radius")->number() == 9.0,
          "parent explicit type/variant style did not inherit");
    check(field(button, "opacity") != nullptr && *field(button, "opacity")->number() == 0.75,
          "explicit themed opacity did not materialize");
    check(field(button, "background") != nullptr &&
              *field(button, "background")->color() == runtime::ColorValue{80U, 81U, 82U, 255U},
          "authored visual did not override the theme");
    check(field(button, "foreground") != nullptr &&
              *field(button, "foreground")->color() == child_tokens.danger,
          "symbolic color token did not update visual/text foreground");
    check(field(button, "padding") != nullptr && *field(button, "padding")->number() == 11.0,
          "symbolic layout token did not resolve");
    const ui::DescriptionNode& text = *resolved.root->children->at(1U);
    check(field(text, "foreground") != nullptr &&
              *field(text, "foreground")->color() == child_tokens.accent,
          "effective child tokens were not used for semantic style");
    check(field(text, "pixelSize") != nullptr && *field(text, "pixelSize")->number() == 18.0,
          "theme density did not materialize into default text size");
    const ui::DescriptionNode& fallback = *resolved.root->children->at(2U)->children->at(0U);
    check(field(fallback, "foreground") != nullptr &&
              *field(fallback, "foreground")->color() == child_tokens.accent,
          "unknown named scope did not fall back to its inherited theme");

    const ui::ThemeMaterializationResult entering = ui::materialize_theme_subtree(
        node("Text", "entering",
             properties(std::nullopt,
                        {{"color", runtime::Value(runtime::ThemeTokenValue{"accent"})}},
                        "primary")),
        catalog, resolved.root->projected_theme, resolved.root->projected_theme_scope,
        [](const std::string_view type) { return type != "ThemeScope"; });
    check(entering.root != nullptr &&
              entering.root->projected_theme == resolved.root->projected_theme &&
              entering.root->projected_theme_generation == catalog.generation() &&
              field(*entering.root, "foreground") != nullptr &&
              field(*entering.root, "foreground")->color() != nullptr &&
              *field(*entering.root, "foreground")->color() == child_tokens.accent,
          "entering virtual subtree did not inherit its retained collection theme context");

    ui::ThemeMaterializationCache materialization_cache;
    const ui::ThemeMaterializationResult cached_first = ui::materialize_theme_tree(
        scoped, catalog, [](const std::string_view type) { return type != "ThemeScope"; }, {},
        &materialization_cache);
    const ui::ThemeMaterializationResult cached_second = ui::materialize_theme_tree(
        scoped, catalog, [](const std::string_view type) { return type != "ThemeScope"; }, {},
        &materialization_cache);
    check(cached_second.root == cached_first.root && cached_second.stats.resolved_nodes == 0U,
          "unchanged immutable theme input did not reuse its materialized subtree");

    std::size_t lazy_resolver_calls = 0U;
    auto lazy = std::make_shared<ui::DescriptionNode>(*node("Panel", "lazy"));
    lazy->children = std::make_shared<const ui::GeneratedDescriptionChildren>(
        100U, [&lazy_resolver_calls](const std::size_t index) {
            ++lazy_resolver_calls;
            return node("Text", "row-" + std::to_string(index));
        });
    lazy->materialization = ui::MaterializationRange{40U, 43U};
    const ui::ThemeMaterializationResult lazy_result =
        ui::materialize_theme_tree(lazy, catalog, [](const std::string_view) { return true; });
    check(lazy_resolver_calls == 3U && lazy_result.stats.resolved_nodes == 4U,
          "theme resolution requested an offscreen lazy child");
}

void test_catalog_and_motion_policy() {
    using namespace strata;
    ui::ThemeCatalog catalog;
    const std::uint64_t initial_generation = catalog.generation();
    ui::ThemeMotionPolicy policy(
        true,
        {
            {"fast",
             ui::MotionTiming{
                 9'000'000, 2'000'000, "cubic-out", {}, false, ui::MotionFillMode::both}},
            {"standard",
             ui::MotionTiming{17'000'000, 0, "cubic-in-out", {}, false, ui::MotionFillMode::both}},
        });
    ui::Theme motion_theme("motion", ui::ThemeTokens{}, nullptr, policy);
    check(catalog.register_theme(motion_theme) && catalog.generation() > initial_generation,
          "theme registration did not advance catalog generation");
    check(!catalog.register_theme(motion_theme), "identical theme registration invalidated twice");
    check(catalog.set_root(motion_theme), "theme root selection did not change");
    check(!catalog.unregister_theme("motion"), "active root theme was unregistered");

    auto animated_properties = properties(
        std::nullopt,
        {
            {"animateChanges", runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                                   {"policy", runtime::Value("missing")},
                                   {"properties", runtime::Value(std::vector<runtime::Value>{
                                                      runtime::Value("radius")})},
                               })},
            {"animateContentSize",
             runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                 {"policy", runtime::Value("fast")},
             })},
        });
    std::size_t unknown = 0U;
    const ui::ThemeMaterializationResult resolved = ui::materialize_theme_tree(
        node("Button", "animated", std::move(animated_properties)), catalog,
        [](const std::string_view) { return true; },
        [&unknown](const std::string_view name, const ui::DescriptionNode&) {
            check(name == "missing", "unknown motion callback reported the wrong name");
            ++unknown;
        });
    check(unknown == 1U, "unknown scoped motion timing was not diagnosed once per materialization");
    check(ui::theme_motion_reduced(*resolved.root, false),
          "scoped reduced-motion policy was not retained");
    check(ui::theme_motion_timing(*resolved.root, "fast").duration_nanos == 9'000'000,
          "named scoped motion timing was not retained");
    const std::optional<ui::ContentSizeMotionSpec> content =
        ui::content_size_motion(*resolved.root);
    check(content.has_value() && content->timing.duration_nanos == 9'000'000 &&
              content->timing.delay_nanos == 2'000'000 &&
              content->timing.easing == ui::MotionEasing(ui::MotionEasingKind::cubic_out),
          "content-size motion did not consume the full scoped timing");
    check(ui::theme_motion_timing(*resolved.root, "missing").duration_nanos == 17'000'000,
          "unknown scoped timing did not fall back to standard");
    check(ui::theme_motion_reduced(*resolved.root, true),
          "environment reduced-motion override was lost");
}

void test_exact_resolution_validation_and_local_scope() {
    using namespace strata;
    const auto radius_style = [](const double radius) {
        ui::ThemeWidgetVisualStyle visual;
        visual.radius = radius;
        return ui::ThemedWidgetStyle{
            visual, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
        };
    };
    auto parent =
        std::make_shared<const ui::Theme>("parent-case", ui::ThemeTokens{}, nullptr, std::nullopt,
                                          std::map<ui::ThemeWidgetKey, ui::ThemedWidgetStyle>{
                                              {{"Button", "Primary"}, radius_style(3.0)},
                                          });
    ui::Theme child("child-case", ui::ThemeTokens{}, parent, std::nullopt,
                    std::map<ui::ThemeWidgetKey, ui::ThemedWidgetStyle>{
                        {{"Button", "default"}, radius_style(2.0)},
                        {{"Button", "danger"}, radius_style(1.0)},
                    });
    check(child.style("Button", "danger").visual->radius == 1.0, "local exact variant did not win");
    check(child.style("Button", "Primary").visual->radius == 2.0,
          "local default did not precede parent explicit style");
    check(child.style("Button", "primary").visual->radius == 2.0,
          "variant resolution became case-insensitive");
    ui::Theme inherited_only("inherited-only", ui::ThemeTokens{}, parent);
    check(inherited_only.style("Button", "Primary").visual->radius == 3.0,
          "parent explicit variant did not inherit");

    const auto rejects = [](const auto& action) {
        try {
            action();
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    check(rejects([] { static_cast<void>(ui::Theme(" \t", ui::ThemeTokens{})); }),
          "whitespace-only theme name was accepted");
    check(rejects([] { ui::ThemeWidgetKey{"Button", " \n"}.validate(); }),
          "whitespace-only theme variant was accepted");
    check(rejects([] { static_cast<void>(ui::Theme{}.style("Button", " \t")); }),
          "whitespace-only resolved variant was accepted");
    check(rejects([] { static_cast<void>(runtime::Value(runtime::ThemeTokenValue{"  "})); }),
          "whitespace-only theme token was accepted");
    check(rejects([] { static_cast<void>(runtime::Value(runtime::KeyValue{" \t"})); }),
          "whitespace-only runtime key was accepted");
    check(rejects([] {
              static_cast<void>(runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                  {"  ", runtime::Value(1.0)}}));
          }),
          "whitespace-only style patch key was accepted");
    ui::ThemedWidgetStyle{}.validate();
    check(rejects([] {
              static_cast<void>(ui::ThemeMotionPolicy(
                  false, {
                             {"standard",
                              ui::MotionTiming{10, 0, " \t", {}, false, ui::MotionFillMode::both}},
                         }));
          }),
          "whitespace-only theme timing easing was accepted");

    ui::ThemeCatalog catalog;
    ui::ThemeTokens local_tokens;
    local_tokens.accent = runtime::ColorValue{4U, 5U, 6U, 255U};
    check(catalog.set_scoped_theme("local.scope", ui::Theme("local", local_tokens)),
          "typed local theme scope did not install");
    const ui::ThemeMaterializationResult local = ui::materialize_theme_tree(
        node("Panel", "local.scope", {},
             {node("Text", "local.text", properties(std::nullopt, {}, "primary"))}),
        catalog, [](const std::string_view type) { return type != "Panel"; });
    check(field(*local.root->children->at(0U), "foreground") != nullptr &&
              *field(*local.root->children->at(0U), "foreground")->color() == local_tokens.accent,
          "typed local scope did not inherit into descendants");
    check(catalog.clear_scoped_theme("local.scope"), "typed local theme scope did not clear");
}

class TestKeySequence final : public strata::runtime::KeyedSequence {
  public:
    explicit TestKeySequence(std::vector<std::string> keys) : keys_(std::move(keys)) {}
    [[nodiscard]] std::uint64_t generation() const noexcept override {
        return 1U;
    }
    [[nodiscard]] std::size_t count() const noexcept override {
        return keys_.size();
    }
    [[nodiscard]] std::string key_at(const std::size_t index) const override {
        return keys_.at(index);
    }
    [[nodiscard]] std::optional<std::size_t>
    index_of_key(const std::string_view key) const override {
        for (std::size_t index = 0U; index < keys_.size(); ++index) {
            if (keys_[index] == key)
                return index;
        }
        return std::nullopt;
    }
    [[nodiscard]] bool
    same_generation(const strata::runtime::KeyedSequence& other) const noexcept override {
        const auto* sequence = dynamic_cast<const TestKeySequence*>(&other);
        return sequence != nullptr && sequence->keys_ == keys_;
    }

  private:
    std::vector<std::string> keys_;
};

void test_full_style_round_trip_and_nullability() {
    using namespace strata;
    ui::ThemeWidgetVisualStyle visual;
    visual.background.reset();
    visual.foreground = runtime::ColorValue{1U, 2U, 3U, 4U};
    visual.border = ui::ThemeBorder{2.0, runtime::ColorValue{5U, 6U, 7U, 8U}, false};
    visual.focus_ring.reset();
    visual.indicator_size.reset();
    visual.track_height = 13.0;

    ui::ThemeTextLayoutStyle text;
    text.primary_font = "fixture:primary";
    text.fallback_fonts = {"fixture:fallback-a", "fixture:fallback-b"};
    text.pixel_size = 15.0;
    text.style_flags = 5U;
    text.line_height.reset();
    text.line_height_multiplier = 1.25;
    text.letter_spacing = 0.5;

    ui::LayoutStyle layout;
    layout.participates = false;
    layout.kind = ui::LayoutKind::scroll;
    layout.width = ui::LayoutSize::clamp(
        ui::LayoutSize(ui::LayoutSize::Kind::percent, 0.25),
        ui::LayoutSize::clamp(std::nullopt, ui::LayoutSize(ui::LayoutSize::Kind::content),
                              ui::LayoutSize(ui::LayoutSize::Kind::fixed, 160.0)),
        ui::LayoutSize(ui::LayoutSize::Kind::fixed, 180.0));
    layout.height = ui::LayoutSize{ui::LayoutSize::Kind::percent, 0.75};
    layout.min_width = ui::LayoutSize{ui::LayoutSize::Kind::content};
    layout.max_height = ui::LayoutSize{ui::LayoutSize::Kind::fixed, 240.0};
    layout.aspect_ratio = 1.5;
    layout.intrinsic_size = ui::Size{90.0, 60.0};
    layout.padding = {1.0, 2.0, 3.0, 4.0};
    layout.margin = {5.0, 6.0, 7.0, 8.0};
    layout.gap = {9.0, 10.0};
    layout.align_items = ui::LayoutAlign::stretch;
    layout.justify_content = ui::LayoutJustify::space_between;
    layout.align_content = ui::LayoutJustify::space_evenly;
    layout.align_self = ui::LayoutAlign::end;
    layout.justify_self = ui::LayoutAlign::center;
    layout.wrap = true;
    layout.clip = true;
    layout.z_index = 7;
    layout.grid_columns = {
        ui::LayoutSize{ui::LayoutSize::Kind::fixed, 30.0},
        ui::LayoutSize{ui::LayoutSize::Kind::fill, 2.0},
    };
    layout.grid_rows = {ui::LayoutSize{ui::LayoutSize::Kind::content}};
    layout.grid_column = 2U;
    layout.grid_row = 3U;
    layout.column_span = 2U;
    layout.row_span = 4U;
    layout.scroll_horizontal = true;
    layout.scroll_vertical = true;
    layout.scroll_viewport_insets = {11.0, 12.0, 13.0, 14.0};
    layout.scroll_content_padding = {15.0, 16.0, 17.0, 18.0};
    layout.scrollbar_gutter = 6.0;
    layout.scroll_offset = {19.0, 20.0};
    layout.pin_horizontal = true;
    layout.pin_vertical = true;
    layout.portal_target = "overlay";
    layout.detach_from_parent_clip = false;
    ui::VirtualListSpec virtual_list;
    virtual_list.axis = ui::LayoutAxis::horizontal;
    virtual_list.items =
        std::make_shared<const TestKeySequence>(std::vector<std::string>{"one", "two"});
    virtual_list.item_extent = 24.0;
    virtual_list.overscan = 3U;
    virtual_list.item_members = std::make_shared<const ui::VirtualItemMembers>(
        ui::VirtualItemMembers{{"one.child"}, {"two.child"}});
    virtual_list.item_extents.emplace(std::vector<double>{23.0, 25.0});
    virtual_list.measure_item_extents = true;
    layout.virtual_list = std::move(virtual_list);

    ui::ThemeAnimationSet motion;
    motion.declared_animations.push_back(ui::CompiledMotion{
        "fade",
        ui::MotionTrigger::hover,
        ui::MotionTiming{100'000'000, 5'000'000, "linear", {}, false, ui::MotionFillMode::both},
        {ui::MotionTrack{ui::MotionProperty::opacity,
                         {
                             ui::MotionKeyframe{0.0, 0.0, std::nullopt},
                             ui::MotionKeyframe{1.0, 1.0, std::nullopt},
                         }}},
    });
    motion.attachments.push_back(ui::ThemeMotionAttachment{
        ui::MotionTrigger::hover,
        "fade",
        true,
        ui::MotionDirection::forward,
        std::nullopt,
    });
    motion.value_channels.push_back(ui::ThemeMotionValueChannel{
        "indicator-target",
        ui::MotionProperty::indicator_position,
        0.5,
        "standard",
    });
    motion.resolved_properties =
        ui::ThemeResolvedPropertyMotion{{ui::MotionProperty::radius}, "fast"};
    motion.content_size = ui::ThemeContentSizeMotion{true, true, false, "standard"};

    ui::ThemeNullable<ui::ThemeAnimationSet> themed_motion;
    themed_motion.emplace(motion);
    ui::ThemedWidgetStyle style{visual, std::nullopt, text, layout, std::move(themed_motion)};
    ui::Theme theme("roundtrip", ui::ThemeTokens{}, nullptr, std::nullopt,
                    {{{"Panel", "ExactCase"}, style}});
    ui::ThemeCatalog catalog;
    check(catalog.register_theme(theme), "round-trip theme did not register");
    const ui::ThemeMaterializationResult materialized = ui::materialize_theme_tree(
        node("Panel", "roundtrip.node", properties("roundtrip", {}, "ExactCase")), catalog,
        [](const std::string_view) { return true; });
    check(field(*materialized.root, "background") != nullptr &&
              field(*materialized.root, "background")->kind() == runtime::ValueKind::null_value,
          "nullable visual background did not round-trip as explicit null");
    check(field(*materialized.root, "border") != nullptr &&
              field(*materialized.root, "border")->field("inside") != nullptr &&
              !*field(*materialized.root, "border")->field("inside")->boolean(),
          "border inside placement was lost");
    check(field(*materialized.root, "fallbackFonts") != nullptr &&
              field(*materialized.root, "fallbackFonts")->list()->values.size() == 2U &&
              *field(*materialized.root, "fontStyleFlags")->number() == 5.0 &&
              field(*materialized.root, "lineHeight")->kind() == runtime::ValueKind::null_value,
          "text fallback/style metadata was lost");
    const ui::LayoutStyle decoded = ui::layout_style(*materialized.root);
    check(!decoded.participates && decoded.kind == layout.kind && decoded.width == layout.width &&
              decoded.height == layout.height && decoded.min_width == layout.min_width &&
              decoded.max_height == layout.max_height &&
              decoded.aspect_ratio == layout.aspect_ratio &&
              decoded.intrinsic_size == layout.intrinsic_size &&
              decoded.padding == layout.padding && decoded.margin == layout.margin &&
              decoded.gap == layout.gap && decoded.align_items == layout.align_items &&
              decoded.justify_content == layout.justify_content &&
              decoded.align_content == layout.align_content &&
              decoded.align_self == layout.align_self &&
              decoded.justify_self == layout.justify_self && decoded.wrap == layout.wrap &&
              decoded.clip == layout.clip && decoded.z_index == layout.z_index &&
              decoded.grid_column == layout.grid_column && decoded.grid_row == layout.grid_row &&
              decoded.column_span == layout.column_span && decoded.row_span == layout.row_span &&
              decoded.grid_columns == layout.grid_columns &&
              decoded.grid_rows == layout.grid_rows &&
              decoded.scroll_viewport_insets == layout.scroll_viewport_insets &&
              decoded.scroll_content_padding == layout.scroll_content_padding &&
              decoded.scrollbar_gutter == layout.scrollbar_gutter && decoded.scroll_horizontal &&
              decoded.scroll_vertical && decoded.scroll_offset == layout.scroll_offset &&
              decoded.pin_horizontal && decoded.pin_vertical &&
              decoded.portal_target == layout.portal_target && !decoded.detach_from_parent_clip,
          "full themed layout did not survive typed materialization");
    check(ui::layout_detail::resolve_content_size(decoded.width, 200.0, 400.0, 0.0) == 160.0,
          "nested recursive clamp did not resolve its typed preferred/maximum branches");
    check(decoded.virtual_list.has_value() && decoded.virtual_list->item_count() == 2U &&
              decoded.virtual_list->items->key_at(1U) == "two" &&
              decoded.virtual_list->item_extents.has_value() &&
              decoded.virtual_list->item_extents->values() == std::vector<double>({23.0, 25.0}),
          "virtual-list layout metadata was lost");
    check(field(*materialized.root, "hover") != nullptr &&
              field(*materialized.root, "hover")->field("animation") != nullptr &&
              field(*materialized.root, "hover")
                  ->field("animation")
                  ->string()
                  ->starts_with("$theme|"),
          "theme-local declared animation was not qualified");

    ui::ThemedWidgetStyle disabled;
    disabled.motion.emplace(ui::ThemeAnimationSet{});
    disabled.validate();
    ui::Theme disabled_theme("disabled", ui::ThemeTokens{}, nullptr, std::nullopt,
                             {{{"Panel", "default"}, disabled}});
    const ui::ThemedWidgetStyle disabled_resolved = disabled_theme.style("Panel");
    check(disabled_resolved.motion.has_value() && disabled_resolved.motion->has_value() &&
              (**disabled_resolved.motion).attachments.empty(),
          "explicit empty animation set was not retained as motion disable");
    ui::ThemeCatalog disabled_catalog;
    check(disabled_catalog.register_theme(disabled_theme), "empty-motion theme did not register");
    const ui::ThemeMaterializationResult empty_materialized = ui::materialize_theme_tree(
        node("Panel", "empty",
             properties("disabled",
                        {
                            {"$contentSizeMotionDefaults",
                             runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                                 {"height", runtime::Value(true)},
                             })},
                            {"$disclosureDefaults",
                             runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                                 {"expanded", runtime::Value(true)},
                             })},
                        })),
        disabled_catalog, [](const std::string_view) { return true; });
    check(field(*empty_materialized.root, "$themeAnimationSet") != nullptr &&
              *field(*empty_materialized.root, "$themeAnimationSet")->string() == "present-empty" &&
              !ui::content_size_motion(*empty_materialized.root).has_value(),
          "present-empty motion collapsed into defaults or synthetic content-size motion");
    ui::DescriptionNode::Properties authored_motion = properties("disabled");
    authored_motion.emplace(
        "animateContentSize",
        runtime::ExpressionValue(runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"height", runtime::Value(true)},
            {"timing", runtime::Value("standard")},
        })));
    const ui::ThemeMaterializationResult authored_materialized =
        ui::materialize_theme_tree(node("Panel", "authored", std::move(authored_motion)),
                                   disabled_catalog, [](const std::string_view) { return true; });
    check(ui::content_size_motion(*authored_materialized.root).has_value(),
          "authored content-size motion did not retain precedence over present-empty theme motion");
}

void test_unicode_recursive_layout_and_typed_motion_contracts() {
    using namespace strata;
    const auto rejects = [](const auto& action) {
        try {
            action();
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    check(core::utf8_blankness("\xE2\x80\x83\xC2\xA0") == core::Utf8Blankness::blank,
          "Unicode White_Space was not classified as blank");
    check(core::utf8_blankness("\xE2\x80\x83x") == core::Utf8Blankness::non_blank,
          "Unicode whitespace hid non-blank text");
    check(core::utf8_blankness("\xF0\x28\x8C\x28") == core::Utf8Blankness::malformed,
          "malformed UTF-8 was not distinguished from blank text");
    check(rejects([] { static_cast<void>(ui::Theme("\xE2\x80\x83", ui::ThemeTokens{})); }) &&
              rejects([] {
                  static_cast<void>(runtime::Value(runtime::ThemeTokenValue{"\xC2\xA0"}));
              }) &&
              rejects([] { ui::ThemeWidgetKey{"Panel", "\xE2\x80\x83"}.validate(); }) &&
              rejects([] {
                  static_cast<void>(
                      ui::ThemeMotionPolicy(false, {
                                                       {"standard", ui::MotionTiming{}},
                                                       {"\xE2\x80\x83", ui::MotionTiming{}},
                                                   }));
              }) &&
              rejects([] { static_cast<void>(ui::Theme("\xF0\x28\x8C\x28", ui::ThemeTokens{})); }),
          "theme/token validation did not share Unicode blankness and malformed UTF-8 rules");

    ui::CompiledMotion inline_motion{
        "",
        ui::MotionTrigger::exit,
        ui::MotionTiming{
            70,
            11,
            ui::MotionEasing::cubic_bezier(0.2, -0.3, 0.8, 1.3),
            {ui::MotionRepeatKind::count, 2U},
            true,
            ui::MotionFillMode::forwards,
        },
        {ui::MotionTrack{ui::MotionProperty::opacity,
                         {
                             ui::MotionKeyframe{0.4, 0.25, ui::MotionEasing("cubic-out")},
                         }}},
    };
    ui::ThemeAnimationSet anonymous;
    anonymous.attachments.push_back(ui::ThemeMotionAttachment{
        ui::MotionTrigger::exit,
        ui::ThemeAnimationSpec(inline_motion),
        true,
        ui::MotionDirection::forward,
        ui::MotionTrigger::enter,
    });
    anonymous.validate();
    check(inline_motion.timing.repeat.kind == ui::MotionRepeatKind::count &&
              inline_motion.timing.reverse &&
              inline_motion.timing.fill_mode == ui::MotionFillMode::forwards &&
              inline_motion.tracks.front().keyframes.front().offset == 0.4,
          "anonymous one-keyframe/full timing model was lossy");
    const auto retains_exit = [](ui::ThemeAnimationSet set, std::string name) {
        ui::ThemeNullable<ui::ThemeAnimationSet> motion;
        motion.emplace(std::move(set));
        ui::Theme theme(name, ui::ThemeTokens{}, nullptr, std::nullopt,
                        {{{"Panel", "default"},
                          ui::ThemedWidgetStyle{
                              std::nullopt,
                              std::nullopt,
                              std::nullopt,
                              std::nullopt,
                              std::move(motion),
                          }}});
        ui::ThemeCatalog catalog;
        check(catalog.register_theme(theme), "inline exit theme did not register");
        const ui::ThemeMaterializationResult materialized =
            ui::materialize_theme_tree(node("Panel", "exit.node", properties(name)), catalog,
                                       [](const std::string_view) { return true; });
        ui::RetainedTree tree;
        static_cast<void>(tree.reconcile(materialized.root));
        ui::MotionRuntime runtime;
        runtime.set_supplemental(catalog.declared_animations());
        return runtime.should_retain_for_exit(*tree.root());
    };
    check(retains_exit(anonymous, "retained-exit"),
          "cancelOnDetach=true did not make the inline exit eligible for retention");
    ui::ThemeAnimationSet cancelled_exit = anonymous;
    cancelled_exit.attachments.front().cancel_on_detach = false;
    check(!retains_exit(std::move(cancelled_exit), "cancelled-exit"),
          "cancelOnDetach=false retained a detached exit animation");
    check(rejects([] { static_cast<void>(ui::MotionEasing("not-an-easing")); }),
          "unknown easing silently became linear");
    check(ui::MotionEasing("\xE2\x80\x83 CUBIC_OUT \xC2\xA0") ==
              ui::MotionEasing(ui::MotionEasingKind::cubic_out),
          "Unicode-wrapped named easing did not trim and normalize");
    check(rejects([] { static_cast<void>(ui::MotionEasing("\xF0\x28\x8C\x28")); }),
          "malformed UTF-8 easing name was accepted");

    ui::ThemeAnimationSet repeating_channel;
    repeating_channel.channels.push_back(ui::ThemeMotionChannel{
        "timeline",
        ui::ThemeAnimationSpec(inline_motion),
        ui::MotionInteraction::hover,
        std::nullopt,
    });
    check(rejects([&] { repeating_channel.validate(); }),
          "repeating animation was accepted for a timeline channel");

    ui::CompiledMotion opacity_motion = inline_motion;
    opacity_motion.timing.repeat = {};
    ui::ThemeAnimationSet conflict;
    conflict.channels.push_back(ui::ThemeMotionChannel{
        "timeline",
        ui::ThemeAnimationSpec(opacity_motion),
        ui::MotionInteraction::hover,
        std::nullopt,
    });
    conflict.value_channels.push_back(ui::ThemeMotionValueChannel{
        "target",
        ui::MotionProperty::opacity,
        0.5,
        "standard",
    });
    check(rejects([&] { conflict.validate(); }), "competing typed motion channels were accepted");
    ui::ThemeAnimationSet wrong_kind;
    wrong_kind.value_channels.push_back(ui::ThemeMotionValueChannel{
        "wrong",
        ui::MotionProperty::opacity,
        true,
        "standard",
    });
    check(rejects([&] { wrong_kind.validate(); }),
          "non-interpolable/wrong-kind value channel was accepted");
    ui::ThemeAnimationSet invalid_continuity = anonymous;
    invalid_continuity.attachments.front().continuity_trigger = ui::MotionTrigger::exit;
    check(rejects([&] { invalid_continuity.validate(); }),
          "self-referential continuity trigger was accepted");

    const auto themed_animation = [](std::string theme_name, std::string component) {
        ui::CompiledMotion declaration{
            "e",
            ui::MotionTrigger::animate,
            ui::MotionTiming{},
            {ui::MotionTrack{ui::MotionProperty::opacity,
                             {
                                 ui::MotionKeyframe{0.5, 1.0, std::nullopt},
                             }}},
        };
        ui::ThemeAnimationSet set;
        set.declared_animations.push_back(std::move(declaration));
        ui::ThemeNullable<ui::ThemeAnimationSet> motion;
        motion.emplace(std::move(set));
        std::map<ui::ThemeWidgetKey, ui::ThemedWidgetStyle> styles;
        styles.emplace(ui::ThemeWidgetKey{std::move(component), "d"},
                       ui::ThemedWidgetStyle{std::nullopt, std::nullopt, std::nullopt, std::nullopt,
                                             std::move(motion)});
        return ui::Theme(std::move(theme_name), ui::ThemeTokens{}, nullptr, std::nullopt,
                         std::move(styles));
    };
    ui::ThemeCatalog collision_catalog;
    check(collision_catalog.register_theme(themed_animation("a", "b:c")) &&
              collision_catalog.register_theme(themed_animation("a:b", "c")) &&
              collision_catalog.declared_animations().size() == 2U,
          "colon-bearing scoped animation identities collided");

    ui::Theme empty_style_theme("empty-style", ui::ThemeTokens{}, nullptr, std::nullopt,
                                {{{"Panel", "default"}, ui::ThemedWidgetStyle{}}});
    const ui::ResolvedThemeWidgetStyle empty_style = empty_style_theme.resolved_style("Panel");
    const ui::ResolvedThemeWidgetStyle missing_style = empty_style_theme.resolved_style("Button");
    check(empty_style.explicit_style && empty_style.style == ui::ThemedWidgetStyle{} &&
              empty_style_theme.widget_styles().contains(ui::ThemeWidgetKey{"Panel", "default"}) &&
              !missing_style.explicit_style && missing_style.style.visual.has_value(),
          "explicit all-null style entry collapsed into semantic fallback");
    ui::ThemeCatalog empty_style_catalog;
    check(empty_style_catalog.register_theme(empty_style_theme),
          "all-null style theme did not register");
    const ui::ThemeMaterializationResult empty_style_materialized = ui::materialize_theme_tree(
        node("Panel", "empty-style.node", properties("empty-style")), empty_style_catalog,
        [](const std::string_view) { return true; });
    check(field(*empty_style_materialized.root, "background") == nullptr &&
              field(*empty_style_materialized.root, "foreground") == nullptr &&
              empty_style_materialized.root->properties.contains("$layout"),
          "all-null style entry did not suppress semantic materialization");

    ui::LayoutStyle inside_layout;
    inside_layout.kind = ui::LayoutKind::scroll;
    inside_layout.scroll_viewport_insets = {9.0, 9.0, 9.0, 9.0};
    inside_layout.scroll_viewport_insets_from_inside_border = true;
    ui::ThemeWidgetVisualStyle inside_visual;
    inside_visual.border = ui::ThemeBorder{3.0, {}, true};
    ui::Theme inside_theme("inside", ui::ThemeTokens{}, nullptr, std::nullopt,
                           {{{"Panel", "default"},
                             ui::ThemedWidgetStyle{
                                 inside_visual,
                                 std::nullopt,
                                 std::nullopt,
                                 inside_layout,
                                 std::nullopt,
                             }}});
    ui::ThemeCatalog inside_catalog;
    check(inside_catalog.register_theme(inside_theme), "inside-border theme did not register");
    const ui::ThemeMaterializationResult inside =
        ui::materialize_theme_tree(node("Panel", "inside.node", properties("inside")),
                                   inside_catalog, [](const std::string_view) { return true; });
    const ui::LayoutStyle resolved_inside = ui::layout_style(*inside.root);
    check(field(*inside.root, "border") != nullptr &&
              field(*inside.root, "border")->field("width") != nullptr &&
              field(*inside.root, "border")->field("width")->number() != nullptr &&
              *field(*inside.root, "border")->field("width")->number() == 3.0 &&
              resolved_inside.scroll_viewport_insets == ui::Edges{3.0, 3.0, 3.0, 3.0} &&
              !resolved_inside.scroll_viewport_insets_from_inside_border &&
              ui::layout_detail::scroll_viewport_edges(resolved_inside) ==
                  ui::Edges{3.0, 3.0, 3.0, 3.0},
          "inside-border viewport insets were not resolved during layout materialization");
    const ui::ThemeMaterializationResult authored_inside = ui::materialize_theme_tree(
        node("Panel", "inside.authored",
             properties(
                 "inside",
                 {
                     {"border", runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                                    {"color", runtime::Value(runtime::ColorValue{})},
                                    {"inside", runtime::Value(true)},
                                    {"width", runtime::Value(5.0)},
                                })},
                 })),
        inside_catalog, [](const std::string_view) { return true; });
    check(ui::layout_style(*authored_inside.root).scroll_viewport_insets ==
              ui::Edges{5.0, 5.0, 5.0, 5.0},
          "authored border did not retain precedence over themed viewport insets");

    check(ui::motion_terminal_progress(inline_motion, ui::MotionDirection::forward) == 0.0 &&
              ui::motion_terminal_progress(inline_motion, ui::MotionDirection::reverse) == 1.0,
          "counted alternating terminal direction disagrees with exit completion");
}

[[nodiscard]] std::shared_ptr<const strata::runtime::ApplicationBundle> load_bundle() {
    return strata::runtime::ApplicationBundle::create();
}

[[nodiscard]] strata::compiler::ModuleLoader no_imports() {
    return
        [](const std::string_view, const std::string_view path) -> strata::compiler::ModuleSource {
            throw strata::compiler::ModuleLoadError("unexpected theme fixture import '" +
                                                    std::string(path) + "'");
        };
}

void test_scroll_animation_lifecycle() {
    using namespace strata;
    constexpr std::string_view source = R"(
overlay Main {
  state scrolled = false
  root Scroll(
    key: "scroll",
    vertical: true,
    horizontal: false,
    onScroll: action("state.set", name: "scrolled", value: true),
    layout: { width: 100, height: 100 }
  ) {
    Panel(key: "content", layout: { width: 100, height: 500 })
  }
}
)";
    runtime::ApplicationContext application("theme-scroll", load_bundle());
    check(application
              .compile_and_activate(
                  compiler::ModuleSource{"theme-scroll.strata", std::string(source)}, no_imports(),
                  0U)
              .activated(),
          "scroll animation fixture did not activate");
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 320;
    environment.framebuffer_height = 240;
    environment.logical_width = 320.0;
    environment.logical_height = 240.0;
    ui::ThemeMotionPolicy timings(
        false,
        {
            {"fast",
             ui::MotionTiming{200'000'000, 0, "linear", {}, false, ui::MotionFillMode::both}},
            {"standard",
             ui::MotionTiming{200'000'000, 0, "linear", {}, false, ui::MotionFillMode::both}},
        });
    ui::Surface surface("theme-scroll", application, runtime::LayerRole::overlay, "Main",
                        environment, {}, ui::WidgetRegistry{}, ui::BehaviorRegistry{}, nullptr,
                        ui::Theme("scroll-theme", ui::ThemeTokens{}, nullptr, timings));
    bool unicode_timing_rejected = false;
    try {
        static_cast<void>(surface.animate_scroll_to(ui::ScrollAnimationRequest{
            "scroll",
            std::nullopt,
            1.0,
            "\xE2\x80\x83",
            std::nullopt,
        }));
    } catch (const std::invalid_argument&) {
        unicode_timing_rejected = true;
    }
    check(unicode_timing_rejected, "animated scrolling accepted a Unicode-whitespace timing name");
    ui::ThemeMotionPolicy scoped_timings(
        false,
        {
            {"fast",
             ui::MotionTiming{100'000'000, 0, "linear", {}, false, ui::MotionFillMode::both}},
            {"standard",
             ui::MotionTiming{200'000'000, 0, "linear", {}, false, ui::MotionFillMode::both}},
        });
    check(surface.set_scoped_theme(
              "scroll", ui::Theme("scroll-local", ui::ThemeTokens{}, nullptr, scoped_timings)),
          "scroll target scoped motion policy did not install");
    static_cast<void>(surface.frame(1'000'000));
    check(surface.animate_scroll_to(ui::ScrollAnimationRequest{
              "scroll",
              std::nullopt,
              300.0,
              "fast",
              std::nullopt,
          }),
          "scroll animation did not start");
    static_cast<void>(surface.frame(11'000'000));
    const ui::SurfaceFrame& halfway_frame = surface.frame(61'000'000);
    const ui::RetainedNode* scroll = surface.tree().find_key("scroll");
    check(scroll != nullptr, "scroll target detached unexpectedly");
    const ui::LayoutRecord* halfway = surface.layout().find(scroll->identity());
    check(halfway != nullptr && std::abs(halfway->scroll_offset.y - 150.0) < 0.001,
          "scroll timing did not sample its halfway offset");
    check(halfway_frame.lifecycle_input.events.size() == 1U,
          "one effective animation offset change did not emit exactly one scroll event");
    const data::JsonValue& halfway_event = halfway_frame.lifecycle_input.events.front();
    const data::JsonValue* halfway_offset = halfway_event.find("offset");
    check(halfway_event.find("type") != nullptr &&
              halfway_event.find("type")->string() != nullptr &&
              *halfway_event.find("type")->string() == "scroll-changed" &&
              halfway_offset != nullptr && halfway_offset->find("y") != nullptr &&
              halfway_offset->find("y")->number() != nullptr &&
              std::abs(*halfway_offset->find("y")->number() - 150.0) < 0.001,
          "authored onScroll lost its post-mutation clamped offset payload");
    ui::InputOperationResult no_op;
    check(!surface.input().scroll_to("scroll", halfway->scroll_offset, no_op) &&
              no_op.events.empty() && no_op.action_outcomes.empty(),
          "an unchanged scroll offset emitted a redundant authored event");

    check(surface.animate_scroll_to(ui::ScrollAnimationRequest{
              "scroll",
              std::nullopt,
              250.0,
              "fast",
              std::nullopt,
          }),
          "replacement scroll animation did not start");
    static_cast<void>(surface.frame(71'000'000));
    static_cast<void>(surface.frame(121'000'000));
    scroll = surface.tree().find_key("scroll");
    const ui::LayoutRecord* replaced = surface.layout().find(scroll->identity());
    check(replaced != nullptr && std::abs(replaced->scroll_offset.y - 200.0) < 0.001,
          "replacement animation did not continue from the displayed offset");

    static_cast<void>(surface.input().enqueue_scroll("scroll", 0.0, -1.0));
    static_cast<void>(surface.frame(131'000'000));
    check(surface.active_scroll_animation_count() == 0U,
          "direct input did not cancel scroll animation");

    ui::ThemeMotionPolicy scoped_reduced(
        true, {
                  {"fast",
                   ui::MotionTiming{100'000'000, 0, "linear", {}, false, ui::MotionFillMode::both}},
                  {"standard",
                   ui::MotionTiming{200'000'000, 0, "linear", {}, false, ui::MotionFillMode::both}},
              });
    check(surface.set_scoped_theme("scroll", ui::Theme("scroll-local-reduced", ui::ThemeTokens{},
                                                       nullptr, scoped_reduced)),
          "scoped reduced-motion policy did not install");
    static_cast<void>(surface.frame(132'000'000));
    check(surface.animate_scroll_to(ui::ScrollAnimationRequest{
              "scroll",
              std::nullopt,
              300.0,
              "fast",
              std::nullopt,
          }) &&
              surface.active_scroll_animation_count() == 0U,
          "scoped reduced-motion policy did not synchronously snap scrolling");
    static_cast<void>(surface.frame(133'000'000));
    scroll = surface.tree().find_key("scroll");
    check(surface.layout().find(scroll->identity())->scroll_offset.y == 300.0,
          "scoped reduced-motion scroll snap missed its target");
    check(surface.set_scoped_theme(
              "scroll", ui::Theme("scroll-local", ui::ThemeTokens{}, nullptr, scoped_timings)),
          "scoped motion policy did not restore");
    static_cast<void>(surface.frame(134'000'000));

    check(surface.animate_scroll_to(ui::ScrollAnimationRequest{
              "scroll",
              std::nullopt,
              0.0,
              "missing",
              50'000'000,
          }),
          "unknown-policy scroll animation did not use fallback timing");
    static_cast<void>(surface.frame(141'000'000));
    check(std::ranges::any_of(surface.last_frame().diagnostics.records,
                              [](const auto& diagnostic) {
                                  return diagnostic.diagnostic.code ==
                                         "STRATA.ANIMATION.MOTION_POLICY_UNKNOWN";
                              }),
          "unknown scroll timing did not publish a bounded diagnostic");

    ui::SurfaceEnvironment reduced = surface.environment();
    ++reduced.generation;
    reduced.reduced_motion = true;
    check(surface.adopt_environment(reduced), "reduced-motion environment did not adopt");
    check(surface.animate_scroll_to(ui::ScrollAnimationRequest{
              "scroll",
              std::nullopt,
              100.0,
              "fast",
              std::nullopt,
          }) &&
              surface.active_scroll_animation_count() == 0U,
          "reduced motion did not synchronously snap scroll animation");
    static_cast<void>(surface.frame(151'000'000));
    scroll = surface.tree().find_key("scroll");
    check(surface.layout().find(scroll->identity())->scroll_offset.y == 100.0,
          "reduced-motion scroll snap did not reach its target");

    reduced.reduced_motion = false;
    ++reduced.generation;
    check(surface.adopt_environment(reduced), "motion environment did not restore");
    check(surface.animate_scroll_to(ui::ScrollAnimationRequest{
              "scroll",
              std::nullopt,
              0.0,
              "fast",
              10'000'000,
          }),
          "terminal scroll animation did not start");
    static_cast<void>(surface.frame(161'000'000));
    static_cast<void>(surface.frame(171'000'000));
    check(surface.active_scroll_animation_count() == 0U,
          "completed scroll animation did not settle");
    const ui::SurfaceFrame& idle = surface.frame(181'000'000);
    check(idle.operations.layout_measured_nodes == 0U && idle.operations.rebuilds == 0U,
          "settled scroll animation kept the Surface off its idle fast path");
    check(!surface.animate_scroll_to(ui::ScrollAnimationRequest{
              "scroll",
              std::nullopt,
              0.0,
              "fast",
              std::nullopt,
          }) &&
              surface.active_scroll_animation_count() == 0U,
          "already-settled scroll target scheduled redundant work");

    check(surface.animate_scroll_to(ui::ScrollAnimationRequest{
              "scroll",
              std::nullopt,
              100.0,
              "fast",
              std::nullopt,
          }),
          "interaction-cancellation scroll animation did not start");
    surface.cancel_interactions();
    check(surface.active_scroll_animation_count() == 0U,
          "terminal interaction cancellation retained scroll animation state");
    check(surface.animate_scroll_to(ui::ScrollAnimationRequest{
              "scroll",
              std::nullopt,
              100.0,
              "fast",
              std::nullopt,
          }),
          "detach-cancellation scroll animation did not start");
    constexpr std::string_view detached_source =
        "overlay Main { root Panel(key: \"replacement\") }";
    check(application
              .compile_and_activate(compiler::ModuleSource{"theme-scroll-detached.strata",
                                                           std::string(detached_source)},
                                    no_imports(), 1U)
              .activated(),
          "detach fixture did not activate");
    static_cast<void>(surface.frame(191'000'000));
    check(surface.active_scroll_animation_count() == 0U,
          "detached scroll identity retained its animation");
}

} // namespace

int strata_test_theme() {
    try {
        test_defaults_and_validation();
        test_scopes_styles_tokens_and_lazy_ranges();
        test_catalog_and_motion_policy();
        test_exact_resolution_validation_and_local_scope();
        test_full_style_round_trip_and_nullability();
        test_unicode_recursive_layout_and_typed_motion_contracts();
        test_scroll_animation_lifecycle();
        std::cout << "strata_theme_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_theme_tests: " << error.what() << '\n';
        return 1;
    }
}
