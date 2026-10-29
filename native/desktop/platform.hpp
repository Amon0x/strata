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

/**
 * The operating system's services for `.strata` documents on Windows: the `strata.platform`
 * requests sessions send as effects (open and save dialogs, notifications, opening files and
 * links, window controls and chrome, images on the clipboard), and files dragged in from the
 * shell.
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
     * Says `data_json` on `channel` to `client` (every session when empty), at the next pump.
     * Callable from any thread: sockets and watchers report from their own.
     */
    void post_event(std::string client, std::string channel, std::string data_json);
    /** The JSON a platform event input carries: {"channel", "data"}. */
    [[nodiscard]] static std::string event_json(const PlatformAnswer& event);
    /** Each playing video's newest frame since the last call, for the session that plays it. */
    [[nodiscard]] std::vector<std::pair<std::string, PlatformImage>> video_frames();
    /** Whether a window drag ran since the last call: the press it began with never released. */
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
