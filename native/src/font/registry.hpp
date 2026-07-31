#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "font/opentype.hpp"
#include "runtime/diagnostic.hpp"

namespace strata::font {

/**
 * Per-runtime logical font registry. Immutable parsed faces are content-deduplicated process-wide;
 * logical ids, diagnostics, resource generations, and replacement policy remain runtime-local.
 */
class FontRegistry final {
public:
    explicit FontRegistry(runtime::DiagnosticReporter reporter = {});

    [[nodiscard]] std::shared_ptr<const OpenTypeFont> load_font(
        std::string id,
        resource::ResourceBytes bytes,
        std::string source_id
    );
    [[nodiscard]] std::shared_ptr<const OpenTypeFont> find(std::string_view id) const noexcept;

private:
    runtime::DiagnosticReporter reporter_;
    std::map<std::string, std::shared_ptr<const OpenTypeFont>, std::less<>> fonts_;
};

} // namespace strata::font
