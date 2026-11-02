#include "font/system_raster.hpp"

#include "font/raster.hpp"

#if defined(_WIN32)

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <utility>

#define NOMINMAX
#include <Windows.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include "base/process_lifetime.hpp"

namespace strata::font {
namespace {

using Microsoft::WRL::ComPtr;

/** Keeps a font's bytes alive for as long as DirectWrite reads them, so they are not copied. */
class BytesOwner final : public IUnknown {
  public:
    explicit BytesOwner(std::shared_ptr<const void> owner) noexcept : owner_(std::move(owner)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(const IID& id, void** const out) override {
        if (out == nullptr)
            return E_POINTER;
        if (id != __uuidof(IUnknown)) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return references_.fetch_add(1U) + 1U; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = references_.fetch_sub(1U) - 1U;
        if (left == 0U)
            delete this;
        return left;
    }

  private:
    std::atomic<ULONG> references_{1U};
    std::shared_ptr<const void> owner_;
};

/** DirectWrite's factory and faces are thread-safe, so raster lanes share them without leases. */
class DirectWrite final {
  public:
    DirectWrite() {
        ComPtr<IDWriteFactory5> factory;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                                       reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) ||
            FAILED(factory->CreateInMemoryFontFileLoader(&loader_)) ||
            FAILED(factory->RegisterFontFileLoader(loader_.Get()))) {
            return;
        }
        factory_ = std::move(factory);
        static_cast<void>(factory_.As(&variable_factory_));
        // Reads the system's settings, so once: a glyph asks for its rendering mode with these.
        static_cast<void>(factory_->CreateRenderingParams(&parameters_));
    }

    [[nodiscard]] IDWriteRenderingParams* parameters() const noexcept { return parameters_.Get(); }

    /** The face of `font`, or null when DirectWrite is too old for it or does not take it. */
    [[nodiscard]] ComPtr<IDWriteFontFace> face(const OpenTypeFont& font) {
        if (factory_ == nullptr)
            return nullptr;
        std::shared_ptr<const void> owner = font.cache_identity();
        const auto key = reinterpret_cast<std::uintptr_t>(owner.get());
        {
            const std::scoped_lock lock(mutex_);
            if (const auto found = faces_.find(key); found != faces_.end())
                return found->second.face;
        }
        ComPtr<IDWriteFontFace> created = create_face(font, owner);
        const std::scoped_lock lock(mutex_);
        // Fonts are few and live long. Holding a face holds its bytes, so cap what is kept.
        constexpr std::size_t maximum_faces = 64U;
        if (faces_.size() >= maximum_faces)
            faces_.erase(faces_.begin());
        return faces_.try_emplace(key, Entry{std::move(owner), std::move(created)}).first->second.face;
    }

    [[nodiscard]] IDWriteFactory5* factory() const noexcept { return factory_.Get(); }

  private:
    struct Entry final {
        std::shared_ptr<const void> owner;
        ComPtr<IDWriteFontFace> face;
    };

    [[nodiscard]] ComPtr<IDWriteFontFace> create_face(const OpenTypeFont& font,
                                                      const std::shared_ptr<const void>& owner) const {
        const std::span<const std::uint8_t> bytes = font.source_bytes();
        ComPtr<IUnknown> holder;
        holder.Attach(new (std::nothrow) BytesOwner(owner));
        ComPtr<IDWriteFontFile> file;
        if (holder == nullptr ||
            FAILED(loader_->CreateInMemoryFontFileReference(factory_.Get(), bytes.data(),
                                                            static_cast<UINT32>(bytes.size()),
                                                            holder.Get(), &file))) {
            return nullptr;
        }
        const std::vector<double>& variation = font.variation();
        ComPtr<IDWriteFontFace> result;
        if (variation.empty()) {
            BOOL supported = FALSE;
            DWRITE_FONT_FILE_TYPE file_type{};
            DWRITE_FONT_FACE_TYPE face_type{};
            UINT32 faces = 0U;
            if (FAILED(file->Analyze(&supported, &file_type, &face_type, &faces)) || supported == FALSE)
                return nullptr;
            IDWriteFontFile* const files[] = {file.Get()};
            if (FAILED(factory_->CreateFontFace(face_type, 1U, files, 0U, DWRITE_FONT_SIMULATIONS_NONE,
                                                &result))) {
                return nullptr;
            }
            return result;
        }
        // A variable font's instance: its axes in the file's order, as `variation` holds them.
        ComPtr<IDWriteFontResource> resource;
        if (variable_factory_ == nullptr ||
            FAILED(variable_factory_->CreateFontResource(file.Get(), 0U, &resource)) ||
            resource->GetFontAxisCount() != variation.size()) {
            return nullptr;
        }
        std::vector<DWRITE_FONT_AXIS_VALUE> axes(variation.size());
        if (FAILED(resource->GetDefaultFontAxisValues(axes.data(), static_cast<UINT32>(axes.size()))))
            return nullptr;
        for (std::size_t index = 0U; index < axes.size(); ++index)
            axes[index].value = static_cast<FLOAT>(variation[index]);
        ComPtr<IDWriteFontFace5> instance;
        if (FAILED(resource->CreateFontFace(DWRITE_FONT_SIMULATIONS_NONE, axes.data(),
                                            static_cast<UINT32>(axes.size()), &instance))) {
            return nullptr;
        }
        return instance;
    }

    ComPtr<IDWriteFactory5> factory_;
    ComPtr<IDWriteFactory6> variable_factory_;
    ComPtr<IDWriteInMemoryFontFileLoader> loader_;
    ComPtr<IDWriteRenderingParams> parameters_;
    std::mutex mutex_;
    std::map<std::uintptr_t, Entry> faces_;
};

[[nodiscard]] DirectWrite& direct_write() {
    // Raster lanes may still run while the main thread tears down.
    static base::ProcessLifetime<DirectWrite> instance;
    return instance.get();
}

} // namespace

std::optional<SystemGlyphMask>
rasterize_system_glyph(const OpenTypeFont& font, const std::uint16_t glyph,
                       const double physical_pixel_size, const double phase_offset,
                       const double italic_shear, const SubpixelOrder order) {
    DirectWrite& write = direct_write();
    const ComPtr<IDWriteFontFace> face = write.face(font);
    if (face == nullptr)
        return std::nullopt;
    const FLOAT advance = 0.0F;
    const DWRITE_GLYPH_OFFSET offset{};
    DWRITE_GLYPH_RUN run{};
    run.fontFace = face.Get();
    run.fontEmSize = static_cast<FLOAT>(physical_pixel_size);
    run.glyphCount = 1U;
    run.glyphIndices = &glyph;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;

    // What the font itself asks for at this size, as Skia asks: its `gasp` table has a say.
    IDWriteFactory5* const factory = write.factory();
    DWRITE_RENDERING_MODE mode = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
    if (write.parameters() != nullptr) {
        static_cast<void>(face->GetRecommendedRenderingMode(run.fontEmSize, 1.0F, DWRITE_MEASURING_MODE_NATURAL,
                                                            write.parameters(), &mode));
    }
    const bool subpixel = order != SubpixelOrder::none;
    const DWRITE_MATRIX shear{1.0F, 0.0F, static_cast<FLOAT>(-italic_shear), 1.0F, 0.0F, 0.0F};
    ComPtr<IDWriteGlyphRunAnalysis> analysis;
    if (FAILED(factory->CreateGlyphRunAnalysis(
            &run, italic_shear != 0.0 ? &shear : nullptr, mode, DWRITE_MEASURING_MODE_NATURAL,
            DWRITE_GRID_FIT_MODE_DEFAULT,
            subpixel ? DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE : DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
            static_cast<FLOAT>(phase_offset), 0.0F, &analysis))) {
        return std::nullopt;
    }
    const DWRITE_TEXTURE_TYPE texture =
        subpixel ? DWRITE_TEXTURE_CLEARTYPE_3x1 : DWRITE_TEXTURE_ALIASED_1x1;
    RECT bounds{};
    if (FAILED(analysis->GetAlphaTextureBounds(texture, &bounds)))
        return std::nullopt;
    SystemGlyphMask mask;
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
        return mask;
    mask.width = static_cast<std::uint32_t>(bounds.right - bounds.left);
    mask.height = static_cast<std::uint32_t>(bounds.bottom - bounds.top);
    mask.left = bounds.left;
    mask.top = -bounds.top;
    const std::size_t pixels = static_cast<std::size_t>(mask.width) * mask.height;
    std::vector<std::uint8_t> samples(pixels * (subpixel ? 3U : 1U));
    if (FAILED(analysis->CreateAlphaTexture(texture, &bounds, samples.data(),
                                            static_cast<UINT32>(samples.size())))) {
        return std::nullopt;
    }
    if (!subpixel) {
        mask.bytes = std::move(samples);
        return mask;
    }
    mask.bytes.resize(pixels * 4U);
    const bool reversed = order == SubpixelOrder::bgr;
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel) {
        const std::uint8_t* const source = samples.data() + pixel * 3U;
        std::uint8_t* const target = mask.bytes.data() + pixel * 4U;
        target[0U] = source[reversed ? 2U : 0U];
        target[1U] = source[1U];
        target[2U] = source[reversed ? 0U : 2U];
        target[3U] = std::max({source[0U], source[1U], source[2U]});
    }
    return mask;
}

} // namespace strata::font

#else

namespace strata::font {

std::optional<SystemGlyphMask> rasterize_system_glyph(const OpenTypeFont&, std::uint16_t, double, double,
                                                      double, SubpixelOrder) {
    return std::nullopt;
}

} // namespace strata::font

#endif
