#include "font/registry.hpp"

#include <stdexcept>
#include <utility>

#include "base/utf8.hpp"

namespace strata::font {

FontRegistry::FontRegistry(base::DiagnosticReporter reporter)
    : reporter_(std::move(reporter)) {}

std::shared_ptr<const OpenTypeFont> FontRegistry::load_font(
    std::string id,
    resource::ResourceBytes bytes,
    std::string source_id
) {
    if (id.empty() || source_id.empty() || !base::valid_utf8(id) || !base::valid_utf8(source_id)) {
        throw std::invalid_argument("font id and source id must be non-empty valid UTF-8");
    }
    try {
        auto font = std::make_shared<const OpenTypeFont>(OpenTypeFont::parse(std::move(bytes)));
        fonts_.insert_or_assign(std::move(id), font);
        return font;
    } catch (const FontError&) {
        if (reporter_) {
            reporter_(base::RuntimeDiagnostic{
                "STRATA.FONT.REGISTRATION_FAILED",
                "Font '" + id + "' could not be registered because '" + source_id +
                    "' did not parse as a valid OpenType font.",
                {},
                std::nullopt,
                base::DiagnosticSeverity::warning,
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
