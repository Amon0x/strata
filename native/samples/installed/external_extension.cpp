#include <strata/extension.hpp>

#include <array>
#include <cstdint>
#include <memory>

namespace {

using namespace strata::extension;

void installed_subtargets(Subtargets& targets) {
    const Rect bounds = targets.bounds();
    CanvasTransform transform{bounds, Point{0.0, 0.0},
                              Point{bounds.width / 10.0, bounds.height / 10.0}};
    const Point projected = transform.project(Point{5.0, 5.0});
    static_cast<void>(targets.reserve(1U));
    static_cast<void>(targets.add(Subtarget{
        "installed.point",
        5U,
        Rect{projected.x - bounds.x - 4.0, projected.y - bounds.y - 4.0, 8.0, 8.0},
        1,
        true,
        true,
    }));
}

void installed_present(Present& present) {
    constexpr Color white = rgba(255U, 255U, 255U);
    constexpr std::array<MeshVertex, 4U> vertices{
        MeshVertex{0.0, 0.0, 0.0, 0.0, 0.0, white},
        MeshVertex{1.0, 0.0, 0.0, 1.0, 0.0, white},
        MeshVertex{1.0, 1.0, 0.0, 1.0, 1.0, white},
        MeshVertex{0.0, 1.0, 0.0, 0.0, 1.0, white},
    };
    constexpr std::array<std::uint32_t, 6U> indices{0U, 1U, 2U, 0U, 2U, 3U};
    MeshBatch<4U, 6U> batch;
    static_cast<void>(batch.append(vertices, indices));
    present.mesh(present.bounds(), "installed.canvas", batch.geometry());
}

std::unique_ptr<strata::extension::Package> installed_package() {
    auto created = strata::extension::package("installed.smoke.v1");
    created->widget(widget("InstalledExternalWidget")
                        .no_children()
                        .intrinsic_size(32.0, 16.0)
                        .subtargets(&installed_subtargets)
                        .present(&installed_present));
    return created;
}

} // namespace

STRATA_EXTENSION_PACKAGE(installed_package)
