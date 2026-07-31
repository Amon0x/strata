#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace strata::resource {

class ResourceError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ResourceId final {
public:
    [[nodiscard]] static ResourceId parse(std::string_view value);

    [[nodiscard]] const std::string& value() const noexcept;
    [[nodiscard]] std::filesystem::path relative_path() const;

    [[nodiscard]] friend bool operator==(const ResourceId&, const ResourceId&) = default;

private:
    explicit ResourceId(std::string value);

    std::string value_;
};

struct ResourceLimits final {
    std::size_t maximum_bytes = 16U * 1024U * 1024U;
};

using ResourceBytes = std::vector<std::uint8_t>;

/** Loads confined resource bytes without imposing a text encoding. */
[[nodiscard]] ResourceBytes load_binary_resource(
    const std::filesystem::path& root,
    const ResourceId& resource,
    const ResourceLimits& limits = ResourceLimits{}
);

[[nodiscard]] std::string load_utf8_resource(
    const std::filesystem::path& root,
    const ResourceId& resource,
    const ResourceLimits& limits = ResourceLimits{}
);

} // namespace strata::resource
