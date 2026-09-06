#pragma once

#include <SDL.h>
#include <optional>
#include <strata/input.hpp>

namespace strata::preview {

// Pure translation: SDL owns the window system; Strata owns focus, gestures, and editing.
[[nodiscard]] std::optional<InputEvent> translate_input(const SDL_Event& event,
                                                        double coordinate_scale, Point pointer,
                                                        std::int64_t time_nanoseconds);

} // namespace strata::preview
