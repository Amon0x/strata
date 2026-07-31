#pragma once

#include <memory>

#include <strata/extension.hpp>

namespace strata::extension {

/** Showcase package proving the public authoring path: two widgets and one behavior. */
[[nodiscard]] std::unique_ptr<Package> demo_package();

} // namespace strata::extension
