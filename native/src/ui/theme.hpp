#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "runtime/diagnostic.hpp"
#include "runtime/value.hpp"
#include "ui/layout.hpp"
#include "ui/motion/model.hpp"
#include "ui/tree.hpp"

namespace strata::ui {

inline constexpr std::string_view default_theme_name = "strata-default";
inline constexpr std::string_view default_widget_variant = "default";
inline constexpr std::string_view default_motion_timing_name = "standard";

struct ThemeTokens final {
    runtime::ColorValue surface{34U, 38U, 46U, 220U};
    runtime::ColorValue surface_raised{24U, 24U, 42U, 240U};
    runtime::ColorValue foreground{236U, 240U, 244U, 255U};
    runtime::ColorValue muted_foreground{160U, 168U, 178U, 220U};
    runtime::ColorValue accent{91U, 141U, 239U, 255U};
    runtime::ColorValue danger{224U, 74U, 74U, 255U};
    runtime::ColorValue focus{112U, 170U, 250U, 255U};
    double spacing_unit = 4.0;
    double radius = 4.0;
    double density = 1.0;

    void validate() const;
    [[nodiscard]] const runtime::ColorValue* color(std::string_view name) const noexcept;
    [[nodiscard]] std::optional<double> number(std::string_view name) const noexcept;
    [[nodiscard]] friend bool operator==(const ThemeTokens&, const ThemeTokens&) = default;
};

struct ThemeBorder final {
    double width = 1.0;
    runtime::ColorValue color;
    bool inside = true;

    void validate() const;
    [[nodiscard]] friend bool operator==(const ThemeBorder&, const ThemeBorder&) = default;
};

/** An engaged outer optional is authored; an empty inner optional explicitly removes the value. */
template <typename T>
using ThemeNullable = std::optional<std::optional<T>>;

struct ThemeWidgetVisualStyle final {
    std::optional<runtime::ColorValue> background;
    runtime::ColorValue foreground{255U, 255U, 255U, 255U};
    std::optional<ThemeBorder> border;
    double radius = 0.0;
    std::optional<runtime::ColorValue> hover_overlay;
    std::optional<runtime::ColorValue> active_overlay;
    std::optional<ThemeBorder> focus_ring;
    double disabled_opacity = 0.45;
    double opacity = 1.0;
    double translate_x = 0.0;
    double translate_y = 0.0;
    double scale = 1.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    std::optional<runtime::ColorValue> track;
    std::optional<runtime::ColorValue> fill;
    std::optional<runtime::ColorValue> thumb;
    std::optional<runtime::ColorValue> selection;
    std::optional<runtime::ColorValue> scrim;
    std::optional<double> indicator_size;
    std::optional<double> indicator_inset;
    std::optional<double> track_width;
    std::optional<double> track_height;
    std::optional<double> track_radius;
    std::optional<double> thumb_size;
    std::optional<double> thumb_radius;
    std::optional<double> indicator_position;

    void validate() const;
    [[nodiscard]] friend bool operator==(
        const ThemeWidgetVisualStyle&,
        const ThemeWidgetVisualStyle&
    ) = default;
};

struct ThemeWidgetTextVisualStyle final {
    runtime::ColorValue color{255U, 255U, 255U, 255U};
    runtime::ColorValue hint_color{160U, 168U, 178U, 180U};
    runtime::ColorValue selection_color{72U, 119U, 218U, 96U};
    runtime::ColorValue caret_color{255U, 255U, 255U, 255U};

    [[nodiscard]] friend bool operator==(
        const ThemeWidgetTextVisualStyle&,
        const ThemeWidgetTextVisualStyle&
    ) = default;
};

struct ThemeTextLayoutStyle final {
    std::string primary_font = "strata:fonts/default-medium";
    std::vector<std::string> fallback_fonts;
    double pixel_size = 12.0;
    std::uint32_t style_flags = 0U;
    std::optional<double> line_height;
    double line_height_multiplier = 1.0;
    double letter_spacing = 0.0;

    void validate() const;
    [[nodiscard]] friend bool operator==(
        const ThemeTextLayoutStyle&,
        const ThemeTextLayoutStyle&
    ) = default;
};

using ThemeWidgetLayoutStyle = LayoutStyle;

struct ThemeAnimationSpec final {
    std::variant<std::string, CompiledMotion> value = std::string{};

    ThemeAnimationSpec() = default;
    ThemeAnimationSpec(std::string name) : value(std::move(name)) {}
    ThemeAnimationSpec(const char* name) : value(std::string(name != nullptr ? name : "")) {}
    ThemeAnimationSpec(CompiledMotion animation) : value(std::move(animation)) {}
    [[nodiscard]] const std::string* named() const noexcept {
        return std::get_if<std::string>(&value);
    }
    [[nodiscard]] const CompiledMotion* inline_animation() const noexcept {
        return std::get_if<CompiledMotion>(&value);
    }
    [[nodiscard]] friend bool operator==(const ThemeAnimationSpec&, const ThemeAnimationSpec&) = default;
};

struct ThemeMotionAttachment final {
    MotionTrigger trigger = MotionTrigger::animate;
    ThemeAnimationSpec animation;
    bool cancel_on_detach = true;
    MotionDirection direction = MotionDirection::forward;
    std::optional<MotionTrigger> continuity_trigger;
    [[nodiscard]] friend bool operator==(const ThemeMotionAttachment&, const ThemeMotionAttachment&) = default;
};

struct ThemeMotionChannel final {
    std::string id;
    ThemeAnimationSpec animation;
    std::optional<MotionInteraction> interaction;
    std::optional<bool> state_target;
    [[nodiscard]] friend bool operator==(const ThemeMotionChannel&, const ThemeMotionChannel&) = default;
};

struct ThemeMotionValueChannel final {
    std::string id;
    MotionProperty property = MotionProperty::opacity;
    MotionValue target = 0.0;
    std::string timing = std::string(default_motion_timing_name);
    [[nodiscard]] friend bool operator==(
        const ThemeMotionValueChannel&,
        const ThemeMotionValueChannel&
    ) = default;
};

struct ThemeResolvedPropertyMotion final {
    std::vector<MotionProperty> properties;
    std::string timing = std::string(default_motion_timing_name);
    [[nodiscard]] friend bool operator==(
        const ThemeResolvedPropertyMotion&,
        const ThemeResolvedPropertyMotion&
    ) = default;
};

struct ThemeDisclosureMotion final {
    bool expanded = false;
    double collapsed_extent = 0.0;
    std::string timing = std::string(default_motion_timing_name);
    [[nodiscard]] friend bool operator==(const ThemeDisclosureMotion&, const ThemeDisclosureMotion&) = default;
};

struct ThemeContentSizeMotion final {
    bool animate_width = false;
    bool animate_height = true;
    bool clip = true;
    std::string timing = std::string(default_motion_timing_name);
    [[nodiscard]] friend bool operator==(const ThemeContentSizeMotion&, const ThemeContentSizeMotion&) = default;
};

struct ThemeAnimationSet final {
    std::vector<ThemeMotionAttachment> attachments;
    std::vector<CompiledMotion> declared_animations;
    std::vector<ThemeMotionChannel> channels;
    std::vector<ThemeMotionValueChannel> value_channels;
    std::optional<ThemeResolvedPropertyMotion> resolved_properties;
    std::optional<ThemeDisclosureMotion> disclosure;
    std::optional<ThemeContentSizeMotion> content_size;

    void validate() const;
    [[nodiscard]] friend bool operator==(const ThemeAnimationSet&, const ThemeAnimationSet&) = default;
};

struct ThemedWidgetStyle final {
    /** An all-omitted value is valid when present in Theme::widget_styles(). */
    std::optional<ThemeWidgetVisualStyle> visual;
    std::optional<ThemeWidgetTextVisualStyle> text_visual;
    std::optional<ThemeTextLayoutStyle> text_layout;
    std::optional<ThemeWidgetLayoutStyle> layout;
    /** Engaged+null disables motion; an engaged empty set remains a distinct present-empty value. */
    ThemeNullable<ThemeAnimationSet> motion;

    void validate() const;
    [[nodiscard]] friend bool operator==(
        const ThemedWidgetStyle&,
        const ThemedWidgetStyle&
    ) = default;
};

struct ThemeWidgetKey final {
    std::string component_type;
    std::string variant = std::string(default_widget_variant);

    void validate() const;
    [[nodiscard]] friend bool operator==(const ThemeWidgetKey&, const ThemeWidgetKey&) = default;
    [[nodiscard]] friend bool operator<(const ThemeWidgetKey& left, const ThemeWidgetKey& right) {
        return left.component_type != right.component_type
                   ? left.component_type < right.component_type
                   : left.variant < right.variant;
    }
};

struct ResolvedThemeWidgetStyle final {
    ThemedWidgetStyle style;
    std::string owner_theme;
    ThemeWidgetKey key;
    bool explicit_style = false;
};

class ThemeMotionPolicy final {
public:
    ThemeMotionPolicy();
    ThemeMotionPolicy(bool reduced_motion, std::map<std::string, MotionTiming, std::less<>> timings);

    [[nodiscard]] bool reduced_motion() const noexcept;
    [[nodiscard]] const std::map<std::string, MotionTiming, std::less<>>& timings() const noexcept;
    [[nodiscard]] const MotionTiming* find(std::string_view name) const noexcept;
    [[nodiscard]] const MotionTiming& timing_or_default(std::string_view name) const noexcept;
    [[nodiscard]] friend bool operator==(
        const ThemeMotionPolicy&,
        const ThemeMotionPolicy&
    ) = default;

private:
    bool reduced_motion_ = false;
    std::map<std::string, MotionTiming, std::less<>> timings_;
};

class Theme final {
public:
    Theme();
    Theme(
        std::string name,
        ThemeTokens tokens,
        std::shared_ptr<const Theme> parent = {},
        std::optional<ThemeMotionPolicy> motion_policy = std::nullopt,
        std::map<ThemeWidgetKey, ThemedWidgetStyle> widget_styles = {}
    );

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const ThemeTokens& tokens() const noexcept;
    [[nodiscard]] const std::shared_ptr<const Theme>& parent() const noexcept;
    [[nodiscard]] const ThemeMotionPolicy& motion_policy() const noexcept;
    [[nodiscard]] const std::map<ThemeWidgetKey, ThemedWidgetStyle>& widget_styles() const noexcept;
    [[nodiscard]] ThemedWidgetStyle style(
        std::string_view component_type,
        std::string_view variant = default_widget_variant
    ) const;
    [[nodiscard]] ResolvedThemeWidgetStyle resolved_style(
        std::string_view component_type,
        std::string_view variant = default_widget_variant
    ) const;
    [[nodiscard]] std::shared_ptr<const Theme> derive(
        std::string name,
        ThemeTokens tokens,
        std::map<ThemeWidgetKey, ThemedWidgetStyle> widget_styles = {}
    ) const;
    [[nodiscard]] friend bool operator==(const Theme&, const Theme&) = default;

private:
    [[nodiscard]] std::optional<ResolvedThemeWidgetStyle> explicit_style(
        std::string_view component_type,
        std::string_view variant
    ) const;
    [[nodiscard]] ThemedWidgetStyle semantic_style(
        std::string_view component_type,
        std::string_view variant
    ) const;

    std::string name_;
    ThemeTokens tokens_;
    std::shared_ptr<const Theme> parent_;
    ThemeMotionPolicy motion_policy_;
    std::map<ThemeWidgetKey, ThemedWidgetStyle> widget_styles_;
};

/** Per-Surface immutable theme objects and root selection; never process-global. */
class ThemeCatalog final {
public:
    ThemeCatalog();
    explicit ThemeCatalog(Theme root);

    [[nodiscard]] const std::shared_ptr<const Theme>& root() const noexcept;
    [[nodiscard]] const std::shared_ptr<const Theme>* find(std::string_view name) const noexcept;
    /** Returns true only when the catalog changed. */
    [[nodiscard]] bool register_theme(Theme theme);
    /** Registers and selects the supplied root. Returns true only when the effective root changed. */
    [[nodiscard]] bool set_root(Theme theme);
    /** The active root cannot be removed. */
    [[nodiscard]] bool unregister_theme(std::string_view name);
    [[nodiscard]] const std::shared_ptr<const Theme>* scoped_theme(
        std::string_view node_key
    ) const noexcept;
    [[nodiscard]] bool set_scoped_theme(std::string node_key, Theme theme);
    [[nodiscard]] bool clear_scoped_theme(std::string_view node_key);
    /** Qualified, immutable theme-local animation declarations for a Surface motion catalog. */
    [[nodiscard]] std::map<std::string, CompiledMotion, std::less<>> declared_animations() const;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    void advance_generation();

    std::shared_ptr<const Theme> root_;
    std::map<std::string, std::shared_ptr<const Theme>, std::less<>> themes_;
    std::map<std::string, std::shared_ptr<const Theme>, std::less<>> scoped_themes_;
    std::uint64_t generation_ = 1U;
};

struct ThemeMaterializationStats final {
    std::size_t resolved_nodes = 0U;
    std::size_t symbolic_token_patches = 0U;
};

struct ThemeMaterializationResult final {
    std::shared_ptr<const DescriptionNode> root;
    ThemeMaterializationStats stats;
};

/** Weak structural-sharing cache for repeated materialization of immutable description nodes. */
class ThemeMaterializationCache final {
public:
    [[nodiscard]] std::shared_ptr<const DescriptionNode> find(
        const std::shared_ptr<const DescriptionNode>& source,
        const std::shared_ptr<const Theme>& effective_theme,
        const std::optional<std::string>& scope_namespace,
        std::uint64_t catalog_generation
    );
    void store(
        const std::shared_ptr<const DescriptionNode>& source,
        const std::shared_ptr<const Theme>& effective_theme,
        std::optional<std::string> scope_namespace,
        std::uint64_t catalog_generation,
        const std::shared_ptr<const DescriptionNode>& materialized
    );
    void purge(std::uint64_t catalog_generation);
    void clear() noexcept;

private:
    struct Entry final {
        std::weak_ptr<const DescriptionNode> source;
        std::weak_ptr<const Theme> effective_theme;
        std::optional<std::string> scope_namespace;
        std::uint64_t catalog_generation = 0U;
        std::weak_ptr<const DescriptionNode> materialized;
    };
    std::map<const DescriptionNode*, Entry> entries_;
};

using ThemeTypePredicate = std::function<bool(std::string_view)>;
using UnknownThemeTiming = std::function<void(
    std::string_view,
    const DescriptionNode&
)>;

/**
 * Resolves the authored tree while leaving virtual providers unresolved. Entering rows are later
 * projected from their retained collection's inherited context by materialize_theme_subtree.
 */
[[nodiscard]] ThemeMaterializationResult materialize_theme_tree(
    const std::shared_ptr<const DescriptionNode>& root,
    const ThemeCatalog& catalog,
    const ThemeTypePredicate& themed_type,
    const UnknownThemeTiming& unknown_timing = {},
    ThemeMaterializationCache* cache = nullptr
);
/**
 * Projects one entering retained virtual subtree from its collection parent's already-resolved
 * inherited context. This avoids reconstructing and re-theming the description path above it.
 */
[[nodiscard]] ThemeMaterializationResult materialize_theme_subtree(
    const std::shared_ptr<const DescriptionNode>& root,
    const ThemeCatalog& catalog,
    const std::shared_ptr<const Theme>& inherited_theme,
    const std::optional<std::string>& inherited_scope_namespace,
    const ThemeTypePredicate& themed_type,
    const UnknownThemeTiming& unknown_timing = {},
    ThemeMaterializationCache* cache = nullptr
);

/** Reads the policy annotation installed by materialize_theme_tree. */
[[nodiscard]] bool theme_motion_reduced(
    const DescriptionNode& description,
    bool environment_reduced_motion
) noexcept;
[[nodiscard]] MotionTiming theme_motion_timing(
    const DescriptionNode& description,
    std::string_view name
) noexcept;
[[nodiscard]] bool theme_motion_timing_defined(
    const DescriptionNode& description,
    std::string_view name
) noexcept;

} // namespace strata::ui
