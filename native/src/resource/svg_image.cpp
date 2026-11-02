#include "resource/svg_image.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "base/utf8.hpp"
#include "resource/image_stream.hpp"

namespace strata::resource {
namespace {

/** A validated document carries natural dimensions so publication cannot fail their conversion. */
struct PreparedSvg final {
    svg::Document document;
    std::optional<ImageDimensions> dimensions;
};

using PreparedImage = std::variant<PreparedSvg, ImagePreparation>;

[[nodiscard]] std::optional<ImageDimensions> natural_size(const svg::Document& document) {
    const double width = document.width > 0.0 ? document.width : document.view_box.width;
    const double height = document.height > 0.0 ? document.height : document.view_box.height;
    if (width > 0.0 && height > 0.0) {
        if (!std::isfinite(width) || !std::isfinite(height) ||
            width > std::numeric_limits<std::uint32_t>::max() ||
            height > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("SVG natural dimensions exceed the image coordinate range");
        return ImageDimensions{static_cast<std::uint32_t>(std::ceil(width)),
                               static_cast<std::uint32_t>(std::ceil(height))};
    }
    return std::nullopt;
}

/** Parses only owned input; geometry validation is supplied by the consuming UI layer. */
[[nodiscard]] PreparedImage prepare(std::vector<std::uint8_t> bytes,
                                    const std::string& resource_id,
                                    const SvgImageRegistry::Validator validate) {
    if (sniff_image_format(bytes).has_value())
        return prepare_image(std::move(bytes));
    const std::string_view source(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!base::valid_utf8(source)) {
        throw std::invalid_argument("image resource '" + resource_id +
                                    "' is neither a PNG, JPEG, GIF or WebP image nor UTF-8 SVG");
    }
    try {
        auto document = svg::parse(source);
        validate(document);
        const auto dimensions = natural_size(document);
        return PreparedSvg{std::move(document), dimensions};
    } catch (const svg::ParseError& error) {
        throw std::invalid_argument("SVG image resource '" + resource_id +
                                    "' is invalid at byte " +
                                    std::to_string(error.byte_offset()) + ": " + error.what());
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument("SVG image resource '" + resource_id +
                                    "' exceeds the supported UI geometry budget: " + error.what());
    }
}

} // namespace

/** Serialized publication state. Completion callbacks borrow it weakly; workers never capture it. */
struct SvgImageRegistry::State final {
    struct Entry final {
        ~Entry() { pending.cancel(); }
        TextureResource texture;
        std::optional<svg::Document> document;
        ImageView view;
        base::Pending<void> pending;
        bool ready = false;
    };
    struct RuntimeRead final {
        RuntimeRead(base::TaskOwner& tasks, std::string id,
                    const std::optional<ImageDimensions> size)
            : revision(tasks, base::TaskKind::image, std::move(id)), dimensions(size) {}
        base::ResourceRevision revision;
        std::optional<ImageDimensions> dimensions;
    };

    [[nodiscard]] std::optional<ImageDimensions> runtime_size(const std::string_view id) const {
        if (runtime_images != nullptr) {
            const auto found = runtime_images->images().find(id);
            if (found != runtime_images->images().end())
                return found->second->dimensions;
        }
        return std::nullopt;
    }

    void adopt(const std::shared_ptr<Entry>& entry, const std::size_t slot, PreparedImage prepared) {
        if (auto* vector = std::get_if<PreparedSvg>(&prepared); vector != nullptr) {
            entry->document.emplace(std::move(vector->document));
            entry->view.document = &*entry->document;
            entry->view.dimensions = vector->dimensions;
        } else {
            auto& raster = std::get<ImagePreparation>(prepared);
            entry->texture.dimensions = raster.dimensions;
            entry->view.dimensions = raster.dimensions;
            if (raster.continuation.has_value()) {
                entry->texture.stream = std::make_shared<ImageStream>(
                    std::move(raster), *tasks, entry->texture.logical_id);
                entry->view.stream = entry->texture.stream.get();
            } else {
                entry->texture.pixels = std::move(raster.first.rgba);
            }
            // An alias keeps the entry's immutable texture alive across packet residency changes.
            textures[slot] = std::shared_ptr<const TextureResource>(entry, &entry->texture);
            ++texture_generation;
        }
        entry->ready = true;
    }

    base::TaskOwner* tasks = nullptr;
    const RuntimeImageStore* runtime_images = nullptr;
    std::shared_ptr<const LottieLibrary> lottie;
    std::map<std::string, std::shared_ptr<Entry>, std::less<>> entries;
    std::map<std::string, RuntimeRead, std::less<>> runtime_reads;
    std::vector<std::shared_ptr<const TextureResource>> textures;
    std::uint64_t texture_generation = 0U;
    std::uint64_t runtime_generation = 0U;
    bool prune_reads = false;
};

SvgImageRegistry::SvgImageRegistry() : state_(std::make_shared<State>()) {}

SvgImageRegistry::SvgImageRegistry(std::vector<SvgImageResource> images,
                                   std::map<std::string, ImageDimensions, std::less<>> rasters)
    : SvgImageRegistry() {
    for (auto& image : images) {
        auto entry = std::make_shared<State::Entry>();
        entry->document.emplace(std::move(image.document));
        entry->view.document = &*entry->document;
        entry->view.dimensions = natural_size(*entry->document);
        entry->ready = true;
        if (image.logical_id.empty() ||
            !state_->entries.emplace(std::move(image.logical_id), std::move(entry)).second)
            throw std::invalid_argument("image logical ids must be non-empty and unique");
    }
    for (auto& [id, dimensions] : rasters) {
        auto entry = std::make_shared<State::Entry>();
        entry->view.dimensions = dimensions;
        entry->ready = true;
        if (id.empty() || !state_->entries.emplace(id, std::move(entry)).second)
            throw std::invalid_argument("image logical ids must be non-empty and unique");
    }
}

SvgImageRegistry::SvgImageRegistry(base::TaskOwner& tasks, std::vector<ImageSource> sources,
                                   const Validator validate, const RuntimeImageStore* runtime_images,
                                   std::shared_ptr<const LottieLibrary> lottie)
    : SvgImageRegistry() {
    if (validate == nullptr)
        throw std::invalid_argument("image preparation requires SVG geometry validation");
    state_->tasks = &tasks;
    state_->runtime_images = runtime_images;
    state_->lottie = std::move(lottie);
    state_->textures.resize(sources.size());
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        auto& source = sources[index];
        auto entry = std::make_shared<State::Entry>();
        entry->texture.logical_id = std::move(source.logical_id);
        entry->texture.host_id = std::move(source.host_id);
        entry->texture.sampling = source.sampling;
        if (entry->texture.logical_id.empty() || entry->texture.host_id.empty() ||
            !state_->entries.emplace(entry->texture.logical_id, entry).second)
            throw std::invalid_argument("image bindings require unique logical ids and host ids");
        if (tasks.execution() == base::TaskExecution::deterministic) {
            state_->adopt(entry, index, prepare(std::move(source.bytes), source.resource_id, validate));
            continue;
        }
        auto result = std::make_shared<std::optional<PreparedImage>>();
        entry->pending = base::Pending<void>::start(
            tasks, base::TaskKind::image, base::TaskPriority::frame_critical,
            entry->texture.logical_id,
            [bytes = std::move(source.bytes), id = std::move(source.resource_id), validate,
             result](const base::TaskCancellation&) mutable {
                result->emplace(prepare(std::move(bytes), id, validate));
            },
            [weak = std::weak_ptr<State>(state_), target = std::weak_ptr<State::Entry>(entry),
             result, index](const base::TaskOutcome& outcome) {
                const auto owner = weak.lock();
                const auto image = target.lock();
                if (owner != nullptr && image != nullptr && outcome.error == nullptr)
                    owner->adopt(image, index, std::move(result->value()));
            });
    }
}

SvgImageRegistry::~SvgImageRegistry() = default;

base::ResourceResult<ImageView> SvgImageRegistry::lookup(const std::string_view logical_id) const {
    if (const auto found = state_->entries.find(logical_id); found != state_->entries.end()) {
        const auto& entry = *found->second;
        return {entry.ready ? std::optional(entry.view) : std::nullopt, entry.pending.readiness()};
    }
    if (state_->runtime_images == nullptr)
        return {};
    const auto dimensions = state_->runtime_size(logical_id);
    auto read = state_->runtime_reads.find(logical_id);
    if (read == state_->runtime_reads.end()) {
        read = state_->runtime_reads.try_emplace(
            std::string(logical_id), *state_->tasks, std::string(logical_id), dimensions).first;
        state_->prune_reads = true;
    }
    return {dimensions.has_value() ? std::optional(ImageView{dimensions}) : std::nullopt,
            read->second.revision.readiness()};
}

bool SvgImageRegistry::declares(const std::string_view logical_id) const noexcept {
    return state_->entries.contains(logical_id);
}

void SvgImageRegistry::sync_metadata() const {
    if (state_->runtime_images == nullptr)
        return;
    const auto generation = state_->runtime_images->metadata_generation();
    const bool changed = state_->runtime_generation != generation;
    if (!changed && !state_->prune_reads)
        return;
    state_->runtime_generation = generation;
    state_->prune_reads = false;
    for (auto current = state_->runtime_reads.begin(); current != state_->runtime_reads.end();) {
        auto& read = current->second;
        if (!read.revision.has_readers()) {
            current = state_->runtime_reads.erase(current);
            continue;
        }
        if (changed) {
            const auto dimensions = state_->runtime_size(current->first);
            if (read.dimensions != dimensions) {
                read.dimensions = dimensions;
                read.revision.publish();
            }
        }
        ++current;
    }
}

std::uint64_t SvgImageRegistry::texture_generation() const noexcept {
    return state_->texture_generation;
}

std::span<const std::shared_ptr<const TextureResource>> SvgImageRegistry::textures() const noexcept {
    return state_->textures;
}

const LottieLibrary* SvgImageRegistry::lottie() const noexcept {
    return state_->lottie.get();
}

} // namespace strata::resource
