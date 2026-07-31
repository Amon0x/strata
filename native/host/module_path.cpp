#include "module_path.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace strata::host {

std::string resolve_module_id(const std::string_view importer, const std::string_view import_path) {
    if (importer.empty() || import_path.empty() || import_path.front() == '/' ||
        import_path.contains('\\') || import_path.contains('\0') || import_path.contains(':')) {
        throw std::invalid_argument("module import identity is invalid");
    }
    std::vector<std::string> segments;
    const auto append = [&segments](const std::string_view path, const bool allow_parent) {
        std::size_t begin = 0U;
        while (begin <= path.size()) {
            const std::size_t separator = path.find('/', begin);
            const std::size_t end = separator == std::string_view::npos ? path.size() : separator;
            const std::string_view segment = path.substr(begin, end - begin);
            if (segment.empty()) {
                throw std::invalid_argument("module import contains an empty path segment");
            }
            if (segment == ".") {
                // Normalized away.
            } else if (segment == "..") {
                if (!allow_parent || segments.empty()) {
                    throw std::invalid_argument("module import escapes the logical resource root");
                }
                segments.pop_back();
            } else {
                segments.emplace_back(segment);
            }
            if (separator == std::string_view::npos)
                break;
            begin = separator + 1U;
        }
    };
    append(importer, false);
    if (segments.empty())
        throw std::invalid_argument("module importer has no source name");
    segments.pop_back();
    append(import_path, true);
    if (segments.empty()) {
        throw std::invalid_argument("module import resolved to an empty identity");
    }
    std::string resolved;
    for (const std::string& segment : segments) {
        if (!resolved.empty())
            resolved.push_back('/');
        resolved.append(segment);
    }
    return resolved;
}

} // namespace strata::host
