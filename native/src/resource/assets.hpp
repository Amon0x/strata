#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "resource/resource.hpp"
#include "runtime/diagnostic.hpp"

namespace strata::resource {

/** Host-neutral namespaced asset identity (`namespace:path`). */
class AssetId final {
public:
    [[nodiscard]] static AssetId parse(std::string_view value);
    [[nodiscard]] const std::string& value() const noexcept;
    [[nodiscard]] std::filesystem::path relative_path() const;

private:
    explicit AssetId(std::string value, std::size_t separator);
    std::string value_;
    std::size_t separator_;
};

struct AssetLoadResult final {
    std::optional<ResourceBytes> bytes;
    std::vector<runtime::RuntimeDiagnostic> diagnostics;

    [[nodiscard]] bool loaded() const noexcept { return bytes.has_value(); }
};

/** Ordered, instance-owned filesystem resolver; platform adapters may supply zero or more roots. */
class AssetResolver final {
public:
    explicit AssetResolver(
        std::vector<std::filesystem::path> roots = {},
        runtime::DiagnosticReporter reporter = {}
    );

    [[nodiscard]] AssetLoadResult load_bytes(
        const AssetId& id,
        bool report_diagnostics = true
    ) const;

private:
    std::vector<std::filesystem::path> roots_;
    runtime::DiagnosticReporter reporter_;
};

} // namespace strata::resource
