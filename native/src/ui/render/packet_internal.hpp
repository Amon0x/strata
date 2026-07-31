#pragma once

#include <cstdint>
#include <vector>

#include "ui/render.hpp"

namespace strata::ui::detail {

/** Shared v2 logical-command payload encoding. */
[[nodiscard]] std::vector<std::uint8_t> encode_command_payload_v2(const RenderCommand& command);

} // namespace strata::ui::detail
