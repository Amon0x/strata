#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <strata/desktop.hpp>
#include <strata/strata.h>

struct HWND__;
using HWND = HWND__*;

namespace strata::desktop {

/** An image a platform answer brings with it (one pasted from the clipboard), to publish first. */
struct PlatformImage final {
    std::string id;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    /** Straight-alpha RGBA8 rows. */
    std::vector<std::uint8_t> rgba;
};

/**
 * The answer to one platform request, for the session that asked; or, with a `channel`, something
 * said unasked on that channel (to the named session, or every session when `client` is empty),
 * `json` being its data.
 */
struct PlatformAnswer final {
    std::string client;
    std::uint64_t request = 0U;
    std::string json;
    std::optional<PlatformImage> image;
    std::optional<std::string> channel;
};

/** The look the user chose for the operating system (env.systemDark, env.systemAccent). */
struct SystemAppearance final {
    /** The app theme is dark. */
    bool dark = false;
    /** The accent colour, when the system says. */
    std::optional<strata_color> accent;
    /** How the system antialiases text (ClearType and its subpixel order), STRATA_TEXT_ANTIALIASING_*. */
    std::uint32_t text_antialiasing = STRATA_TEXT_ANTIALIASING_GRAYSCALE;

    [[nodiscard]] friend bool operator==(const SystemAppearance& left,
                                         const SystemAppearance& right) noexcept {
        const auto same_accent = [&] {
            if (left.accent.has_value() != right.accent.has_value())
                return false;
            return !left.accent.has_value() || (left.accent->red == right.accent->red &&
                                                left.accent->green == right.accent->green &&
                                                left.accent->blue == right.accent->blue &&
                                                left.accent->alpha == right.accent->alpha);
        };
        return left.dark == right.dark && same_accent() &&
               left.text_antialiasing == right.text_antialiasing;
    }
    /** Writes the appearance into a Surface environment. */
    void apply(strata_surface_environment& environment) const noexcept {
        environment.system_appearance = (dark ? STRATA_SYSTEM_APPEARANCE_DARK : 0U) |
                                        (accent.has_value() ? STRATA_SYSTEM_APPEARANCE_ACCENT : 0U);
        environment.system_accent = accent.value_or(strata_color{});
        environment.text_antialiasing = text_antialiasing;
    }
};

/**
 * The operating system's services for `.strata` documents on Windows: the `strata.platform`
 * requests sessions send as effects (open and save dialogs, notifications, opening files and
 * links, window controls and chrome, images on the clipboard, video and sound, HTTP requests and
 * WebSockets, reading, writing and watching files), and files dragged in from the shell.
 *
 * Requests arrive inside a Surface call, so they are only queued there; `pump` runs them after
 * the frame, where a dialog may block, and returns the answers to deliver as
 * STRATA_INPUT_PLATFORM_RESULT input.
 */
class PlatformServices final {
  public:
    using ResourceReader = std::function<std::vector<std::uint8_t>(std::string_view)>;
    /** A resource id's file, for media the system reads itself. */
    using ResourcePath = std::function<std::wstring(std::string_view)>;
    /** Files over the window: an input kind (FILE_DRAG, ...), client pixels, paths one per line. */
    using FileSink = std::function<void(std::uint32_t kind, double x, double y, std::string paths)>;

    PlatformServices(HWND window, ResourceReader read_resource, ResourcePath resource_path = {});
    ~PlatformServices();

    PlatformServices(const PlatformServices&) = delete;
    PlatformServices& operator=(const PlatformServices&) = delete;

    /** The effect adapter for one session; answers to its requests name `client`. */
    [[nodiscard]] strata_effect_adapter effect_adapter(std::string client);
    /** Runs the requests queued since the last call and returns their answers. */
    [[nodiscard]] std::vector<PlatformAnswer> pump();
    /**
     * Forgets a session that has closed: its requests in flight are cancelled and never answered,
     * and its sockets, watches, sounds and videos stop. A session opened later under the same id
     * starts afresh.
     */
    void close_client(std::string_view client) noexcept;
    /**
     * Says `data_json` on `channel` to `client` (every session when empty), at the next pump.
     * Callable from any thread: sockets and watchers report from their own.
     */
    void post_event(std::string client, std::string channel, std::string data_json);
    /** Answers request `request` of `client` with `json`, at the next pump. Callable from any
     * thread. */
    void answer(std::string client, std::uint64_t request, std::string json);
    /** The windows documents asked to open, or to close by id, since the last call. */
    [[nodiscard]] std::vector<WindowRequest> take_window_requests();
    /** The system's look as last read; it changes when the user changes it. */
    [[nodiscard]] const SystemAppearance& appearance() const noexcept;
    /** Whether the system's look changed since the last call, so environments must follow. */
    [[nodiscard]] bool take_appearance_changed() noexcept;
    /** Whether a `node.capture` waits for the next presented frame's pixels (frame_presented). */
    [[nodiscard]] bool wants_frame() const noexcept;
    /**
     * The pixels of the frame just presented (straight RGBA8 rows, top first): the captures that
     * waited for it are cut from it and written; their answers come at the next pump.
     */
    void frame_presented(std::uint32_t width, std::uint32_t height,
                         std::span<const std::uint8_t> rgba);
    /** The JSON a platform event input carries: {"channel", "data"}. */
    [[nodiscard]] static std::string event_json(const PlatformAnswer& event);
    /** Each playing video's newest frame since the last call, for the session that plays it. */
    [[nodiscard]] std::vector<std::pair<std::string, PlatformImage>> video_frames();
    /**
     * Pauses a playing video once nothing shows it any more (`shown` asks the session `client`
     * whether a node names its image), as a browser pauses a video taken out of the page, and
     * plays it on when something shows it again. A video never shown yet, or one the app paused
     * itself, is left as it is.
     */
    void follow_video_use(
        const std::function<bool(std::string_view client, std::string_view image)>& shown);
    /**
     * Whether a modal loop (a window drag, a drag out) ran since the last call: the press it began
     * with never released, so the host cancels its interactions.
     */
    [[nodiscard]] bool take_window_moved() noexcept;
    /** Accepts files dragged in from the shell, reporting them to `sink`. */
    void accept_files(FileSink sink);
    /** Whether the window shows a system backdrop, so frames clear to transparent. */
    [[nodiscard]] bool transparent() const noexcept;
    /** Window messages the platform answers itself (custom chrome, the tray icon); else nothing. */
    [[nodiscard]] std::optional<std::intptr_t>
    window_message(std::uint32_t message, std::uintptr_t word, std::intptr_t long_value);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::desktop
