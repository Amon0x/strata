#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace strata::runtime {

/** Logical host-space rectangle used by the platform IME candidate-window service. */
struct HostServiceRect final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    [[nodiscard]] friend bool operator==(const HostServiceRect&, const HostServiceRect&) = default;
};

struct HostClipboardRead final {
    std::uint32_t status = 0U;
    std::optional<std::string> text;
};

/**
 * Runtime-shared bridge to optional platform services.
 *
 * Clipboard fallback is deliberately retained here instead of in each InputRouter, so copy/paste
 * remains coherent across sibling surfaces even when the platform clipboard is unavailable. IME
 * ownership is also arbitrated here so an inactive sibling cannot disable the active surface.
 * Callback status zero means success; non-zero values are opaque host/ABI status codes.
 */
class HostServices final {
public:
    using InstalledProbe = std::function<bool()>;
    using ClipboardReader = std::function<HostClipboardRead()>;
    using ClipboardWriter = std::function<std::uint32_t(std::string_view)>;
    using ImeActiveSetter = std::function<std::uint32_t(bool)>;
    using ImeRectSetter = std::function<std::uint32_t(HostServiceRect)>;
    using EffectEmitter =
        std::function<std::uint32_t(std::string_view, std::string_view)>;

    HostServices(
        InstalledProbe clipboard_installed = {},
        ClipboardReader clipboard_reader = {},
        ClipboardWriter clipboard_writer = {},
        InstalledProbe ime_installed = {},
        ImeActiveSetter ime_active_setter = {},
        ImeRectSetter ime_rect_setter = {},
        InstalledProbe effects_installed = {},
        EffectEmitter effect_emitter = {}
    ) : clipboard_installed_(std::move(clipboard_installed)),
        clipboard_reader_(std::move(clipboard_reader)),
        clipboard_writer_(std::move(clipboard_writer)),
        ime_installed_(std::move(ime_installed)),
        ime_active_setter_(std::move(ime_active_setter)),
        ime_rect_setter_(std::move(ime_rect_setter)),
        effects_installed_(std::move(effects_installed)),
        effect_emitter_(std::move(effect_emitter)) {}

    [[nodiscard]] bool clipboard_installed() const noexcept {
        return probe(clipboard_installed_);
    }

    [[nodiscard]] bool ime_installed() const noexcept { return probe(ime_installed_); }

    [[nodiscard]] bool effects_installed() const noexcept {
        return probe(effects_installed_);
    }

    /** Reads the host clipboard when possible, otherwise returns the last safe local value. */
    [[nodiscard]] std::optional<std::string> read_clipboard() noexcept {
        if (clipboard_installed() && clipboard_reader_) {
            try {
                HostClipboardRead read = clipboard_reader_();
                if (read.status == 0U && read.text.has_value()) {
                    clipboard_fallback_ = std::move(*read.text);
                }
            } catch (...) {
                // Platform clipboard failures must not abort input dispatch.
            }
        }
        return clipboard_fallback_;
    }

    /** Updates the safe local value first, then mirrors it to the host on a best-effort basis. */
    [[nodiscard]] bool write_clipboard(const std::string_view text) noexcept {
        try {
            clipboard_fallback_ = std::string(text);
        } catch (...) {
            return false;
        }
        if (!clipboard_installed() || !clipboard_writer_) return true;
        try {
            return clipboard_writer_(text) == 0U;
        } catch (...) {
            return false;
        }
    }

    void set_clipboard_fallback(std::optional<std::string> text) noexcept {
        try {
            clipboard_fallback_ = std::move(text);
        } catch (...) {
            // Test/fallback injection has the same no-throw contract as host clipboard use.
        }
    }

    [[nodiscard]] std::optional<std::string_view> clipboard_fallback() const noexcept {
        return clipboard_fallback_.has_value()
            ? std::optional<std::string_view>(*clipboard_fallback_)
            : std::nullopt;
    }

    /**
     * Publishes one surface's desired IME state. A null rectangle releases only that owner;
     * another focused surface therefore cannot be disabled by an inactive sibling.
     */
    void request_ime(
        const std::string_view owner,
        const std::optional<HostServiceRect> cursor_rect
    ) noexcept {
        if (!ime_installed() || !ime_active_setter_ || !ime_rect_setter_) {
            ime_owner_.reset();
            ime_rect_.reset();
            ime_active_ = false;
            return;
        }
        try {
            if (!cursor_rect.has_value()) {
                if (!ime_owner_.has_value() || *ime_owner_ != owner) return;
                if (ime_active_setter_(false) == 0U) {
                    ime_owner_.reset();
                    ime_rect_.reset();
                    ime_active_ = false;
                }
                return;
            }
            const bool owner_changed = !ime_owner_.has_value() || *ime_owner_ != owner;
            std::optional<std::string> prepared_owner;
            if (owner_changed) prepared_owner.emplace(owner);
            if ((!ime_active_ || owner_changed) && ime_active_setter_(true) != 0U) return;
            ime_active_ = true;
            if (owner_changed) ime_owner_ = std::move(prepared_owner);
            if (!ime_rect_.has_value() || *ime_rect_ != *cursor_rect || owner_changed) {
                if (ime_rect_setter_(*cursor_rect) == 0U) ime_rect_ = *cursor_rect;
            }
        } catch (...) {
            // IME is optional. A platform exception must not escape a Surface frame.
        }
    }

    /** Adapter swaps invalidate cached delivery without inventing a Surface-level state change. */
    void adapters_changed() noexcept {
        if (ime_active_ && ime_installed() && ime_active_setter_) {
            try {
                static_cast<void>(ime_active_setter_(false));
            } catch (...) {
                // The old adapter is leaving; there is no useful recovery beyond forgetting it.
            }
        }
        ime_owner_.reset();
        ime_rect_.reset();
        ime_active_ = false;
    }

    /** Forces the owning surface to republish a logically unchanged rect after host-scale changes. */
    void invalidate_ime_geometry(const std::string_view owner) noexcept {
        if (ime_owner_.has_value() && *ime_owner_ == owner) ime_rect_.reset();
    }

    /** Explicit domain-effect contract. Visual DSL effect values never call this method. */
    [[nodiscard]] std::uint32_t emit_effect(
        const std::string_view id,
        const std::string_view payload_json
    ) noexcept {
        if (!effects_installed() || !effect_emitter_) return unavailable_status;
        try {
            return effect_emitter_(id, payload_json);
        } catch (...) {
            return callback_failure_status;
        }
    }

    static constexpr std::uint32_t unavailable_status = 12U;
    static constexpr std::uint32_t callback_failure_status = 8U;

private:
    [[nodiscard]] static bool probe(const InstalledProbe& value) noexcept {
        if (!value) return false;
        try {
            return value();
        } catch (...) {
            return false;
        }
    }

    InstalledProbe clipboard_installed_;
    ClipboardReader clipboard_reader_;
    ClipboardWriter clipboard_writer_;
    InstalledProbe ime_installed_;
    ImeActiveSetter ime_active_setter_;
    ImeRectSetter ime_rect_setter_;
    InstalledProbe effects_installed_;
    EffectEmitter effect_emitter_;
    std::optional<std::string> clipboard_fallback_{std::string{}};
    std::optional<std::string> ime_owner_;
    std::optional<HostServiceRect> ime_rect_;
    bool ime_active_ = false;
};

} // namespace strata::runtime
