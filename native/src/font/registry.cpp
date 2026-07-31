#include "font/registry.hpp"

#include <stdexcept>
#include <utility>

#include "core/utf8.hpp"

namespace strata::font {

FontRegistry::FontRegistry(runtime::DiagnosticReporter reporter)
    : reporter_(std::move(reporter)) {}

std::shared_ptr<const OpenTypeFont> FontRegistry::load_font(
    std::string id,
    resource::ResourceBytes bytes,
    std::string source_id
) {
    if (id.empty() || source_id.empty() || !core::valid_utf8(id) || !core::valid_utf8(source_id)) {
        throw std::invalid_argument("font id and source id must be non-empty valid UTF-8");
    }
    try {
        auto font = std::make_shared<const OpenTypeFont>(OpenTypeFont::parse(std::move(bytes)));
        fonts_.insert_or_assign(std::move(id), font);
        return font;
    } catch (const FontError&) {
        if (reporter_) {
            reporter_(runtime::RuntimeDiagnostic{
                "STRATA.FONT.REGISTRATION_FAILED",
                "Font '" + id + "' could not be registered because '" + source_id +
                    "' did not parse as a valid OpenType font.",
                {},
                std::nullopt,
                runtime::DiagnosticSeverity::warning,
                std::nullopt,
            });
        }
        return {};
    }
}

std::shared_ptr<const OpenTypeFont> FontRegistry::find(const std::string_view id) const noexcept {
    const auto found = fonts_.find(id);
    return found != fonts_.end() ? found->second : std::shared_ptr<const OpenTypeFont>{};
}

} // namespace strata::font
