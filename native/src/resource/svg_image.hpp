#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <strata/svg.hpp>

#include "base/pending.hpp"
#include "resource/image.hpp"
#include "resource/lottie.hpp"
#include "resource/runtime_images.hpp"

namespace strata::resource {

/** One parsed, surface-owned SVG image identified by the author-facing image id. */
struct SvgImageResource final {
    std::string logical_id;
    svg::Document document;
};

/** An adapter-independent input copied on the owner before preparation begins. */
struct ImageSource final {
    std::string logical_id;
    std::string host_id;
    std::string resource_id;
    TextureSampling sampling = TextureSampling::linear;
    std::vector<std::uint8_t> bytes;
};

/** One coherent owner-call view. Borrowed document and player pointers must not escape the call. */
struct ImageView final {
    std::optional<ImageDimensions> dimensions;
    const svg::Document* document = nullptr;
    ImageStream* stream = nullptr;
};

/**
 * Owner-published image resources. Workers receive owned bytes and a stateless SVG validator,
 * never this registry, its runtime image store or an adapter. Declared ids shadow runtime images
 * even while pending or failed. Its task owner and runtime store must outlive the registry.
 */
class SvgImageRegistry final {
public:
    using Validator = void (*)(const svg::Document&);
    SvgImageRegistry();
    /** Ready native construction, including dimensions-only entries used by fitting consumers. */
    explicit SvgImageRegistry(std::vector<SvgImageResource> images,
                              std::map<std::string, ImageDimensions, std::less<>> rasters = {});
    SvgImageRegistry(base::TaskOwner& tasks, std::vector<ImageSource> sources, Validator validate,
                     const RuntimeImageStore* runtime_images,
                     std::shared_ptr<const LottieLibrary> lottie);
    ~SvgImageRegistry();

    [[nodiscard]] base::ResourceResult<ImageView> lookup(std::string_view logical_id) const;
    [[nodiscard]] bool declares(std::string_view logical_id) const noexcept;
    /** Repeated runtime metadata publication is separate from pixel revisions. Call before framing. */
    void sync_metadata() const;
    [[nodiscard]] std::uint64_t texture_generation() const noexcept;
    /** Null slots are pending, failed or vector resources; binding order never follows completion. */
    [[nodiscard]] std::span<const std::shared_ptr<const TextureResource>> textures() const noexcept;
    /** The surface's Lottie animations, loaded by resource id on first use; null without a loader.
     */
    [[nodiscard]] const LottieLibrary* lottie() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace strata::resource
