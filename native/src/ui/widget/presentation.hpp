#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/render.hpp"
#include "ui/widget/registry.hpp"

namespace strata::resource {
class SvgImageRegistry;
}

namespace strata::ui {

class CommandIndex;
class InputRouter;
class MotionRuntime;
struct MotionComputedValues;
class TextEngine;

enum class WidgetTextAlignment { start, center, end };

struct WidgetVisualStyle final {
    std::optional<Paint> background;
    RenderColor foreground{236U, 240U, 244U, 255U};
    std::optional<RenderBorder> border;
    double radius = 4.0;
    std::optional<RenderColor> hover_overlay{RenderColor{255U, 255U, 255U, 18U}};
    std::optional<RenderColor> active_overlay{RenderColor{0U, 0U, 0U, 32U}};
    std::optional<RenderBorder> focus_ring{RenderBorder{
        2.0,
        RenderColor{112U, 170U, 250U, 255U},
        true,
    }};
    double disabled_opacity = 0.45;
    double opacity = 1.0;
    Paint track{RenderColor{24U, 24U, 42U, 240U}};
    Paint fill{RenderColor{91U, 141U, 239U, 255U}};
    RenderColor thumb{236U, 240U, 244U, 255U};
    RenderColor selection{91U, 141U, 239U, 255U};
    RenderColor text_hint{160U, 168U, 178U, 220U};
    RenderColor text_selection{72U, 119U, 218U, 96U};
    RenderColor caret{236U, 240U, 244U, 255U};
    Paint scrim{RenderColor{0U, 0U, 0U, 150U}};
    std::optional<double> indicator_size;
    std::optional<double> indicator_inset;
    std::optional<double> track_width;
    std::optional<double> track_height;
    std::optional<double> track_radius;
    std::optional<double> track_gap;
    std::optional<double> active_track_gap;
    std::optional<double> thumb_size;
    std::optional<double> thumb_width;
    std::optional<double> thumb_height;
    std::optional<double> active_thumb_width;
    std::optional<double> active_thumb_height;
    std::optional<double> thumb_radius;
    std::optional<double> indicator_position;
};

/** Typed, per-node presentation capability passed only to a widget lifecycle hook. */
class WidgetRenderScope final {
  public:
    WidgetRenderScope(const RetainedNode& node, const LayoutRecord& layout,
                      const LayoutResult& layout_result, const InputRouter& input,
                      const CommandIndex& commands, const TextEngine* text,
                      const resource::SvgImageRegistry* svg_images, const MotionRuntime* motion,
                      double inherited_opacity, WidgetVisualProfile visual_profile,
                      std::vector<RenderCommand>& output, bool apply_presentation_opacity = true);

    [[nodiscard]] const RetainedNode& node() const noexcept;
    [[nodiscard]] const LayoutRecord& layout() const noexcept;
    [[nodiscard]] const LayoutResult& layout_result() const noexcept;
    [[nodiscard]] const InputRouter& input() const noexcept;
    [[nodiscard]] const CommandIndex& command_index() const noexcept;
    [[nodiscard]] const TextEngine* text_engine() const noexcept;
    [[nodiscard]] const MotionComputedValues* motion_values() const noexcept;
    [[nodiscard]] const WidgetVisualStyle& visual() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool focused() const noexcept;
    [[nodiscard]] bool focus_visible() const noexcept;
    [[nodiscard]] bool hovered() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] double alpha() const noexcept;
    [[nodiscard]] Rect root_bounds() const noexcept;
    [[nodiscard]] double motion_progress(std::string_view id, double fallback = 0.0) const noexcept;

    [[nodiscard]] const runtime::Value* property(std::string_view name) const noexcept;
    [[nodiscard]] const runtime::Value* style(std::string_view name) const noexcept;
    [[nodiscard]] const runtime::Value* retained(std::string_view name) const noexcept;
    [[nodiscard]] double number(std::string_view name, double fallback) const noexcept;
    [[nodiscard]] bool boolean(std::string_view name, bool fallback) const noexcept;
    [[nodiscard]] std::string string(std::string_view name, std::string fallback = {}) const;
    [[nodiscard]] const runtime::ValueList* list(std::string_view name) const noexcept;
    [[nodiscard]] double effective_number(std::string_view controlled, std::string_view retained,
                                          std::string_view initial, double fallback) const noexcept;
    [[nodiscard]] bool effective_boolean(std::string_view controlled, std::string_view retained,
                                         std::string_view initial, bool fallback) const noexcept;
    [[nodiscard]] std::optional<std::string_view> node_text() const noexcept;

    void rounded_rect(Rect bounds, Paint fill, std::optional<RenderBorder> border = std::nullopt,
                      std::optional<double> radius = std::nullopt);
    void border(Rect bounds, RenderBorder border, std::optional<double> radius = std::nullopt);
    void solid_rect(Rect bounds, Paint fill);
    void image(Rect bounds, std::string image,
               RenderColor tint = RenderColor{255U, 255U, 255U, 255U},
               TextureRegion source = TextureRegion{});
    void nine_patch(Rect bounds, std::string texture, Edges source_insets, Edges destination_insets,
                    TextureRegion source = TextureRegion{},
                    RenderColor tint = RenderColor{255U, 255U, 255U, 255U});
    /** Draws one authored vector shape inside `bounds`, in that rectangle's normalized space. */
    void shape(Rect bounds, PathShape shape);
    void custom_mesh(Rect bounds, std::string mesh, MeshGeometry geometry,
                     std::optional<std::string> texture = std::nullopt,
                     std::optional<MaterialState> material = std::nullopt);
    void blur_region(Rect bounds, double radius, std::size_t downsample = 1U);
    void shadow(Rect bounds, CornerRadii radii, RenderColor color, double radius,
                double spread = 0.0);
    void push_clip(Rect bounds, CornerRadii radii = {});
    void pop_clip();
    void push_transform(double scale, Point translation);
    void pop_transform();
    void append(RenderCommand command, double opacity = 1.0);
    void text(std::string_view value, Point origin, RenderColor color, double alignment_width = 0.0,
              WidgetTextAlignment horizontal_alignment = WidgetTextAlignment::start,
              double alignment_height = 0.0,
              WidgetTextAlignment vertical_alignment = WidgetTextAlignment::start);
    void node_text(Point origin, RenderColor color);
    void rich_text(Point origin, RenderColor fallback);
    void focus(Rect bounds);
    /** Shared hover/pressed projection for retained widgets and presenter-owned subtargets. */
    void interaction(Rect bounds, std::string_view subtarget = {});

  private:
    const RetainedNode& node_;
    const LayoutRecord& layout_;
    const LayoutResult& layout_result_;
    const InputRouter& input_;
    const CommandIndex& commands_;
    const TextEngine* text_;
    const resource::SvgImageRegistry* svg_images_;
    const MotionRuntime* motion_;
    double inherited_opacity_ = 1.0;
    bool apply_presentation_opacity_ = true;
    std::vector<RenderCommand>& output_;
    WidgetVisualStyle visual_;
    bool enabled_ = true;
};

[[nodiscard]] std::vector<RenderCommand> build_widget_fragment(
    const WidgetRegistry& registry, const RetainedNode& node, const LayoutRecord& layout,
    const LayoutResult& layout_result, const InputRouter& input, const CommandIndex& commands,
    const TextEngine* text, const resource::SvgImageRegistry* svg_images,
    const MotionRuntime* motion, double inherited_opacity, bool apply_presentation_opacity = true);

[[nodiscard]] std::vector<RenderCommand>
build_widget_overlay(const WidgetRegistry& registry, const RetainedNode& node,
                     const LayoutRecord& layout, const LayoutResult& layout_result,
                     const InputRouter& input, const CommandIndex& commands, const TextEngine* text,
                     const resource::SvgImageRegistry* svg_images, const MotionRuntime* motion,
                     double inherited_opacity);

void append_widget_foreground(const WidgetRegistry& registry, const RetainedNode& node,
                              const LayoutRecord& layout, const LayoutResult& layout_result,
                              const InputRouter& input, const CommandIndex& commands,
                              const TextEngine* text, const resource::SvgImageRegistry* svg_images,
                              const MotionRuntime* motion, double inherited_opacity,
                              std::vector<RenderCommand>& output);

[[nodiscard]] std::optional<Rect>
widget_descendant_clip(const WidgetRegistry& registry, const RetainedNode& node,
                       const LayoutRecord& layout, const LayoutResult& layout_result,
                       const InputRouter& input, const CommandIndex& commands,
                       const TextEngine* text, const resource::SvgImageRegistry* svg_images,
                       const MotionRuntime* motion, double inherited_opacity);

[[nodiscard]] const std::string* widget_string_value(const runtime::Value* value) noexcept;
[[nodiscard]] const std::string* widget_image_value(const runtime::Value* value) noexcept;
[[nodiscard]] TextureRegion
widget_texture_region(const runtime::Value* value,
                      TextureRegion fallback = TextureRegion{}) noexcept;
[[nodiscard]] RenderColor widget_color(const runtime::Value* value, RenderColor fallback) noexcept;
[[nodiscard]] RenderColor widget_opacity(RenderColor color, double multiplier) noexcept;
[[nodiscard]] std::string widget_number_text(double value);

void register_primitive_widget_presenters(WidgetRegistry& registry);
void register_control_widget_presenters(WidgetRegistry& registry);
void register_shell_widget_presenters(WidgetRegistry& registry);
void register_collection_widget_presenters(WidgetRegistry& registry);

/** Shared detached overlay hook for command-bound Button and IconButton. */
void command_tooltip_overlay(WidgetRenderScope& scope);

} // namespace strata::ui
