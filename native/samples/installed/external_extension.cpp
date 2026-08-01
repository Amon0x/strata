#include <strata/extension.hpp>

#include <memory>

namespace {

std::unique_ptr<strata::extension::Package> installed_package() {
    auto created = strata::extension::package("installed.smoke.v1");
    created->widget(
        strata::extension::widget("InstalledExternalWidget")
            .no_children()
            .intrinsic_size(32.0, 16.0)
    );
    return created;
}

} // namespace

STRATA_EXTENSION_PACKAGE(installed_package)
