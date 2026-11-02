#pragma once

#include "ui/render.hpp"

namespace strata::resource {
class SvgImageRegistry;
}

namespace strata::ui {

class BehaviorRegistry;
class CommandIndex;
class InputPresentation;
class MotionRuntime;
class RetainedNode;
class TextEngine;
struct LayoutRecord;
struct LayoutResult;

[[nodiscard]] bool has_behavior_overlay(
    const BehaviorRegistry& registry,
    const RetainedNode& node,
    const InputPresentation& input,
    bool detached
);

void append_behavior_overlays(
    const BehaviorRegistry& registry,
    const RetainedNode& node,
    const LayoutRecord& layout,
    const LayoutResult& layout_result,
    const InputPresentation& input,
    const CommandIndex& commands,
    const TextEngine* text,
    const resource::SvgImageRegistry* svg_images,
    const MotionRuntime* motion,
    double inherited_opacity,
    bool detached,
    RenderCommandBuffer& output,
    bool apply_presentation_opacity = true
);

} // namespace strata::ui
