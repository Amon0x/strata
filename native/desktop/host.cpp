#include "host.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <strata/extension.hpp>
#include <strata/host.hpp>
#include <strata/strata.hpp>

#include "host_services.hpp"
#include <strata/render_packet.hpp>
#include "renderer.hpp"
#include "showcase.hpp"

namespace strata::desktop {

using host::RenderPacket;
using host::RenderPacketDecoder;

namespace {

[[nodiscard]] std::string copy(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] double desktop_display_scale(
    const std::uint32_t framebuffer_width,
    const std::uint32_t framebuffer_height,
    const double dpi_scale
) {
    strata_scale_policy_config policy{sizeof(strata_scale_policy_config)};
    strata::require_ok(
        strata_scale_policy_defaults(STRATA_SCALE_POLICY_AUTO_FIT, &policy),
        "desktop scale-policy defaults"
    );
    // Desktop windows need more working room than a full-screen game surface. Auto-fit around a
    // 1600x900 logical viewport, never shrinking below the scale requested by the monitor DPI.
    policy.preferred_logical_width = 1'600.0;
    policy.preferred_logical_height = 900.0;
    policy.min_scale = std::min(dpi_scale, policy.max_scale);
    strata_scale_context context{sizeof(strata_scale_context)};
    strata::require_ok(
        strata_resolve_scale_context(
            &policy,
            static_cast<std::int64_t>(framebuffer_width),
            static_cast<std::int64_t>(framebuffer_height),
            &context
        ),
        "desktop scale-context resolution"
    );
    return context.scale;
}

[[nodiscard]] std::uint32_t current_modifiers() noexcept {
    std::uint32_t result = 0U;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) result |= STRATA_KEY_MODIFIER_SHIFT;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) result |= STRATA_KEY_MODIFIER_CONTROL;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) result |= STRATA_KEY_MODIFIER_ALT;
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0) {
        result |= STRATA_KEY_MODIFIER_SUPER;
    }
    return result;
}

[[nodiscard]] std::string key_name(const std::uint32_t key) {
    switch (key) {
    case VK_TAB: return "tab";
    case VK_RETURN: return "enter";
    case VK_SPACE: return "space";
    case VK_ESCAPE: return "escape";
    case VK_BACK: return "backspace";
    case VK_DELETE: return "delete";
    case VK_LEFT: return "left";
    case VK_RIGHT: return "right";
    case VK_UP: return "up";
    case VK_DOWN: return "down";
    case VK_HOME: return "home";
    case VK_END: return "end";
    case VK_PRIOR: return "page_up";
    case VK_NEXT: return "page_down";
    case VK_INSERT: return "insert";
    default:
        if (key >= 'A' && key <= 'Z') return std::string(1U, static_cast<char>('a' + key - 'A'));
        if (key >= '0' && key <= '9') return std::string(1U, static_cast<char>(key));
        return "win32:" + std::to_string(key);
    }
}

[[nodiscard]] std::string json_string(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    constexpr char hexadecimal[] = "0123456789abcdef";
    for (const char byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20U) {
                result += "\\u00";
                result.push_back(hexadecimal[character >> 4U]);
                result.push_back(hexadecimal[character & 0x0FU]);
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
    }
    result.push_back('"');
    return result;
}

template <typename Byte>
[[nodiscard]] std::uint64_t fingerprint(const std::span<const Byte> bytes) noexcept {
    std::uint64_t result = 14695981039346656037ULL;
    for (const Byte byte : bytes) {
        result ^= static_cast<std::uint8_t>(byte);
        result *= 1099511628211ULL;
    }
    return result;
}

} // namespace

struct Host::Impl final {
    struct ProfileSection final {
        std::string path;
        std::uint64_t last_sample_frame_index = 0U;
        std::int64_t last_nanos = 0;
        std::int64_t p95_nanos = 0;
        std::int64_t p99_nanos = 0;
        std::int64_t maximum_nanos = 0;
    };

    struct ProfileCounter final {
        std::string name;
        std::uint64_t value = 0U;
    };

    struct ProfileSnapshot final {
        std::string scope_id;
        std::uint64_t frame_index = 0U;
        std::vector<ProfileSection> sections;
        std::vector<ProfileCounter> counters;
    };

    struct FrameBucket final {
        std::int64_t index = 0;
        std::int64_t maximum_total_nanos = 0;
        std::int64_t maximum_native_nanos = 0;
        std::uint64_t total_nanos = 0U;
        std::uint64_t frame_count = 0U;
    };

    struct TimingWindow final {
        void add(const std::int64_t nanos) {
            samples.push_back(std::max<std::int64_t>(0, nanos));
            if (samples.size() > 120U) samples.pop_front();
        }

        [[nodiscard]] std::int64_t last() const noexcept {
            return samples.empty() ? 0 : samples.back();
        }

        [[nodiscard]] std::int64_t p95() const {
            if (samples.empty()) return 0;
            std::vector<std::int64_t> ordered(samples.begin(), samples.end());
            const std::size_t index = (ordered.size() * 95U + 99U) / 100U - 1U;
            std::ranges::nth_element(ordered, ordered.begin() + index);
            return ordered[index];
        }

        [[nodiscard]] std::int64_t maximum() const noexcept {
            return samples.empty() ? 0 : *std::ranges::max_element(samples);
        }

        std::deque<std::int64_t> samples;
    };

    struct TimingSummary final {
        std::int64_t last_nanos = 0;
        std::int64_t p95_nanos = 0;
        std::int64_t maximum_nanos = 0;
    };

    struct ProfileCapture final {
        std::int64_t captured_at = 0;
        ProfileSnapshot runtime;
        ProfileSnapshot surface;
        std::array<TimingSummary, 5U> host;
    };

    class DurableWriter final {
    public:
        DurableWriter() : worker_([this] { run(); }) {}
        DurableWriter(const DurableWriter&) = delete;
        DurableWriter& operator=(const DurableWriter&) = delete;
        ~DurableWriter() {
            {
                std::scoped_lock lock(mutex_);
                stopping_ = true;
            }
            changed_.notify_all();
        }

        void enqueue(std::filesystem::path path, std::string payload) {
            std::scoped_lock lock(mutex_);
            if (failure_ != nullptr) std::rethrow_exception(failure_);
            pending_.insert_or_assign(
                std::move(path), Pending{++version_, std::move(payload)}
            );
            changed_.notify_one();
        }

        [[nodiscard]] std::optional<std::string> pending(
            const std::filesystem::path& path
        ) const {
            std::scoped_lock lock(mutex_);
            if (failure_ != nullptr) std::rethrow_exception(failure_);
            const auto found = pending_.find(path);
            return found != pending_.end()
                ? std::optional<std::string>(found->second.payload) : std::nullopt;
        }

        void flush() {
            std::unique_lock lock(mutex_);
            settled_.wait(lock, [this] { return pending_.empty() || failure_ != nullptr; });
            if (failure_ != nullptr) std::rethrow_exception(failure_);
        }

    private:
        struct Pending final {
            std::uint64_t version = 0U;
            std::string payload;
        };

        static void write(const std::filesystem::path& path, const std::string_view payload) {
            std::filesystem::create_directories(path.parent_path());
            const std::filesystem::path temporary = path.wstring() + L".tmp";
            {
                std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
                output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
                output.flush();
                if (!output) throw std::runtime_error("desktop durable write failed");
            }
            if (!MoveFileExW(
                    temporary.c_str(),
                    path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                )) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                throw std::runtime_error("desktop durable atomic replace failed");
            }
        }

        void run() noexcept {
            for (;;) {
                std::filesystem::path path;
                Pending current;
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                    if (pending_.empty() && stopping_) return;
                    const auto& [next_path, next] = *pending_.begin();
                    path = next_path;
                    current = next;
                }
                try {
                    write(path, current.payload);
                } catch (...) {
                    std::scoped_lock lock(mutex_);
                    failure_ = std::current_exception();
                    pending_.clear();
                    settled_.notify_all();
                    return;
                }
                {
                    std::scoped_lock lock(mutex_);
                    const auto found = pending_.find(path);
                    if (found != pending_.end() && found->second.version == current.version) {
                        pending_.erase(found);
                    }
                    if (pending_.empty()) settled_.notify_all();
                }
            }
        }

        mutable std::mutex mutex_;
        std::condition_variable changed_;
        std::condition_variable settled_;
        std::map<std::filesystem::path, Pending> pending_;
        std::exception_ptr failure_;
        std::uint64_t version_ = 0U;
        bool stopping_ = false;
        std::jthread worker_;
    };

    struct AsyncBridge final {
        struct Pending final {
            std::uint64_t id = 0U;
            std::string binding;
            std::string payload;
            std::int64_t started_nanos = 0;
            bool progress_published = false;
        };

        Impl* owner = nullptr;
        strata_runtime* runtime = nullptr;
        std::map<std::uint64_t, Pending> pending;
    };

    struct Session final {
        std::string id;
        std::unique_ptr<strata::Runtime> runtime;
        std::unique_ptr<AsyncBridge> async;
        std::unique_ptr<strata::host::Bindings> bindings;
        std::optional<strata::Surface> surface;
        RenderPacketDecoder decoder;
    };

    Impl(
        HWND window,
        std::filesystem::path resource_root,
        std::string instance_label,
        const HostOptions options
    )
        : window(window),
          services(window, std::move(resource_root)),
          renderer(window, options.vsync),
          instance_label(std::move(instance_label)),
          showcase_model(this->instance_label) {
        if (this->instance_label.empty()) {
            throw std::invalid_argument("desktop instance label must not be empty");
        }
        RECT client{};
        if (!GetClientRect(window, &client)) throw std::runtime_error("could not read desktop client area");
        framebuffer_width = static_cast<std::uint32_t>(std::max<LONG>(1L, client.right - client.left));
        framebuffer_height = static_cast<std::uint32_t>(std::max<LONG>(1L, client.bottom - client.top));
        const UINT dpi = GetDpiForWindow(window);
        const double dpi_scale = dpi == 0U ? 1.0 : static_cast<double>(dpi) / 96.0;
        display_scale = desktop_display_scale(
            framebuffer_width, framebuffer_height, dpi_scale
        );
        renderer.resize(
            framebuffer_width,
            framebuffer_height,
            logical_width(),
            logical_height()
        );
        performance_hud_enabled = options.performance_hud;
        profile_sampling_enabled = options.profile_sampling;
        if (options.performance_hud) create_performance_hud();
        if (options.restore_window_geometry) restore_window_geometry();
        RECT restored_client{};
        if (GetClientRect(window, &restored_client)) {
            framebuffer_width = static_cast<std::uint32_t>(
                std::max<LONG>(1L, restored_client.right - restored_client.left)
            );
            framebuffer_height = static_cast<std::uint32_t>(
                std::max<LONG>(1L, restored_client.bottom - restored_client.top)
            );
            display_scale = desktop_display_scale(
                framebuffer_width, framebuffer_height, dpi_scale
            );
            renderer.resize(
                framebuffer_width, framebuffer_height, logical_width(), logical_height()
            );
        }
    }

    ~Impl() noexcept {
        try {
            remember_window_geometry();
        } catch (...) {
        }
        close_session(performance_hud);
        close_session(debug);
        close_session(settings);
        close_session(showcase);
        try {
            durable_writer.flush();
        } catch (...) {
        }
    }

    [[nodiscard]] double logical_width() const noexcept {
        return static_cast<double>(framebuffer_width) / display_scale;
    }

    [[nodiscard]] double logical_height() const noexcept {
        return static_cast<double>(framebuffer_height) / display_scale;
    }

    [[nodiscard]] std::int64_t now() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - epoch
        ).count();
    }

    static std::int64_t clock(void* const user_data) noexcept {
        return static_cast<Impl*>(user_data)->frame_time;
    }

    [[nodiscard]] std::filesystem::path durable_path(
        const std::string_view application_id
    ) const {
        wchar_t* local = nullptr;
        std::size_t local_size = 0U;
        const errno_t environment_status = _wdupenv_s(
            &local, &local_size, L"LOCALAPPDATA"
        );
        const std::filesystem::path root = environment_status == 0 && local != nullptr
            ? std::filesystem::path(local)
            : std::filesystem::temp_directory_path();
        std::free(local);
        const std::string identity = instance_label + ":" + std::string(application_id);
        std::ostringstream name;
        name << std::hex << fingerprint(std::span(identity));
        return root / "Strata" / "durable" / (name.str() + ".json");
    }

    static strata_status durable_load(
        void* const user_data,
        const strata_string_view application_id,
        strata_bytes_view* const out_bytes
    ) noexcept {
        if (user_data == nullptr || out_bytes == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            auto& self = *static_cast<Impl*>(user_data);
            const std::filesystem::path path = self.durable_path(copy(application_id));
            if (std::optional<std::string> pending = self.durable_writer.pending(path);
                pending.has_value()) {
                self.durable_read_buffer = std::move(*pending);
                *out_bytes = strata_bytes_view{
                    reinterpret_cast<const std::uint8_t*>(self.durable_read_buffer.data()),
                    self.durable_read_buffer.size(),
                };
                return STRATA_STATUS_OK;
            }
            if (!std::filesystem::is_regular_file(path)) return STRATA_STATUS_NOT_FOUND;
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open()) return STRATA_STATUS_INTERNAL_ERROR;
            self.durable_read_buffer.assign(
                std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
            );
            if (input.bad()) return STRATA_STATUS_INTERNAL_ERROR;
            *out_bytes = strata_bytes_view{
                reinterpret_cast<const std::uint8_t*>(self.durable_read_buffer.data()),
                self.durable_read_buffer.size(),
            };
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INTERNAL_ERROR;
        }
    }

    static strata_status durable_write(
        void* const user_data,
        const strata_string_view application_id,
        const strata_bytes_view bytes
    ) noexcept {
        if (user_data == nullptr || (bytes.data == nullptr && bytes.size != 0U)) {
            return STRATA_STATUS_INVALID_ARGUMENT;
        }
        try {
            auto& self = *static_cast<Impl*>(user_data);
            self.durable_writer.enqueue(
                self.durable_path(copy(application_id)),
                bytes.size == 0U
                    ? std::string{}
                    : std::string(
                          reinterpret_cast<const char*>(bytes.data),
                          reinterpret_cast<const char*>(bytes.data) + bytes.size
                      )
            );
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INTERNAL_ERROR;
        }
    }

    static strata_status async_begin(
        void* const user_data,
        const std::uint64_t request_id,
        const strata_string_view binding,
        const strata_string_view,
        const strata_string_view payload_json
    ) noexcept {
        if (user_data == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            auto& bridge = *static_cast<AsyncBridge*>(user_data);
            const std::string binding_value = copy(binding);
            if (binding_value != "asyncResults" && binding_value != "asyncTreeChildren") {
                return STRATA_STATUS_NOT_FOUND;
            }
            const std::string payload = copy(payload_json);
            bridge.pending.insert_or_assign(request_id, AsyncBridge::Pending{
                request_id,
                binding_value,
                payload,
                bridge.owner->frame_time,
                false,
            });
            if (binding_value == "asyncResults") {
                bridge.owner->showcase_model.data_activity("query " + query_text(payload));
            }
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INTERNAL_ERROR;
        }
    }

    static void async_cancel(void* const user_data, const std::uint64_t request_id) noexcept {
        if (user_data != nullptr) {
            static_cast<AsyncBridge*>(user_data)->pending.erase(request_id);
        }
    }

    static void capture_value(void* const user_data, const strata_string_view value) noexcept {
        try {
            *static_cast<std::string*>(user_data) = copy(value);
        } catch (...) {
        }
    }

    static std::string query_text(const std::string_view payload_json) {
        std::istringstream input{std::string(payload_json)};
        std::string value;
        if (!(input >> std::quoted(value))) return {};
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        std::ranges::transform(value, value.begin(), [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

    static std::optional<std::int64_t> json_integer(
        const std::string_view json,
        const std::string_view name
    ) {
        const std::string marker = "\"" + std::string(name) + "\":";
        const std::size_t start = json.find(marker);
        if (start == std::string_view::npos) return std::nullopt;
        std::int64_t value = 0;
        const char* first = json.data() + start + marker.size();
        const char* last = json.data() + json.size();
        const auto parsed = std::from_chars(first, last, value);
        return parsed.ec == std::errc{} ? std::optional<std::int64_t>(value) : std::nullopt;
    }

    static std::optional<bool> json_boolean(
        const std::string_view json,
        const std::string_view name
    ) {
        const std::string marker = "\"" + std::string(name) + "\":";
        const std::size_t start = json.find(marker);
        if (start == std::string_view::npos) return std::nullopt;
        const std::string_view value = json.substr(start + marker.size());
        if (value.starts_with("true")) return true;
        if (value.starts_with("false")) return false;
        return std::nullopt;
    }

    void restore_window_geometry() {
        if (performance_hud.runtime == nullptr) return;
        std::string json;
        const strata_value_json_sink sink{
            sizeof(strata_value_json_sink), &json, &Impl::capture_value,
        };
        const strata_result read = strata_runtime_read_durable_shell_value_json(
            performance_hud.runtime->native_handle(),
            strata::view("desktop.window.geometry"),
            &sink
        );
        if (read.status == STRATA_STATUS_NOT_FOUND) return;
        strata::require_ok(read, "desktop window geometry restoration");
        const auto x = json_integer(json, "x");
        const auto y = json_integer(json, "y");
        const auto width = json_integer(json, "width");
        const auto height = json_integer(json, "height");
        const bool maximized = json_boolean(json, "maximized").value_or(false);
        if (!x.has_value() || !y.has_value() || !width.has_value() || !height.has_value() ||
            *width < 320 || *height < 240 || *width > 16'384 || *height > 16'384) {
            return;
        }
        RECT placement{
            static_cast<LONG>(*x), static_cast<LONG>(*y),
            static_cast<LONG>(*x + *width), static_cast<LONG>(*y + *height),
        };
        const HMONITOR monitor = MonitorFromRect(&placement, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(MONITORINFO)};
        if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
            const LONG width_value = placement.right - placement.left;
            placement.left = std::clamp(
                placement.left,
                info.rcWork.left - width_value + 80L,
                info.rcWork.right - 80L
            );
            placement.top = std::clamp(
                placement.top,
                info.rcWork.top,
                info.rcWork.bottom - 80L
            );
        }
        SetWindowPos(
            window, nullptr, placement.left, placement.top,
            placement.right - placement.left, placement.bottom - placement.top,
            SWP_NOACTIVATE | SWP_NOZORDER
        );
        if (maximized) ShowWindow(window, SW_MAXIMIZE);
    }

    void remember_window_geometry() {
        if (performance_hud.runtime == nullptr || !IsWindow(window)) return;
        WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
        if (!GetWindowPlacement(window, &placement)) return;
        const RECT& normal = placement.rcNormalPosition;
        const bool maximized = placement.showCmd == SW_SHOWMAXIMIZED;
        const std::string json =
            "{\"height\":" + std::to_string(normal.bottom - normal.top) +
            ",\"maximized\":" + (maximized ? std::string("true") : std::string("false")) +
            ",\"width\":" + std::to_string(normal.right - normal.left) +
            ",\"x\":" + std::to_string(normal.left) +
            ",\"y\":" + std::to_string(normal.top) + "}";
        strata::require_ok(
            strata_runtime_write_durable_shell_value_json(
                performance_hud.runtime->native_handle(),
                strata::view("desktop.window.geometry"),
                strata::view(json)
            ),
            "desktop window geometry persistence"
        );
        strata::require_ok(
            strata_runtime_flush_durable_state(performance_hud.runtime->native_handle()),
            "desktop durable state flush"
        );
    }

    void advance_async(Session& session) {
        if (session.async == nullptr || session.runtime == nullptr) return;
        std::vector<std::uint64_t> completed;
        for (auto& [id, request] : session.async->pending) {
            const std::int64_t elapsed = frame_time - request.started_nanos;
            const bool tree_children = request.binding == "asyncTreeChildren";
            if (!tree_children && !request.progress_published && elapsed >= 350'000'000) {
                request.progress_published = true;
                const std::string message = "Simulated desktop host query";
                const strata_async_progress progress{
                    sizeof(strata_async_progress), 1.0, 2.0, 1U, 0U,
                    strata::view(message),
                };
                const strata_result result = strata_runtime_async_progress(
                    session.runtime->native_handle(), id, &progress
                );
                if (result.status != STRATA_STATUS_NOT_FOUND) {
                    strata::require_ok(result, "desktop async progress");
                }
            }
            const std::int64_t completion_delay = tree_children
                ? 100'000'000 : 850'000'000;
            if (elapsed < completion_delay) continue;
            strata_result result;
            const std::string query = query_text(request.payload);
            if (tree_children) {
                const strata::host::Value event = strata::host::Value::parse(request.payload);
                if (event.string() != nullptr) {
                    static_cast<void>(showcase_model.load_tree_children(*event.string()));
                }
                const std::string items = showcase_model.tree_items().json();
                result = strata_runtime_async_succeed_json(
                    session.runtime->native_handle(), id, strata::view(items)
                );
            } else if (query == "fail") {
                result = strata_runtime_async_fail(
                    session.runtime->native_handle(), id,
                    strata::view("The simulated source rejected the query."),
                    strata::view("SIMULATED_FAILURE")
                );
            } else if (query == "empty") {
                result = strata_runtime_async_succeed_json(
                    session.runtime->native_handle(), id, strata::view("[]")
                );
            } else {
                std::string values = "[";
                for (std::size_t index = 0U; index < 12U; ++index) {
                    if (index != 0U) values.push_back(',');
                    values += "{\"key\":\"async." + std::to_string(index) +
                        "\",\"label\":\"Simulated result " + std::to_string(index) + "\"}";
                }
                values.push_back(']');
                result = strata_runtime_async_succeed_json(
                    session.runtime->native_handle(), id, strata::view(values)
                );
            }
            if (result.status != STRATA_STATUS_NOT_FOUND) {
                strata::require_ok(result, "desktop async completion");
            }
            completed.push_back(id);
        }
        for (const std::uint64_t id : completed) session.async->pending.erase(id);
    }

    static void diagnostic(
        void* const,
        const strata_diagnostic* const value
    ) noexcept {
        if (value == nullptr) return;
        try {
            std::cerr << "strata desktop: " << copy(value->code) << ": "
                      << copy(value->message) << '\n';
        } catch (...) {
        }
    }

    [[nodiscard]] strata_surface_environment environment() const noexcept {
        return strata_surface_environment{
            sizeof(strata_surface_environment),
            environment_generation,
            static_cast<std::int64_t>(framebuffer_width),
            static_cast<std::int64_t>(framebuffer_height),
            logical_width(),
            logical_height(),
            display_scale,
            0.0,
            0.0,
            0.0,
            0.0,
            STRATA_POINT_SNAP_NEAREST,
            STRATA_RECTANGLE_SNAP_OUTWARD,
            STRATA_SURFACE_DENSITY_COMFORTABLE,
            STRATA_POINTER_PRECISION_FINE,
            STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD |
                STRATA_SURFACE_INPUT_CLIPBOARD,
            0U,
            0U,
        };
    }

    static void profile(void* const user_data, const strata_profiler_snapshot* const snapshot) {
        if (user_data == nullptr || snapshot == nullptr) return;
        auto& result = *static_cast<ProfileSnapshot*>(user_data);
        result.scope_id = copy(snapshot->scope_id);
        result.frame_index = snapshot->frame_index;
        result.sections.clear();
        result.counters.clear();
        result.sections.reserve(snapshot->section_count);
        result.counters.reserve(snapshot->counter_count);
        for (std::size_t index = 0U; index < snapshot->section_count; ++index) {
            const strata_profiler_section& section = snapshot->sections[index];
            result.sections.push_back(ProfileSection{
                copy(section.path), section.last_sample_frame_index,
                section.last_nanos, section.p95_nanos,
                section.p99_nanos, section.maximum_nanos,
            });
        }
        for (std::size_t index = 0U; index < snapshot->counter_count; ++index) {
            const strata_profiler_counter& counter = snapshot->counters[index];
            result.counters.push_back(ProfileCounter{copy(counter.name), counter.value});
        }
    }

    void publish(Session& session, const std::string_view id, const std::string& json) {
        static_cast<void>(session.runtime->publish_host_snapshot(id, json));
    }

    /**
     * Compiles every material the application declares an HLSL stage for. The declaration is the
     * same backend-neutral one every host reads, so adding a backend adds a source key, not a
     * second contract.
     */
    void declare_materials(const strata::Runtime& runtime) {
        for (const strata::MaterialDeclaration& declaration :
             runtime.material_declarations("hlsl")) {
            if (declaration.source.empty()) continue;
            renderer.declare_material(declaration.id, services.text(declaration.source));
        }
    }

    [[nodiscard]] Session create_session(
        std::string id,
        const std::string_view schemas_resource,
        const std::span<const std::string_view> extension_packages = {}
    ) {
        frame_time = now();
        strata_runtime_config config{};
        config.struct_size = sizeof(config);
        config.abi_version = STRATA_ABI_VERSION_CURRENT;
        config.required_capabilities = STRATA_CAPABILITY_CORE_LIFECYCLE |
            STRATA_CAPABILITY_CALLER_CLOCK |
            STRATA_CAPABILITY_HOST_SNAPSHOTS |
            STRATA_CAPABILITY_APPLICATION_LIFECYCLE |
            STRATA_CAPABILITY_COMPILER_ACTIVATION |
            STRATA_CAPABILITY_COMPILED_MODULE_ACTIVATION |
            STRATA_CAPABILITY_ACTION_DISPATCH |
            STRATA_CAPABILITY_RESOURCE_ADAPTER |
            STRATA_CAPABILITY_CLIPBOARD_IME_ADAPTER |
            STRATA_CAPABILITY_DURABLE_STATE |
            STRATA_CAPABILITY_ASYNC_HOST_DATA |
            STRATA_CAPABILITY_SURFACE_RUNTIME |
            STRATA_CAPABILITY_SURFACE_RENDER_PACKET;
        config.stable_identity_seed = 0x5354524154414445ULL;
        config.clock = strata_clock{sizeof(strata_clock), this, &Impl::clock};
        config.diagnostics = strata_diagnostic_sink{
            sizeof(strata_diagnostic_sink), this, &Impl::diagnostic,
        };
        Session result;
        result.id = std::move(id);
        result.runtime = std::make_unique<strata::Runtime>(config);

        const strata_resource_adapter resources = services.resource_adapter();
        strata::require_ok(
            strata_runtime_set_resource_adapter(result.runtime->native_handle(), &resources),
            "desktop resource adapter installation"
        );
        const strata_clipboard_adapter clipboard = services.clipboard_adapter();
        strata::require_ok(
            strata_runtime_set_clipboard_adapter(result.runtime->native_handle(), &clipboard),
            "desktop clipboard adapter installation"
        );

        const std::string registry = services.text("strata/registry-v1.json");
        const std::string schemas = services.text(schemas_resource);
        std::vector<std::string> extension_schemas;
        extension_schemas.reserve(extension_packages.size());
        for (const std::string_view package_id : extension_packages) {
            extension_schemas.push_back(
                strata::extension::Registry::instance().require(package_id).schema_json()
            );
        }
        std::vector<strata_string_view> extension_schema_views;
        extension_schema_views.reserve(extension_schemas.size());
        for (const std::string& document : extension_schemas) {
            extension_schema_views.push_back(strata::view(document));
        }
        const strata_application_config application{
            sizeof(strata_application_config),
            strata::view(result.id),
            strata::view(registry),
            strata::view(schemas),
            extension_schema_views.empty() ? nullptr : extension_schema_views.data(),
            extension_schema_views.size(),
        };
        result.runtime->configure_application(application);
        const strata_durable_store_adapter durable{
            sizeof(strata_durable_store_adapter), this, &Impl::durable_load, &Impl::durable_write,
        };
        result.runtime->set_durable_store(durable);
        result.async = std::make_unique<AsyncBridge>(AsyncBridge{
            this, result.runtime->native_handle(), {},
        });
        const strata_async_host_adapter async{
            sizeof(strata_async_host_adapter),
            result.async.get(),
            &Impl::async_begin,
            &Impl::async_cancel,
        };
        result.runtime->set_async_host(async);
        result.bindings = std::make_unique<strata::host::Bindings>(
            *result.runtime, "strata.desktop." + result.id
        );
        declare_materials(*result.runtime);
        return result;
    }

    void activate(
        Session& session,
        const std::string_view source_resource,
        const std::string_view root_name,
        const strata_surface_extension_bundle* const extensions
    ) {
        if (!source_resource.ends_with(".strata")) {
            throw std::invalid_argument("desktop bundled module must use a .strata resource id");
        }
        std::string artifact_resource(source_resource);
        artifact_resource.resize(artifact_resource.size() - std::string_view(".strata").size());
        artifact_resource += ".compiled.bin";
        const std::vector<std::uint8_t> artifact = services.bytes(artifact_resource);
        const strata_compiled_activation_config activation{
            sizeof(strata_compiled_activation_config),
            1U,
            strata_bytes_view{artifact.data(), artifact.size()},
        };
        const strata_activation_info activated = session.runtime->activate(activation);
        if (activated.status != STRATA_ACTIVATION_ACTIVATED) {
            throw std::runtime_error("desktop compiled Strata module did not activate");
        }

        constexpr std::array fonts{
            strata_surface_font_resource{
                strata::view("strata:fonts/default-medium"),
                strata::view("assets/strata/fonts/medium.ttf"),
            },
            strata_surface_font_resource{
                strata::view("strata:fonts/default"),
                strata::view("assets/strata/fonts/default.ttf"),
            },
            strata_surface_font_resource{
                strata::view("strata:fonts/mono"),
                strata::view("assets/strata/fonts/mono.ttf"),
            },
        };
        constexpr std::array textures{
            strata_surface_texture_resource{
                strata::view("strata:ui/icons/chevron-down"),
                strata::view("assets/strata/textures/ui/icons/chevron-down.png"),
                STRATA_TEXTURE_SAMPLING_LINEAR,
                0U,
            },
            strata_surface_texture_resource{
                strata::view("strata:ui/icons/chevron-up"),
                strata::view("assets/strata/textures/ui/icons/chevron-up.png"),
                STRATA_TEXTURE_SAMPLING_LINEAR,
                0U,
            },
        };
        strata_surface_config surface_config{};
        surface_config.struct_size = sizeof(surface_config);
        surface_config.id = strata::view(session.id);
        surface_config.root_role = STRATA_SURFACE_ROOT_OVERLAY;
        surface_config.root_name = strata::view(root_name);
        surface_config.environment = environment();
        surface_config.fonts = fonts.data();
        surface_config.font_count = fonts.size();
        surface_config.extensions = extensions;
        surface_config.textures = textures.data();
        surface_config.texture_count = textures.size();
        session.surface.emplace(session.runtime->create_surface(surface_config));
    }

    void create_settings() {
        settings = create_session(
            "settings.desktop",
            "assets/strata/ui/settings_app.schemas.json"
        );
        settings.bindings->on("settings.save", [this](const strata::host::ActionEvent&) {
            ++settings_save_count;
            settings_revision.changed();
            return strata::host::ActionResult::handled;
        });
        settings.bindings->snapshot(
            "settings.desktop",
            [this] { return settings_revision.value(); },
            [this] {
                const std::string saved = settings_save_count == 0U
                    ? "No changes saved yet"
                    : "Saved " + std::to_string(settings_save_count) + " time" +
                        (settings_save_count == 1U ? std::string{} : std::string("s"));
                const auto profile = [](const std::string_view id, const std::string_view label) {
                    return strata::host::Value::object({
                        {"key", std::string("settings.profile.") + std::string(id)},
                        {"label", label},
                        {"mayHaveChildren", false},
                        {"childrenLoaded", true},
                    });
                };
                return strata::host::Value::object({
                    {"settings", strata::host::Value::object({
                        {"savedMessage", saved},
                        {"profileTree", strata::host::Value::array({
                            profile("balanced", "Balanced — Recommended visual and input defaults"),
                            profile("performance", "Performance — Reduced effects and tighter layout"),
                            profile("cinematic", "Cinematic — High-fidelity effects and comfortable spacing"),
                        })},
                    })},
                });
            }
        );
        settings.bindings->synchronize();
        activate(
            settings,
            "assets/strata/ui/settings_app.strata",
            "SettingsApp",
            nullptr
        );
    }

    void create_showcase() {
        static constexpr std::array action_ids{
            std::string_view("control.alpha.action"), std::string_view("control.beta.action"),
            std::string_view("control.gamma.action"), std::string_view("demo.behavior.activate"),
            std::string_view("demo.behavior.disabled"), std::string_view("demo.combo.query"),
            std::string_view("demo.combo.select"), std::string_view("demo.command.global"),
            std::string_view("demo.command.scoped"), std::string_view("demo.custom.pulse"),
            std::string_view("demo.data.collection"), std::string_view("demo.drop.target"),
            std::string_view("demo.events.clear"), std::string_view("demo.focus.scope"),
            std::string_view("demo.form.completed"), std::string_view("demo.host.bump"),
            std::string_view("demo.host.message"), std::string_view("demo.inspect.arm"),
            std::string_view("demo.inspect.clear"), std::string_view("demo.inspect.coalesce"),
            std::string_view("demo.inspect.driver"), std::string_view("demo.inspect.dynamic-probe"),
            std::string_view("demo.inspect.pointer"), std::string_view("demo.inspect.select"),
            std::string_view("demo.nav.right"), std::string_view("demo.reorder"),
            std::string_view("demo.split.controlled"), std::string_view("demo.state.restore"),
            std::string_view("demo.state.snapshot"),
        };
        static constexpr std::array extension_packages{std::string_view("strata.demo.v1")};
        showcase = create_session(
            "demo.desktop",
            "assets/strata/ui/demo_surface.schemas.json",
            extension_packages
        );
        for (const std::string_view action_id : action_ids) {
            showcase.bindings->on(std::string(action_id), [this](
                                                        const strata::host::ActionEvent& event
                                                    ) {
                return showcase_model.handle(event);
            });
        }
        showcase.bindings->snapshot(
            "demo.desktop.data.tree",
            [this] { return showcase_model.tree_revision().value(); },
            [this] { return showcase_model.tree_snapshot(); }
        );
        showcase.bindings->snapshot(
            "demo.desktop.data.table",
            [this] { return showcase_model.table_revision().value(); },
            [this] { return showcase_model.table_snapshot(); }
        );
        showcase.bindings->snapshot(
            "demo.desktop.data.grid",
            [this] { return showcase_model.grid_revision().value(); },
            [this] { return showcase_model.grid_snapshot(); }
        );
        showcase.bindings->snapshot(
            "demo.desktop.state",
            [this] { return showcase_model.demo_revision().value(); },
            [this] { return showcase_model.demo_snapshot(); }
        );
        showcase.bindings->synchronize();
        activate(
            showcase,
            "assets/strata/ui/demo_surface.strata",
            "MainShowcase",
            &strata::extension::Registry::instance().require(extension_packages.front()).bundle()
        );
        strata::require_ok(
            strata_surface_set_profiler_capture(showcase.surface->native_handle(), 1U),
            "desktop showcase profiler capture"
        );
    }

    [[nodiscard]] static TimingSummary summarize(const TimingWindow& timings) {
        return TimingSummary{timings.last(), timings.p95(), timings.maximum()};
    }

    void capture_profile_snapshot() {
        if (!showcase.surface.has_value()) return;
        ProfileCapture capture;
        capture.captured_at = frame_time;
        const strata_profiler_snapshot_sink runtime_sink{
            sizeof(strata_profiler_snapshot_sink), &capture.runtime, &Impl::profile,
        };
        strata::require_ok(
            strata_runtime_read_profiler(showcase.runtime->native_handle(), &runtime_sink),
            "desktop showcase runtime profiler sampling"
        );
        const strata_profiler_snapshot_sink surface_sink{
            sizeof(strata_profiler_snapshot_sink), &capture.surface, &Impl::profile,
        };
        strata::require_ok(
            strata_surface_read_profiler(showcase.surface->native_handle(), &surface_sink),
            "desktop showcase profiler sampling"
        );
        capture.host = {
            summarize(host_total_timings),
            summarize(showcase_core_timings),
            summarize(showcase_submit_timings),
            summarize(tooling_timings),
            summarize(present_timings),
        };
        profile_history.push_back(std::move(capture));
        constexpr std::size_t maximum_profile_captures = 64U;
        if (profile_history.size() > maximum_profile_captures) profile_history.pop_front();
    }

    [[nodiscard]] ProfileSnapshot merged_profile_snapshot() const {
        ProfileSnapshot result;
        std::map<std::string, std::size_t, std::less<>> sections;
        const auto merge_sections = [&result, &sections](
                                        const ProfileSnapshot& snapshot,
                                        const bool runtime
                                    ) {
            for (const ProfileSection& source : snapshot.sections) {
                const std::string path = runtime
                    ? "runtime:" + snapshot.scope_id + "/" + source.path
                    : source.path;
                const auto [found, inserted] = sections.try_emplace(
                    path,
                    result.sections.size()
                );
                if (inserted) {
                    result.sections.push_back(source);
                    result.sections.back().path = path;
                    continue;
                }
                ProfileSection& destination = result.sections[found->second];
                destination.last_sample_frame_index = source.last_sample_frame_index;
                destination.last_nanos = source.last_nanos;
                destination.p95_nanos = std::max(
                    destination.p95_nanos, source.p95_nanos
                );
                destination.p99_nanos = std::max(
                    destination.p99_nanos, source.p99_nanos
                );
                destination.maximum_nanos = std::max(
                    destination.maximum_nanos, source.maximum_nanos
                );
            }
        };
        for (const ProfileCapture& capture : profile_history) {
            result.scope_id = capture.surface.scope_id;
            result.frame_index = capture.surface.frame_index;
            result.counters = capture.surface.counters;
            merge_sections(capture.runtime, true);
            merge_sections(capture.surface, false);
        }
        return result;
    }

    [[nodiscard]] static std::string full_profile_path(
        const std::string_view scope_id,
        const std::string_view path
    ) {
        if (path.starts_with("runtime:")) return std::string(path);
        return std::string(scope_id) + "/" + std::string(path);
    }

    [[nodiscard]] std::array<TimingSummary, 5U> merged_host_timings() const {
        std::array<TimingSummary, 5U> result{};
        for (const ProfileCapture& capture : profile_history) {
            for (std::size_t index = 0U; index < result.size(); ++index) {
                result[index].last_nanos = capture.host[index].last_nanos;
                result[index].p95_nanos = std::max(
                    result[index].p95_nanos, capture.host[index].p95_nanos
                );
                result[index].maximum_nanos = std::max(
                    result[index].maximum_nanos, capture.host[index].maximum_nanos
                );
            }
        }
        return result;
    }

    [[nodiscard]] std::string debug_json() const {
        const ProfileSnapshot profile_snapshot = merged_profile_snapshot();
        const std::array<TimingSummary, 5U> host_timings = merged_host_timings();
        std::vector<ProfileSection> hot_paths = profile_snapshot.sections;
        std::ranges::stable_sort(hot_paths, [](const ProfileSection& left, const ProfileSection& right) {
            return left.p99_nanos > right.p99_nanos;
        });
        if (hot_paths.size() > 24U) hot_paths.resize(24U);
        static constexpr std::size_t host_path_count = 5U;
        std::ostringstream output;
        output << "{\"debug\":{\"mode\":" << json_string(debug_mode)
               << ",\"frame\":" << profile_snapshot.frame_index
               << ",\"errors\":0,\"warnings\":0,\"infos\":0,"
                  "\"diagnosticCount\":0,\"droppedDiagnostics\":0,"
                  "\"hotPathCount\":" << hot_paths.size() + host_path_count
               << ",\"spikeCount\":0,\"motionCount\":0,\"runningMotionCount\":0,"
                  "\"semanticCount\":0,\"collectionCount\":0,\"summaryMetrics\":[";
        std::int64_t last_frame = 0;
        for (const ProfileSection& section : profile_snapshot.sections) {
            if (section.path == "frame") last_frame = std::max(last_frame, section.last_nanos);
        }
        output << "{\"label\":\"Active native surfaces\",\"value\":\"1\"},"
               << "{\"label\":\"Latest native frame\",\"value\":\""
               << profile_snapshot.frame_index << "\"},"
               << "{\"label\":\"Last native frame\",\"value\":\""
               << static_cast<double>(last_frame) / 1'000'000.0 << " ms\"},"
               << "{\"label\":\"Last complete host frame\",\"value\":\""
               << static_cast<double>(last_total_nanos) / 1'000'000.0 << " ms\"},"
               << "{\"label\":\"Last showcase core frame\",\"value\":\""
               << static_cast<double>(last_native_nanos) / 1'000'000.0 << " ms\"}],\"counters\":[";
        const std::size_t counter_count = profile_snapshot.counters.size();
        for (std::size_t index = 0U; index < counter_count; ++index) {
            if (index != 0U) output << ',';
            output << "{\"label\":" << json_string(profile_snapshot.counters[index].name)
                   << ",\"value\":\"" << profile_snapshot.counters[index].value << "\"}";
        }
        output << "],\"hotPaths\":[";
        const auto emit_host_path = [&output](
                                        const std::string_view path,
                                        const TimingSummary& timings,
                                        const bool first
                                    ) {
            if (!first) output << ',';
            output << "{\"path\":" << json_string(path)
                   << ",\"lastMillis\":" << static_cast<double>(timings.last_nanos) / 1'000'000.0
                   << ",\"p95Millis\":" << static_cast<double>(timings.p95_nanos) / 1'000'000.0
                   << ",\"p99Millis\":" << static_cast<double>(timings.maximum_nanos) / 1'000'000.0
                   << ",\"maxMillis\":" << static_cast<double>(timings.maximum_nanos) / 1'000'000.0
                   << '}';
        };
        emit_host_path("demo.desktop/host/total", host_timings[0U], true);
        emit_host_path("demo.desktop/host/showcase-core", host_timings[1U], false);
        emit_host_path("demo.desktop/host/showcase-submit", host_timings[2U], false);
        emit_host_path("demo.desktop/host/tooling", host_timings[3U], false);
        emit_host_path("demo.desktop/host/present", host_timings[4U], false);
        for (std::size_t index = 0U; index < hot_paths.size(); ++index) {
            output << ',';
            const ProfileSection& section = hot_paths[index];
            output << "{\"path\":" << json_string(full_profile_path(profile_snapshot.scope_id, section.path))
                   << ",\"lastMillis\":" << static_cast<double>(section.last_nanos) / 1'000'000.0
                   << ",\"p95Millis\":" << static_cast<double>(section.p95_nanos) / 1'000'000.0
                   << ",\"p99Millis\":" << static_cast<double>(section.p99_nanos) / 1'000'000.0
                   << ",\"maxMillis\":" << static_cast<double>(section.maximum_nanos) / 1'000'000.0 << '}';
        }
        output << "],\"spikes\":[],\"diagnostics\":[],\"motions\":[],"
                  "\"semantics\":[],\"collections\":[],\"reloadMetrics\":[]}}";
        return output.str();
    }

    [[nodiscard]] std::string debug_export_text() const {
        const ProfileSnapshot profile_snapshot = merged_profile_snapshot();
        const std::array<TimingSummary, 5U> host_timings = merged_host_timings();
        std::vector<ProfileSection> hot_paths = profile_snapshot.sections;
        std::ranges::stable_sort(
            hot_paths,
            [](const ProfileSection& left, const ProfileSection& right) {
                return left.p95_nanos > right.p95_nanos;
            }
        );
        const auto millis = [](const std::int64_t nanos) {
            return static_cast<double>(nanos) / 1'000'000.0;
        };
        std::ostringstream output;
        output << std::fixed << std::setprecision(4)
               << "STRATA DEBUG PROFILE\n"
               << "frame\t" << profile_snapshot.frame_index << '\n'
               << "columns\tpath\tlast_ms\trecent_p95_peak_ms\tmax_ms\n\n"
               << "HOST PATHS\n";
        static constexpr std::array<std::string_view, 5U> host_paths{
            "demo.desktop/host/total",
            "demo.desktop/host/showcase-core",
            "demo.desktop/host/showcase-submit",
            "demo.desktop/host/tooling",
            "demo.desktop/host/present",
        };
        for (std::size_t index = 0U; index < host_paths.size(); ++index) {
            output << host_paths[index] << '\t'
                   << millis(host_timings[index].last_nanos) << '\t'
                   << millis(host_timings[index].p95_nanos) << '\t'
                   << millis(host_timings[index].maximum_nanos) << '\n';
        }
        output << "\nNATIVE HOT PATHS (all sections)\n";
        for (const ProfileSection& section : hot_paths) {
            output << full_profile_path(profile_snapshot.scope_id, section.path) << '\t'
                   << millis(section.last_nanos) << '\t'
                   << millis(section.p95_nanos) << '\t'
                   << millis(section.maximum_nanos) << '\n';
        }
        output << "\nCOUNTERS (latest snapshot)\n";
        for (const ProfileCounter& counter : profile_snapshot.counters) {
            output << counter.name << '\t' << counter.value << '\n';
        }
        return output.str();
    }

    void create_debug() {
        if (debug.surface.has_value()) return;
        debug = create_session(
            "profiler.desktop", "assets/strata/ui/debug_overlay.schemas.json"
        );
        debug.bindings->on("strata.debug.close", [this](const strata::host::ActionEvent&) {
            debug_enabled = false;
            return strata::host::ActionResult::handled;
        });
        debug.bindings->on(
            "strata.debug.clear-diagnostics",
            [this](const strata::host::ActionEvent&) {
                clear_diagnostics_pending = true;
                return strata::host::ActionResult::handled;
            }
        );
        debug.bindings->on(
            "strata.debug.copy-profile",
            [this](const strata::host::ActionEvent&) {
                static_cast<void>(services.write_clipboard(debug_export_text()));
                return strata::host::ActionResult::handled;
            }
        );
        debug.bindings->on(
            "strata.debug.select-mode",
            [this](const strata::host::ActionEvent& event) {
                const std::string mode(event.payload.require_string("mode"));
                if (std::ranges::contains(debug_modes, mode)) {
                    debug_mode = mode;
                    debug_snapshot_dirty = true;
                }
                return strata::host::ActionResult::handled;
            }
        );
        publish(debug, "profiler.desktop", debug_json());
        activate(
            debug,
            "assets/strata/ui/debug_overlay.strata",
            "ProfilerOverlay",
            nullptr
        );
    }

    [[nodiscard]] std::string performance_json() const {
        std::uint64_t recent_frames = 0U;
        std::uint64_t recent_total_nanos = 0U;
        if (!frame_buckets.empty()) {
            const std::int64_t first_recent = frame_buckets.back().index - 3;
            for (const FrameBucket& bucket : frame_buckets) {
                if (bucket.index < first_recent) continue;
                recent_frames += bucket.frame_count;
                recent_total_nanos += bucket.total_nanos;
            }
        }
        const double fps = recent_total_nanos > 0U
            ? static_cast<double>(recent_frames) * 1'000'000'000.0 /
                static_cast<double>(recent_total_nanos)
            : 0.0;
        const double total_millis = static_cast<double>(last_total_nanos) / 1'000'000.0;
        const double native_millis = static_cast<double>(last_native_nanos) / 1'000'000.0;
        const auto decimal = [](const double value) {
            std::ostringstream text;
            text << std::fixed << std::setprecision(1) << value;
            return text.str();
        };

        std::ostringstream output;
        output << "{\"performance\":{\"fps\":" << json_string(decimal(fps))
               << ",\"totalMillis\":" << json_string(decimal(total_millis))
               << ",\"nativeMillis\":" << json_string(decimal(native_millis))
               << ",\"samples\":[";
        bool first = true;
        for (const FrameBucket& bucket : frame_buckets) {
            if (!first) output << ',';
            first = false;
            const auto graph_height = [](const std::int64_t nanos) {
                if (nanos <= 0) return 0.0;
                const double millis = static_cast<double>(nanos) / 1'000'000.0;
                return std::clamp(
                    std::log1p(millis) / std::log1p(500.0) * 58.0, 1.0, 58.0
                );
            };
            const double total_height = graph_height(bucket.maximum_total_nanos);
            const double native_height = graph_height(bucket.maximum_native_nanos);
            output << "{\"key\":\"bucket." << bucket.index
                   << "\",\"totalHeight\":" << total_height
                   << ",\"nativeHeight\":" << native_height
                   << ",\"slow\":"
                   << (bucket.maximum_total_nanos > 16'666'667 ? "true" : "false")
                   << ",\"severe\":"
                   << (bucket.maximum_total_nanos > 100'000'000 ? "true" : "false")
                   << '}';
        }
        output << "]}}";
        return output.str();
    }

    /** Hub view over the surfaces this host owns, in the shape the hub module declares. */
    [[nodiscard]] std::string hub_json() const {
        std::ostringstream output;
        output << std::fixed << std::setprecision(3);
        const double millis = static_cast<double>(host_total_timings.last()) / 1'000'000.0;
        output << "{\"hub\":{\"frameMillis\":" << millis << ",\"frameSummary\":\"";
        output << std::setprecision(1) << millis << " ms  \\u00b7  "
               << std::setprecision(0) << (millis > 0.0 ? 1'000.0 / millis : 0.0) << " fps\",";
        // The chart reads against the window's own peak rather than a fixed budget: a host holding
        // 1 ms frames would otherwise draw a flat line along the floor and say nothing at all.
        double scale = 8.0;
        for (const double sample : frame_history) scale = std::max(scale, sample);
        const std::size_t samples = frame_history.size();
        const auto emit_line = [&] {
            for (std::size_t index = 0U; index < samples; ++index) {
                if (index != 0U) output << ',';
                const double x = samples < 2U
                    ? 0.0
                    : static_cast<double>(index) / static_cast<double>(samples - 1U);
                const double cost = std::clamp(frame_history[index] / scale, 0.0, 1.0);
                output << "{\"x\":" << x << ",\"y\":" << (1.0 - cost) << '}';
            }
        };
        output << std::setprecision(4) << "\"frameHistory\":[";
        emit_line();
        // The filled area is that same curve closed along the floor, which is the one thing the
        // drawing layer cannot derive from the line by itself.
        output << "],\"frameArea\":[";
        emit_line();
        if (samples != 0U) output << ",{\"x\":1,\"y\":1},{\"x\":0,\"y\":1}";
        output << "],\"frameScale\":\"full scale " << std::setprecision(1) << scale << " ms\",";
        const int open_count = (settings_enabled ? 1 : 0) + (showcase_enabled ? 1 : 0) +
            (debug_enabled ? 1 : 0) + (performance_hud_enabled ? 1 : 0);
        output << "\"status\":\"";
        if (open_count == 0) output << "No surface open";
        else if (open_count == 1) output << "1 surface open \\u00b7 shortcuts stay live";
        else output << open_count << " surfaces open \\u00b7 shortcuts stay live";
        output << "\",\"surfaces\":[";
        const std::array<std::array<std::string_view, 4U>, 4U> entries{{
            {"hub.settings", "Settings", "Durable application state", "F6"},
            {"hub.showcase", "Component showcase", "Widgets and extensions", "F7"},
            {"hub.profiler", "Profiler overlay", "Layout and render timings", "F8"},
            {"hub.performance", "Performance HUD", "Passive frame telemetry", "F9"},
        }};
        const std::array<bool, 4U> active{
            settings_enabled, showcase_enabled, debug_enabled, performance_hud_enabled,
        };
        for (std::size_t index = 0U; index < entries.size(); ++index) {
            if (index != 0U) output << ',';
            output << "{\"key\":\"" << entries[index][0] << "\",\"name\":\"" << entries[index][1]
                   << "\",\"detail\":\"" << entries[index][2] << "\",\"shortcut\":\""
                   << entries[index][3] << "\",\"active\":"
                   << (active[index] ? "true" : "false") << '}';
        }
        output << "]}}";
        return output.str();
    }

    void launch_from_hub(const std::string& payload) {
        if (payload.find("hub.self") != std::string::npos) {
            hub_enabled = false;
            if (hub.surface.has_value()) {
                static_cast<void>(strata_surface_cancel_interactions(hub.surface->native_handle()));
            }
        } else if (payload.find("hub.settings") != std::string::npos) {
            toggle_settings();
        } else if (payload.find("hub.showcase") != std::string::npos) {
            toggle_showcase();
        } else if (payload.find("hub.profiler") != std::string::npos) {
            cycle_debug(false);
        } else if (payload.find("hub.performance") != std::string::npos) {
            toggle_performance_hud();
        }
    }

    void create_hub() {
        if (hub.surface.has_value()) return;
        hub = create_session("hub.desktop", "assets/strata/ui/strata_hub.schemas.json");
        hub.bindings->on("hub.launch", [this](const strata::host::ActionEvent& event) {
            launch_from_hub(std::string(event.payload.require_string("target")));
            return strata::host::ActionResult::handled;
        });
        publish(hub, "hub.desktop", hub_json());
        activate(hub, "assets/strata/ui/strata_hub.strata", "StrataHub", nullptr);
    }

    void toggle_hub() {
        if (!hub_enabled && !hub.surface.has_value()) create_hub();
        hub_enabled = !hub_enabled;
        if (!hub_enabled && hub.surface.has_value()) {
            static_cast<void>(strata_surface_cancel_interactions(hub.surface->native_handle()));
        }
    }

    void create_performance_hud() {
        if (performance_hud.surface.has_value()) return;
        performance_hud = create_session(
            "performance.desktop", "assets/strata/ui/performance_hud.schemas.json"
        );
        publish(performance_hud, "performance.desktop", performance_json());
        activate(
            performance_hud,
            "assets/strata/ui/performance_hud.strata",
            "PerformanceHud",
            nullptr
        );
    }

    void record_frame_sample(
        const std::int64_t timestamp,
        const std::int64_t total_nanos,
        const std::int64_t native_nanos
    ) {
        const std::int64_t bucket_index = timestamp / 500'000'000;
        if (frame_buckets.empty() || bucket_index > frame_buckets.back().index) {
            std::int64_t first_index = frame_buckets.empty()
                ? bucket_index - 59
                : frame_buckets.back().index + 1;
            if (bucket_index - first_index >= 60) {
                frame_buckets.clear();
                first_index = bucket_index - 59;
            }
            for (std::int64_t index = first_index; index <= bucket_index; ++index) {
                frame_buckets.push_back(FrameBucket{index});
            }
            while (frame_buckets.size() > 60U) frame_buckets.pop_front();
        }
        if (frame_buckets.empty() || frame_buckets.back().index != bucket_index) return;

        FrameBucket& bucket = frame_buckets.back();
        bucket.maximum_total_nanos = std::max(bucket.maximum_total_nanos, total_nanos);
        bucket.maximum_native_nanos = std::max(bucket.maximum_native_nanos, native_nanos);
        bucket.total_nanos += static_cast<std::uint64_t>(std::max<std::int64_t>(0, total_nanos));
        ++bucket.frame_count;
        last_total_nanos = total_nanos;
        last_native_nanos = native_nanos;
    }

    void close_session(Session& session) noexcept {
        if (!session.surface.has_value()) return;
        try {
            renderer.consume_resources(session.decoder.decode(
                session.surface->prepare_release_packet()
            ));
            session.surface->acknowledge_release_packet();
            session.surface->close();
        } catch (...) {
            try {
                session.surface->abandon();
            } catch (...) {
                std::terminate();
            }
        }
        session.surface.reset();
        session.bindings.reset();
        session.runtime.reset();
        session.decoder.reset();
    }

    void toggle_settings() {
        if (!settings_enabled && !settings.surface.has_value()) create_settings();
        settings_enabled = !settings_enabled;
        if (!settings_enabled && settings.surface.has_value()) {
            static_cast<void>(strata_surface_cancel_interactions(
                settings.surface->native_handle()
            ));
        }
    }

    void toggle_showcase() {
        if (!showcase_enabled && !showcase.surface.has_value()) {
            create_showcase();
        }
        showcase_enabled = !showcase_enabled;
        if (!showcase_enabled && showcase.surface.has_value()) {
            static_cast<void>(strata_surface_cancel_interactions(showcase.surface->native_handle()));
        }
    }

    void cycle_debug(const bool reverse) {
        if (!debug_enabled) {
            debug_mode = std::string(reverse ? debug_modes.back() : debug_modes.front());
            create_debug();
            debug_enabled = true;
        } else {
            const auto current = std::ranges::find(debug_modes, debug_mode);
            const std::ptrdiff_t index = current != debug_modes.end()
                ? current - debug_modes.begin()
                : 0;
            const std::ptrdiff_t next = index + (reverse ? -1 : 1);
            if (next < 0 || next >= static_cast<std::ptrdiff_t>(debug_modes.size())) {
                debug_enabled = false;
                if (debug.surface.has_value()) {
                    static_cast<void>(strata_surface_cancel_interactions(
                        debug.surface->native_handle()
                    ));
                }
                return;
            }
            debug_mode = std::string(debug_modes[static_cast<std::size_t>(next)]);
        }
        debug_snapshot_dirty = true;
        next_debug_snapshot = 0;
    }

    void toggle_performance_hud() {
        performance_hud_enabled = !performance_hud_enabled;
        if (performance_hud_enabled) {
            create_performance_hud();
            performance_snapshot_dirty = true;
            next_performance_snapshot = 0;
        }
    }

    void resize(
        const std::uint32_t next_width,
        const std::uint32_t next_height,
        const double next_scale
    ) {
        if (next_width == 0U || next_height == 0U || next_scale <= 0.0) return;
        framebuffer_width = next_width;
        framebuffer_height = next_height;
        display_scale = desktop_display_scale(next_width, next_height, next_scale);
        renderer.resize(next_width, next_height, logical_width(), logical_height());
        ++environment_generation;
        const strata_surface_environment next = environment();
        for (Session* session : {&showcase, &settings, &debug, &performance_hud, &hub}) {
            if (!session->surface.has_value()) continue;
            std::uint32_t adopted = 0U;
            strata::require_ok(
                strata_surface_adopt_environment(session->surface->native_handle(), &next, &adopted),
                "desktop environment adoption"
            );
            if (adopted == 0U) {
                throw std::runtime_error("desktop environment generation was rejected");
            }
        }
    }

    void enqueue(strata_input_event event, const std::string_view input_text = {}) {
        Session* const session = hub_enabled ? &hub
            : debug_enabled ? &debug
            : settings_enabled ? &settings
            : showcase_enabled ? &showcase
            : nullptr;
        if (session == nullptr || !session->surface.has_value()) return;
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.modifiers = current_modifiers();
        event.text = strata::view(input_text);
        event.timestamp_nanoseconds = now();
        event.key_action = STRATA_KEY_PRESS;
        strata_surface_input_batch_info info{sizeof(strata_surface_input_batch_info)};
        strata::require_ok(
            strata_surface_enqueue_input(session->surface->native_handle(), &event, 1U, &info),
            "desktop input enqueue"
        );
        if (info.accepted_event_count != 1U) {
            throw std::runtime_error("desktop input event was not accepted atomically");
        }
    }

    void flush_pointer_move() {
        if (!pending_pointer_move.has_value()) return;
        const strata_input_event event = *pending_pointer_move;
        pending_pointer_move.reset();
        enqueue(event);
    }

    [[nodiscard]] bool render_session(
        Session& session,
        const strata_surface_frame_info& info,
        std::int64_t* const elapsed_nanos = nullptr,
        DesktopFrameSample* const sample = nullptr
    ) {
        const auto submit_started = std::chrono::steady_clock::now();
        const std::vector<std::uint8_t> encoded = session.surface->render_packet();
        const RenderPacket& packet = session.decoder.decode(encoded);
        const RenderLayerTelemetry render_telemetry = renderer.render_layer(session.id, packet);
        const auto submit_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - submit_started
        ).count();
        if (elapsed_nanos != nullptr) *elapsed_nanos = submit_nanos;
        if (sample != nullptr) {
            sample->frame_index = info.frame_index;
            sample->frame_time_nanos = info.frame_time_nanoseconds;
            sample->input_events = info.processed_input_event_count;
            sample->emitted_events = info.emitted_event_count;
            sample->render_commands = info.render_command_count;
            sample->packet_bytes = info.render_packet_size;
            sample->draw_calls = packet.planned_draw_count;
            sample->batches = packet.batches.size();
            sample->vertices = packet.vertices.size() / 88U;
            sample->blur_passes = render_telemetry.blur_passes;
        }
        strata_profiler_host_frame telemetry{};
        telemetry.struct_size = sizeof(strata_profiler_host_frame);
        telemetry.version = STRATA_PROFILER_HOST_FRAME_VERSION_CURRENT;
        telemetry.draw_calls = packet.planned_draw_count;
        telemetry.batches = packet.batches.size();
        telemetry.vertices = packet.vertices.size() / 88U;
        telemetry.blur_passes = render_telemetry.blur_passes;
        telemetry.blur_target_width = render_telemetry.blur_target_width;
        telemetry.blur_target_height = render_telemetry.blur_target_height;
        telemetry.blur_nanos = render_telemetry.blur_nanos;
        telemetry.submit_nanos = submit_nanos;
        strata::require_ok(
            strata_surface_record_host_frame(session.surface->native_handle(), &telemetry),
            "desktop host profiler publication"
        );
        return info.render_command_count > 0U && packet.planned_draw_count > 0U;
    }

    void frame() {
        const auto host_frame_started = std::chrono::steady_clock::now();
        std::int64_t tooling_nanos = 0;
        flush_pointer_move();
        if (clear_diagnostics_pending && showcase.surface.has_value()) {
            clear_diagnostics_pending = false;
            strata::require_ok(
                strata_surface_clear_diagnostics(showcase.surface->native_handle()),
                "desktop diagnostic clear"
            );
            debug_snapshot_dirty = true;
        }
        for (Session* session : {&showcase, &settings, &debug, &performance_hud, &hub}) {
            if (session->bindings != nullptr) session->bindings->synchronize();
        }
        frame_time = now();
        advance_async(showcase);
        advance_async(settings);
        advance_async(debug);
        advance_async(hub);
        advance_async(performance_hud);
        const auto tooling_update_started = std::chrono::steady_clock::now();
        if (debug_enabled && (debug_snapshot_dirty || frame_time >= next_debug_snapshot)) {
            debug_snapshot_dirty = false;
            publish(debug, "profiler.desktop", debug_json());
            next_debug_snapshot = frame_time + 100'000'000;
        }
        if (performance_hud_enabled &&
            (performance_snapshot_dirty || frame_time >= next_performance_snapshot)) {
            performance_snapshot_dirty = false;
            publish(performance_hud, "performance.desktop", performance_json());
            next_performance_snapshot = frame_time + 100'000'000;
        }
        tooling_nanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - tooling_update_started
        ).count();
        renderer.begin_frame();
        bool had_draws = false;
        std::int64_t showcase_native_nanos = 0;
        std::int64_t showcase_submit_nanos = 0;
        DesktopFrameSample sample;
        if (showcase_enabled && showcase.surface.has_value()) {
            const auto native_started = std::chrono::steady_clock::now();
            const strata_surface_frame_info info = showcase.surface->frame(frame_time);
            showcase_native_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - native_started
            ).count();
            if (rendered_frames == 0U) {
                const std::string canonical_frame = showcase.surface->frame_json();
                first_render_fingerprint = fingerprint(std::span(canonical_frame));
                first_frame_contains_instance =
                    canonical_frame.find(instance_label) != std::string::npos;
            }
            had_draws = render_session(
                showcase, info, &showcase_submit_nanos, &sample
            ) || had_draws;
        }
        if (settings_enabled && settings.surface.has_value()) {
            const strata_surface_frame_info info = settings.surface->frame(frame_time);
            had_draws = render_session(settings, info) || had_draws;
        }
        if (debug_enabled && debug.surface.has_value()) {
            const auto tooling_started = std::chrono::steady_clock::now();
            const strata_surface_frame_info info = debug.surface->frame(frame_time);
            had_draws = render_session(debug, info) || had_draws;
            tooling_nanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - tooling_started
            ).count();
        }
        if (hub_enabled && hub.surface.has_value()) {
            const strata_surface_frame_info info = hub.surface->frame(frame_time);
            had_draws = render_session(hub, info) || had_draws;
        }
        if (performance_hud_enabled && performance_hud.surface.has_value()) {
            const auto tooling_started = std::chrono::steady_clock::now();
            const strata_surface_frame_info info = performance_hud.surface->frame(frame_time);
            had_draws = render_session(performance_hud, info) || had_draws;
            tooling_nanos += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - tooling_started
            ).count();
        }
        const auto present_started = std::chrono::steady_clock::now();
        const bool presented = renderer.end_frame();
        const std::int64_t present_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - present_started
        ).count();
        const std::int64_t total_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - host_frame_started
        ).count();
        host_total_timings.add(total_nanos);
        frame_history.push_back(static_cast<double>(total_nanos) / 1'000'000.0);
        while (frame_history.size() > 48U) frame_history.pop_front();
        showcase_core_timings.add(showcase_native_nanos);
        showcase_submit_timings.add(showcase_submit_nanos);
        tooling_timings.add(tooling_nanos);
        present_timings.add(present_nanos);
        if (profile_sampling_enabled && frame_time >= next_profile_capture) {
            capture_profile_snapshot();
            next_profile_capture = frame_time + 100'000'000;
        }
        record_frame_sample(
            now(),
            total_nanos,
            showcase_native_nanos
        );
        if (rendered_frames == 0U && !showcase_enabled && debug.surface.has_value()) {
            const std::string canonical_frame = debug.surface->frame_json();
            first_render_fingerprint = fingerprint(std::span(canonical_frame));
            first_frame_contains_instance = true;
        }
        sample.total_nanos = total_nanos;
        sample.core_nanos = showcase_native_nanos;
        sample.submit_nanos = showcase_submit_nanos;
        sample.tooling_nanos = tooling_nanos;
        sample.present_nanos = present_nanos;
        sample.had_draws = had_draws;
        sample.presented = presented;
        last_performance_sample = sample;
        ++rendered_frames;
        last_frame_had_draws = had_draws;
    }

    HWND window = nullptr;
    HostServices services;
    Renderer renderer;
    std::string instance_label;
    ShowcaseModel showcase_model;
    DurableWriter durable_writer;
    std::string durable_read_buffer;
    Session showcase;
    Session settings;
    Session debug;
    Session performance_hud;
    Session hub;
    std::deque<FrameBucket> frame_buckets;
    std::deque<ProfileCapture> profile_history;
    TimingWindow host_total_timings;
    TimingWindow showcase_core_timings;
    TimingWindow showcase_submit_timings;
    TimingWindow tooling_timings;
    TimingWindow present_timings;
    DesktopFrameSample last_performance_sample;
    std::optional<strata_input_event> pending_pointer_move;
    std::string debug_mode = "DIAGNOSTICS";
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    std::int64_t frame_time = 0;
    std::int64_t last_total_nanos = 0;
    std::int64_t last_native_nanos = 0;
    std::int64_t next_debug_snapshot = 0;
    std::int64_t next_performance_snapshot = 0;
    std::int64_t next_profile_capture = 0;
    std::uint64_t environment_generation = 1U;
    std::uint64_t rendered_frames = 0U;
    std::uint64_t settings_save_count = 0U;
    strata::host::Revision settings_revision;
    std::uint64_t first_render_fingerprint = 0U;
    std::uint32_t framebuffer_width = 1U;
    std::uint32_t framebuffer_height = 1U;
    double display_scale = 1.0;
    bool showcase_enabled = false;
    bool settings_enabled = false;
    bool debug_enabled = false;
    bool performance_hud_enabled = true;
    bool profile_sampling_enabled = true;
    bool hub_enabled = false;
    std::deque<double> frame_history;
    bool debug_snapshot_dirty = true;
    bool performance_snapshot_dirty = true;
    bool clear_diagnostics_pending = false;
    bool last_frame_had_draws = false;
    bool first_frame_contains_instance = false;
    static constexpr std::array debug_modes{
        std::string_view("DIAGNOSTICS"), std::string_view("SUMMARY"),
        std::string_view("MOTION"), std::string_view("SEMANTICS"),
        std::string_view("HOT_TREE"), std::string_view("SPIKES"),
    };
};

Host::Host(
    HWND window,
    std::filesystem::path resource_root,
    std::string instance_label,
    const HostOptions options
)
    : impl_(std::make_unique<Impl>(
          window, std::move(resource_root), std::move(instance_label), options
      )) {}
Host::~Host() = default;

void Host::resize(
    const std::uint32_t framebuffer_width,
    const std::uint32_t framebuffer_height,
    const double scale
) {
    impl_->resize(framebuffer_width, framebuffer_height, scale);
}

void Host::pointer(
    const std::uint32_t kind,
    const std::int32_t button,
    const double x,
    const double y
) {
    strata_input_event event{};
    event.kind = kind;
    event.pointer_id = 0;
    event.button = button;
    event.x = x / impl_->display_scale;
    event.y = y / impl_->display_scale;
    if (kind == STRATA_INPUT_POINTER_MOVE) {
        impl_->pending_pointer_move = event;
        return;
    }
    impl_->flush_pointer_move();
    impl_->enqueue(event);
}

void Host::scroll(
    const double x,
    const double y,
    const double delta_x,
    const double delta_y
) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_SCROLL;
    event.x = x / impl_->display_scale;
    event.y = y / impl_->display_scale;
    event.delta_x = delta_x;
    event.delta_y = delta_y;
    impl_->enqueue(event);
}

void Host::key(const std::uint32_t virtual_key) {
    impl_->flush_pointer_move();
    if (virtual_key == VK_F6) {
        impl_->toggle_settings();
        return;
    }
    if (virtual_key == VK_F7) {
        impl_->toggle_showcase();
        return;
    }
    if (virtual_key == VK_F8) {
        impl_->cycle_debug((GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return;
    }
    if (virtual_key == VK_F9) {
        impl_->toggle_performance_hud();
        return;
    }
    if (virtual_key == VK_F10) {
        impl_->toggle_hub();
        return;
    }
    const std::string name = key_name(virtual_key);
    strata_input_event event{};
    event.kind = STRATA_INPUT_KEY;
    impl_->enqueue(event, name);
}

void Host::text(std::string utf8) {
    impl_->flush_pointer_move();
    strata_input_event event{};
    event.kind = STRATA_INPUT_TEXT;
    impl_->enqueue(event, utf8);
}

void Host::cancel_interactions() noexcept {
    impl_->pending_pointer_move.reset();
    for (Impl::Session* session : {&impl_->showcase, &impl_->debug}) {
        if (session->surface.has_value()) {
            static_cast<void>(strata_surface_cancel_interactions(
                session->surface->native_handle()
            ));
        }
    }
}

void Host::persist_window_geometry() {
    impl_->remember_window_geometry();
    impl_->durable_writer.flush();
}

void Host::frame() { impl_->frame(); }
double Host::scale() const noexcept { return impl_->display_scale; }

const DesktopFrameSample& Host::last_frame_sample() const noexcept {
    return impl_->last_performance_sample;
}

DesktopHostInfo Host::performance_info() const {
    const RendererInfo renderer = impl_->renderer.info();
    return DesktopHostInfo{
        renderer.adapter,
        renderer.driver_version,
        renderer.vendor_id,
        renderer.device_id,
        renderer.dedicated_video_memory,
        impl_->framebuffer_width,
        impl_->framebuffer_height,
        impl_->logical_width(),
        impl_->logical_height(),
        impl_->display_scale,
        renderer.vsync,
    };
}

std::string Host::performance_frame_json() {
    if (!impl_->showcase.surface.has_value()) {
        throw std::logic_error("desktop performance selectors require an open showcase");
    }
    return impl_->showcase.surface->frame_json();
}

bool Host::smoke_ready() const noexcept {
    return impl_->rendered_frames >= 2U && impl_->last_frame_had_draws;
}

std::uint64_t Host::smoke_fingerprint() const noexcept {
    return impl_->first_render_fingerprint;
}

bool Host::smoke_identity_bound() const noexcept {
    return impl_->first_frame_contains_instance;
}

void Host::reset_profile() {
    impl_->profile_history.clear();
    impl_->host_total_timings.samples.clear();
    impl_->showcase_core_timings.samples.clear();
    impl_->showcase_submit_timings.samples.clear();
    impl_->tooling_timings.samples.clear();
    impl_->present_timings.samples.clear();
    impl_->next_profile_capture = 0;
}

std::string Host::profile_report() {
    impl_->capture_profile_snapshot();
    return impl_->debug_export_text();
}

} // namespace strata::desktop
