#include "resource/assets.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::resource {

AssetId::AssetId(std::string value, const std::size_t separator)
    : value_(std::move(value)), separator_(separator) {}

AssetId AssetId::parse(const std::string_view value) {
    if (value.empty() || !core::valid_utf8(value)) {
        throw std::invalid_argument("asset identity must be non-empty valid UTF-8");
    }
    const std::size_t separator = value.find(':');
    if (separator == 0U || separator == std::string_view::npos || separator + 1U == value.size() ||
        value.find(':', separator + 1U) != std::string_view::npos) {
        throw std::invalid_argument("asset identity must use namespace:path form");
    }
    const auto valid_namespace = [](const char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return std::islower(byte) != 0 || std::isdigit(byte) != 0 || character == '_' ||
               character == '-' || character == '.';
    };
    if (!std::ranges::all_of(value.substr(0U, separator), valid_namespace)) {
        throw std::invalid_argument("asset namespace contains unsupported characters");
    }
    static_cast<void>(ResourceId::parse(value.substr(separator + 1U)));
    return AssetId(std::string(value), separator);
}

const std::string& AssetId::value() const noexcept { return value_; }

std::filesystem::path AssetId::relative_path() const {
    return std::filesystem::path(value_.substr(0U, separator_)) /
           ResourceId::parse(std::string_view(value_).substr(separator_ + 1U)).relative_path();
}

AssetResolver::AssetResolver(
    std::vector<std::filesystem::path> roots,
    runtime::DiagnosticReporter reporter
) : roots_(std::move(roots)), reporter_(std::move(reporter)) {}

AssetLoadResult AssetResolver::load_bytes(
    const AssetId& id,
    const bool report_diagnostics
) const {
    for (const std::filesystem::path& root : roots_) {
        try {
            const std::filesystem::path relative = id.relative_path();
            return AssetLoadResult{
                load_binary_resource(root, ResourceId::parse(relative.generic_string())),
                {},
            };
        } catch (const ResourceError&) {
            // Ordered resolvers continue to the next source; one canonical missing result is
            // published only after every configured source declines the identity.
        }
    }
    runtime::RuntimeDiagnostic diagnostic{
        "STRATA.ASSET.RESOURCE_MISSING",
        "Resource '" + id.value() + "' was not found in configured asset sources.",
        {},
        std::nullopt,
        runtime::DiagnosticSeverity::error,
        std::nullopt,
    };
    AssetLoadResult result{{}, {diagnostic}};
    if (report_diagnostics && reporter_) reporter_(std::move(diagnostic));
    return result;
}

} // namespace strata::resource
