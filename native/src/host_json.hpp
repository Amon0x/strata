#pragma once

#include <strata/host.hpp>

namespace strata::data {
class JsonValue;
}

namespace strata::host {
/** Converts already-parsed host configuration without a JSON serialization/parsing round trip. */
[[nodiscard]] Value from_json_value(const data::JsonValue& value);
}
