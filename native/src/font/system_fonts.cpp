#include "font/system_fonts.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <compare>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "base/file.hpp"
#include "base/task_pool.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace strata::font {
namespace {

/** Inline candidates avoid initializer-list backing storage and dynamic initialization. */
struct ScriptFonts final {
    std::uint32_t first;
    std::uint32_t last;
    std::array<std::string_view, 3U> files;
};

constexpr ScriptFonts preferred_fonts[]{
    {0x1F000U, 0x1FAFFU, {"seguiemj.ttf", "NotoColorEmoji.ttf", "seguisym.ttf"}},
    {0x2600U, 0x27BFU, {"seguiemj.ttf", "seguisym.ttf", "NotoColorEmoji.ttf"}},
    {0x2B00U, 0x2BFFU, {"seguiemj.ttf", "seguisym.ttf"}},
    {0xFE0FU, 0xFE0FU, {"seguiemj.ttf"}},
    {0x200DU, 0x200DU, {"seguiemj.ttf"}},
    {0x0590U, 0x05FFU, {"segoeui.ttf", "arial.ttf", "NotoSansHebrew-Regular.ttf"}},
    {0x0600U, 0x08FFU, {"segoeui.ttf", "arial.ttf", "NotoSansArabic-Regular.ttf"}},
    {0xFB1DU, 0xFEFFU, {"segoeui.ttf", "arial.ttf", "NotoSansArabic-Regular.ttf"}},
    {0x0900U, 0x0DFFU, {"Nirmala.ttc", "Nirmala.ttf", "NotoSansDevanagari-Regular.ttf"}},
    {0x0E00U, 0x0E7FU, {"leelawui.ttf", "tahoma.ttf"}},
    {0x1100U, 0x11FFU, {"malgun.ttf"}},
    {0xAC00U, 0xD7AFU, {"malgun.ttf"}},
    {0x3040U, 0x30FFU, {"YuGothR.ttc", "msgothic.ttc", "msyh.ttc"}},
    {0x3000U, 0x303FU, {"msyh.ttc", "YuGothR.ttc"}},
    {0x3400U, 0x9FFFU, {"msyh.ttc", "YuGothR.ttc", "simsun.ttc"}},
    {0xF900U, 0xFAFFU, {"msyh.ttc", "YuGothR.ttc"}},
    {0xFF00U, 0xFFEFU, {"msyh.ttc", "YuGothR.ttc"}},
    {0x2000U, 0x25FFU, {"seguisym.ttf", "segoeui.ttf", "cambria.ttc"}},
};

constexpr std::string_view general_fonts[]{"segoeui.ttf", "arial.ttf", "seguisym.ttf"};

[[nodiscard]] std::optional<std::string> environment(const char* const name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0U;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
        return std::nullopt;
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* const value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

[[nodiscard]] std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

/**
 * A retained catalog generation. Shared indexes and faces are published under short locks; all
 * traversal, mapping, parsing and coverage extraction happens outside them. Prefetch jobs retain
 * State, never this wrapper, so dropping the last client cancels without joining a worker.
 */
struct SystemFontCatalog final {
    struct State final {
        using Ranges = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
        struct Files final {
            std::vector<std::filesystem::path> ordered;
            std::map<std::string, std::size_t, std::less<>> names;
        };
        struct Named final {
            std::filesystem::path path;
            std::string family;
            std::string full;
            std::string postscript;
            int weight = 400;
            bool italic = false;
        };
        struct FamilyKey final {
            std::string name;
            std::string family;
            int weight;
            bool italic;
            [[nodiscard]] friend auto operator<=>(const FamilyKey&, const FamilyKey&) = default;
        };

        explicit State(std::vector<std::filesystem::path> roots) : directories(std::move(roots)) {}

        /**
         * A file's name in UTF-8. Some names have no spelling in it (a lone surrogate on Windows),
         * and one unreadable file name must not cost the application every system font.
         */
        [[nodiscard]] static std::optional<std::string> name_of(const std::filesystem::path& path) {
            try {
                const std::u8string name = path.filename().u8string();
                return std::string(name.begin(), name.end());
            } catch (const std::system_error&) {
                return std::nullopt;
            }
        }

        [[nodiscard]] std::shared_ptr<const Files> files() {
            {
                const std::scoped_lock lock(mutex);
                if (file_index != nullptr)
                    return file_index;
            }
            auto result = std::make_shared<Files>();
            std::vector<std::filesystem::path> roots = directories;
            if (roots.empty()) {
#ifdef _WIN32
                roots.emplace_back(
                    std::filesystem::path(environment("WINDIR").value_or("C:\\Windows")) / "Fonts");
                if (const auto local = environment("LOCALAPPDATA"))
                    roots.emplace_back(std::filesystem::path(*local) / "Microsoft" / "Windows" /
                                       "Fonts");
#else
                roots = {"/usr/share/fonts", "/usr/local/share/fonts"};
                if (const auto home = environment("HOME"))
                    roots.emplace_back(std::filesystem::path(*home) / ".local" / "share" / "fonts");
#endif
            }
            std::error_code error;
            for (const auto& directory : roots) {
                for (auto entry = std::filesystem::recursive_directory_iterator(directory, error);
                     !error && entry != std::filesystem::recursive_directory_iterator();
                     entry.increment(error)) {
                    const std::optional<std::string> name = name_of(entry->path());
                    if (!name.has_value())
                        continue;
                    const std::string lowered = lower(*name);
                    if (lowered.ends_with(".ttf") || lowered.ends_with(".ttc"))
                        result->ordered.push_back(entry->path());
                }
                error.clear();
            }
            std::ranges::sort(result->ordered);
            result->ordered.erase(std::ranges::unique(result->ordered).begin(), result->ordered.end());
            for (std::size_t index = 0U; index < result->ordered.size(); ++index)
                result->names.emplace(lower(*name_of(result->ordered[index])), index);
            const std::scoped_lock lock(mutex);
            if (file_index == nullptr)
                file_index = std::move(result);
            return file_index;
        }

        [[nodiscard]] static const std::filesystem::path*
        path_of(const Files& index, const std::string_view name) {
            auto found = index.names.find(name);
            if (found == index.names.end())
                found = index.names.find(lower(std::string(name)));
            return found == index.names.end() ? nullptr : &index.ordered[found->second];
        }

        [[nodiscard]] static std::string id_of(const std::filesystem::path& path) {
            return "system:" + lower(*name_of(path));
        }

        /** A mapped face retains its mapping independently of this catalog. */
        [[nodiscard]] static std::optional<OpenTypeFont> map(const std::filesystem::path& path) {
#ifdef _WIN32
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return std::nullopt;
            LARGE_INTEGER size{};
            const HANDLE mapping =
                GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 64LL * 1024 * 1024
                    ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
            CloseHandle(file);
            if (mapping == nullptr)
                return std::nullopt;
            const void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            CloseHandle(mapping);
            if (view == nullptr)
                return std::nullopt;
            std::shared_ptr<const void> owner(view, [](const void* mapped) { UnmapViewOfFile(mapped); });
            return OpenTypeFont::parse_borrowed(
                std::move(owner), std::span(static_cast<const std::uint8_t*>(view),
                                           static_cast<std::size_t>(size.QuadPart)));
#else
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error || size == 0U || size > 64U * 1024U * 1024U)
                return std::nullopt;
            auto bytes = base::read_file(path);
            if (!bytes.has_value())
                return std::nullopt;
            return OpenTypeFont::parse(std::move(*bytes));
#endif
        }

        [[nodiscard]] static std::optional<OpenTypeFont> read_face(const std::filesystem::path& path) {
            try {
                return map(path);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }

        [[nodiscard]] std::optional<OpenTypeFont> load(const std::filesystem::path& path) {
            {
                const std::scoped_lock lock(mutex);
                if (const auto found = loaded.find(path); found != loaded.end())
                    return found->second;
            }
            auto face = read_face(path);
            const std::scoped_lock lock(mutex);
            return loaded.try_emplace(path, std::move(face)).first->second;
        }

        [[nodiscard]] static bool covers(const Ranges& ranges, const std::uint32_t code_point) {
            const auto found = std::ranges::upper_bound(ranges, code_point, {},
                                                        [](const auto& range) { return range.first; });
            return found != ranges.begin() && std::prev(found)->second >= code_point;
        }

        [[nodiscard]] std::optional<OpenTypeFont>
        covering(const std::filesystem::path& path, const std::uint32_t code_point) {
            std::optional<OpenTypeFont> known;
            std::shared_ptr<const Ranges> ranges;
            bool loaded_before = false;
            {
                const std::scoped_lock lock(mutex);
                if (const auto found = loaded.find(path); found != loaded.end()) {
                    known = found->second;
                    loaded_before = true;
                }
                if (const auto found = coverage.find(path); found != coverage.end())
                    ranges = found->second;
            }
            if (loaded_before)
                return known.has_value() && known->glyph_id(code_point) != 0U ? known : std::nullopt;
            if (ranges != nullptr) {
                if (!covers(*ranges, code_point))
                    return std::nullopt;
                auto face = load(path);
                return face.has_value() && face->glyph_id(code_point) != 0U ? face : std::nullopt;
            }
            auto face = read_face(path);
            if (!face.has_value()) {
                const std::scoped_lock lock(mutex);
                loaded.try_emplace(path, std::nullopt);
                return std::nullopt;
            }
            auto prepared = std::make_shared<const Ranges>(face->coverage_ranges());
            const bool covered = covers(*prepared, code_point);
            const std::scoped_lock lock(mutex);
            coverage.try_emplace(path, std::move(prepared));
            if (covered)
                return loaded.try_emplace(path, std::move(face)).first->second;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<SystemFont> font_for(const std::uint32_t code_point) {
            {
                const std::scoped_lock lock(mutex);
                if (missing.contains(code_point))
                    return std::nullopt;
            }
            const auto index = files();
            const auto try_name = [&](const std::string_view name) -> std::optional<SystemFont> {
                const auto* path = path_of(*index, name);
                if (path != nullptr) {
                    if (auto face = covering(*path, code_point))
                        return SystemFont{id_of(*path), std::move(*face)};
                }
                return std::nullopt;
            };
            // Cache population never changes precedence: concurrently loaded broad-coverage fonts
            // must not replace a preferred color/script face merely by finishing first.
            for (const ScriptFonts& script : preferred_fonts) {
                if (code_point < script.first || code_point > script.last)
                    continue;
                for (const auto name : script.files) {
                    if (name.empty())
                        break;
                    if (auto found = try_name(name))
                        return found;
                }
            }
            for (const auto name : general_fonts) {
                if (auto found = try_name(name))
                    return found;
            }
            for (const auto& path : index->ordered) {
                if (auto face = covering(path, code_point))
                    return SystemFont{id_of(path), std::move(*face)};
            }
            const std::scoped_lock lock(mutex);
            missing.insert(code_point);
            return std::nullopt;
        }

        [[nodiscard]] std::optional<SystemFont> font_named(const std::string_view file) {
            const auto index = files();
            const auto* path = path_of(*index, file);
            if (path == nullptr)
                return std::nullopt;
            if (auto face = load(*path))
                return SystemFont{id_of(*path), std::move(*face)};
            return std::nullopt;
        }

        [[nodiscard]] std::shared_ptr<const std::vector<Named>> names() {
            {
                const std::scoped_lock lock(mutex);
                if (name_index != nullptr)
                    return name_index;
            }
            const auto index = files();
            auto result = std::make_shared<std::vector<Named>>();
            for (const auto& path : index->ordered) {
                const auto face = read_face(path);
                if (!face.has_value())
                    continue;
                const auto& metadata = face->metadata();
                const std::string subfamily = lower(metadata.subfamily);
                result->push_back(Named{
                    path, lower(metadata.family), lower(metadata.full_name),
                    lower(metadata.postscript_name),
                    metadata.weight_class != 0U ? static_cast<int>(metadata.weight_class)
                    : (metadata.style_flags & font_style_bold) != 0U ? 700 : 400,
                    (metadata.style_flags & font_style_italic) != 0U ||
                        subfamily.find("italic") != std::string::npos ||
                        subfamily.find("oblique") != std::string::npos,
                });
            }
            const std::scoped_lock lock(mutex);
            if (name_index == nullptr)
                name_index = std::move(result);
            return name_index;
        }

        [[nodiscard]] std::optional<SystemFont> font_family(const std::string_view name,
                                                            const std::string_view family,
                                                            const int weight, const bool italic) {
            FamilyKey key{lower(std::string(name)), lower(std::string(family)), weight, italic};
            {
                const std::scoped_lock lock(mutex);
                if (const auto found = families.find(key); found != families.end())
                    return found->second;
            }
            const auto named = names();
            const Named* best = nullptr;
            int best_score = std::numeric_limits<int>::max();
            for (const Named& entry : *named) {
                int score = 0;
                if (!key.name.empty() &&
                    (entry.postscript == key.name || entry.full == key.name)) {
                    score = -1;
                } else if (!key.family.empty() && entry.family == key.family) {
                    score = std::abs(entry.weight - weight) + (entry.italic != italic ? 1000 : 0);
                } else {
                    continue;
                }
                if (score < best_score) {
                    best_score = score;
                    best = &entry;
                }
            }
            std::optional<SystemFont> result;
            if (best != nullptr) {
                if (auto face = load(best->path))
                    result = SystemFont{id_of(best->path), std::move(*face)};
            }
            const std::scoped_lock lock(mutex);
            return families.try_emplace(std::move(key), std::move(result)).first->second;
        }

        const std::vector<std::filesystem::path> directories;
        std::mutex mutex;
        std::shared_ptr<const Files> file_index;
        std::shared_ptr<const std::vector<Named>> name_index;
        std::map<std::filesystem::path, std::optional<OpenTypeFont>> loaded;
        std::map<std::filesystem::path, std::shared_ptr<const Ranges>> coverage;
        std::map<FamilyKey, std::optional<SystemFont>> families;
        std::set<std::uint32_t> missing;
        bool enumerating = false;
    };

    explicit SystemFontCatalog(std::vector<std::filesystem::path> directories = {})
        : state(std::make_shared<State>(std::move(directories))) {}

    void prepare() {
        const std::scoped_lock serialized(submissions);
        static_cast<void>(tasks.drain());
        {
            const std::scoped_lock lock(state->mutex);
            if (state->file_index != nullptr || state->enumerating)
                return;
            state->enumerating = true;
        }
        try {
            static_cast<void>(tasks.post(
                base::TaskKind::font, base::TaskPriority::prefetch,
                [retained = state](const base::TaskCancellation& cancellation) {
                    if (cancellation.cancelled())
                        return;
                    static_cast<void>(retained->files());
                },
                // This only looks ahead. A failure, such as running out of memory, is not kept:
                // whoever next needs the fonts tries again and hears about it then.
                [retained = state](const base::TaskOutcome&) {
                    const std::scoped_lock lock(retained->mutex);
                    retained->enumerating = false;
                }));
        } catch (...) {
            const std::scoped_lock lock(state->mutex);
            state->enumerating = false;
            throw;
        }
    }

    std::shared_ptr<State> state;
    std::mutex submissions;
    base::TaskOwner tasks;
};

namespace {

/** Process-lived admission state; callers pin a generation before queuing detached work. */
struct CatalogSource final {
    std::mutex mutex;
    std::shared_ptr<SystemFontCatalog> current = std::make_shared<SystemFontCatalog>();
};

[[nodiscard]] CatalogSource& catalog_source() {
    static CatalogSource& source = *new CatalogSource;
    return source;
}

} // namespace

std::shared_ptr<SystemFontCatalog> system_font_catalog() {
    CatalogSource& source = catalog_source();
    const std::scoped_lock lock(source.mutex);
    return source.current;
}

void set_system_font_directories(std::vector<std::filesystem::path> directories) {
    auto next = std::make_shared<SystemFontCatalog>(std::move(directories));
    CatalogSource& source = catalog_source();
    {
        const std::scoped_lock lock(source.mutex);
        source.current.swap(next);
    }
}

void prepare_system_fonts(const std::shared_ptr<SystemFontCatalog>& catalog) {
    catalog->prepare();
}

std::optional<SystemFont> system_font_for(const std::shared_ptr<SystemFontCatalog>& catalog,
                                         const std::uint32_t code_point) {
    return catalog->state->font_for(code_point);
}

std::optional<SystemFont> system_font_named(const std::shared_ptr<SystemFontCatalog>& catalog,
                                           const std::string_view file) {
    return catalog->state->font_named(file);
}

std::optional<SystemFont> system_font_family(const std::shared_ptr<SystemFontCatalog>& catalog,
                                             const std::string_view name,
                                             const std::string_view family, const int weight,
                                             const bool italic) {
    return catalog->state->font_family(name, family, weight, italic);
}

} // namespace strata::font
