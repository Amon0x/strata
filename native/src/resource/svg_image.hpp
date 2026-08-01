#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <strata/svg.hpp>

namespace strata::resource {

/** One parsed, surface-owned SVG image identified by the author-facing image id. */
struct SvgImageResource final {
    std::string logical_id;
    svg::Document document;
};

/** Immutable lookup table used by widget presentation to project SVGs into vector render commands. */
class SvgImageRegistry final {
public:
    SvgImageRegistry() = default;
    explicit SvgImageRegistry(std::vector<SvgImageResource> images);

    [[nodiscard]] const svg::Document* find(std::string_view logical_id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::map<std::string, svg::Document, std::less<>> images_;
};

} // namespace strata::resource
