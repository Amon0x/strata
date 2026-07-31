#include "extensions.hpp"

#include <set>
#include <stdexcept>
#include <string>

#include <strata/extension.hpp>

namespace strata::host {

const strata_surface_extension_bundle* SelectedExtensions::pointer() noexcept {
    if (widgets.empty() && behaviors.empty()) return nullptr;
    bundle = strata_surface_extension_bundle{
        sizeof(strata_surface_extension_bundle),
        widgets.empty() ? nullptr : widgets.data(),
        widgets.size(),
        behaviors.empty() ? nullptr : behaviors.data(),
        behaviors.size(),
    };
    return &bundle;
}

std::vector<std::string> package_schemas(const std::vector<std::string>& package_ids) {
    std::vector<std::string> documents;
    documents.reserve(package_ids.size());
    std::set<std::string, std::less<>> seen;
    for (const std::string& id : package_ids) {
        if (id.empty() || !seen.insert(id).second) {
            throw std::invalid_argument("native extension package ids must be non-empty and unique");
        }
        documents.push_back(extension::Registry::instance().require(id).schema_json());
    }
    return documents;
}

SelectedExtensions select_extensions(const std::vector<std::string>& package_ids) {
    SelectedExtensions result;
    std::set<std::string, std::less<>> seen;
    for (const std::string& id : package_ids) {
        if (id.empty() || !seen.insert(id).second) {
            throw std::invalid_argument("native extension package ids must be non-empty and unique");
        }
        const strata_surface_extension_bundle& package =
            extension::Registry::instance().require(id).bundle();
        result.widgets.insert(
            result.widgets.end(),
            package.widgets,
            package.widgets + package.widget_count
        );
        result.behaviors.insert(
            result.behaviors.end(),
            package.behaviors,
            package.behaviors + package.behavior_count
        );
    }
    return result;
}

} // namespace strata::host
