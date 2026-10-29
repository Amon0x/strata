#include "font/system_fonts.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace strata::font {
namespace {

/** File names, in order of preference, for the code points of a script or block. */
struct ScriptFonts final {
    std::uint32_t first = 0U;
    std::uint32_t last = 0U;
    std::vector<std::string_view> files;
};

[[nodiscard]] const std::vector<ScriptFonts>& preferred_fonts() {
    static const std::vector<ScriptFonts> table{
        // Emoji and pictographs: colour first.
        {0x1F000U, 0x1FAFFU, {"seguiemj.ttf", "NotoColorEmoji.ttf", "seguisym.ttf"}},
        {0x2600U, 0x27BFU, {"seguiemj.ttf", "seguisym.ttf", "NotoColorEmoji.ttf"}},
        {0x2B00U, 0x2BFFU, {"seguiemj.ttf", "seguisym.ttf"}},
        {0xFE0FU, 0xFE0FU, {"seguiemj.ttf"}},
        {0x200DU, 0x200DU, {"seguiemj.ttf"}},
        // Hebrew and Arabic.
        {0x0590U, 0x05FFU, {"segoeui.ttf", "arial.ttf", "NotoSansHebrew-Regular.ttf"}},
        {0x0600U, 0x08FFU, {"segoeui.ttf", "arial.ttf", "NotoSansArabic-Regular.ttf"}},
        {0xFB1DU, 0xFEFFU, {"segoeui.ttf", "arial.ttf", "NotoSansArabic-Regular.ttf"}},
        // Indic, Thai.
        {0x0900U, 0x0DFFU, {"Nirmala.ttc", "Nirmala.ttf", "NotoSansDevanagari-Regular.ttf"}},
        {0x0E00U, 0x0E7FU, {"leelawui.ttf", "tahoma.ttf"}},
        // Hangul, kana, Han.
        {0x1100U, 0x11FFU, {"malgun.ttf"}},
        {0xAC00U, 0xD7AFU, {"malgun.ttf"}},
        {0x3040U, 0x30FFU, {"YuGothR.ttc", "msgothic.ttc", "msyh.ttc"}},
        {0x3000U, 0x303FU, {"msyh.ttc", "YuGothR.ttc"}},
        {0x3400U, 0x9FFFU, {"msyh.ttc", "YuGothR.ttc", "simsun.ttc"}},
        {0xF900U, 0xFAFFU, {"msyh.ttc", "YuGothR.ttc"}},
        {0xFF00U, 0xFFEFU, {"msyh.ttc", "YuGothR.ttc"}},
        // Symbols, arrows, maths, box drawing.
        {0x2000U, 0x25FFU, {"seguisym.ttf", "segoeui.ttf", "cambria.ttc"}},
    };
    return table;
}

/** Fonts tried for any code point once the preferred ones fail. */
constexpr std::string_view general_fonts[] = {"segoeui.ttf", "arial.ttf", "seguisym.ttf"};

/** An environment variable's value, if set. */
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

class Catalog final {
  public:
    /** Lists the font folders on a worker, once, so the first fallback need not wait for it. */
    void prepare() {
        const std::scoped_lock lock(mutex_);
        if (enumerated_ || preparing_.joinable())
            return;
        preparing_ = std::jthread([this] {
            const std::scoped_lock held(mutex_);
            enumerate();
        });
    }

    void set_directories(std::vector<std::filesystem::path> directories) {
        const std::scoped_lock lock(mutex_);
        directories_ = std::move(directories);
        files_.clear();
        enumerated_ = false;
        loaded_.clear();
        coverage_.clear();
        missing_.clear();
    }

    [[nodiscard]] std::optional<SystemFont> font_for(const std::uint32_t code_point) {
        const std::scoped_lock lock(mutex_);
        if (missing_.contains(code_point))
            return std::nullopt;
        enumerate();
        // Fonts already loaded, then the ones known for its script, then every other one.
        for (auto& [path, face] : loaded_) {
            if (face.glyph_id(code_point) != 0U)
                return SystemFont{id_of(path), face};
        }
        for (const ScriptFonts& script : preferred_fonts()) {
            if (code_point < script.first || code_point > script.last)
                continue;
            for (const std::string_view name : script.files) {
                if (const auto found = try_file(name, code_point))
                    return found;
            }
        }
        for (const std::string_view name : general_fonts) {
            if (const auto found = try_file(name, code_point))
                return found;
        }
        for (const std::filesystem::path& path : files_) {
            if (loaded_.contains(path))
                continue;
            const auto known = coverage_.find(path);
            if (known != coverage_.end()) {
                if (!covers(known->second, code_point))
                    continue;
            } else if (!summarize(path, code_point)) {
                continue;
            }
            if (const auto found = load(path);
                found.has_value() && found->glyph_id(code_point) != 0U)
                return SystemFont{id_of(path), *found};
        }
        missing_.insert(code_point);
        return std::nullopt;
    }

    [[nodiscard]] std::optional<SystemFont> font_named(const std::string_view file) {
        const std::scoped_lock lock(mutex_);
        enumerate();
        const std::optional<std::filesystem::path> path = path_of(file);
        if (!path.has_value())
            return std::nullopt;
        const std::optional<OpenTypeFont> face = load(*path);
        if (!face.has_value())
            return std::nullopt;
        return SystemFont{id_of(*path), *face};
    }

  private:
    using Ranges = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

    [[nodiscard]] static std::string id_of(const std::filesystem::path& path) {
        return "system:" + lower(path.filename().string());
    }

    [[nodiscard]] static bool covers(const Ranges& ranges, const std::uint32_t code_point) {
        const auto found = std::ranges::upper_bound(ranges, code_point, {},
                                                    [](const auto& range) { return range.first; });
        return found != ranges.begin() && std::prev(found)->second >= code_point;
    }

    void enumerate() {
        if (enumerated_)
            return;
        enumerated_ = true;
        if (directories_.empty()) {
#ifdef _WIN32
            directories_.emplace_back(
                std::filesystem::path(environment("WINDIR").value_or("C:\\Windows")) / "Fonts");
            if (const std::optional<std::string> local = environment("LOCALAPPDATA"))
                directories_.emplace_back(std::filesystem::path(*local) / "Microsoft" / "Windows" /
                                          "Fonts");
#else
            directories_ = {"/usr/share/fonts", "/usr/local/share/fonts"};
            if (const std::optional<std::string> home = environment("HOME"))
                directories_.emplace_back(std::filesystem::path(*home) / ".local" / "share" /
                                          "fonts");
#endif
        }
        std::error_code error;
        for (const std::filesystem::path& directory : directories_) {
            for (auto entry = std::filesystem::recursive_directory_iterator(directory, error);
                 !error && entry != std::filesystem::recursive_directory_iterator();
                 entry.increment(error)) {
                const std::string extension = lower(entry->path().extension().string());
                if (extension == ".ttf" || extension == ".ttc")
                    files_.push_back(entry->path());
            }
            error.clear();
        }
        std::ranges::sort(files_);
    }

    [[nodiscard]] std::optional<std::filesystem::path> path_of(const std::string_view name) const {
        const std::string wanted = lower(std::string(name));
        for (const std::filesystem::path& path : files_) {
            if (lower(path.filename().string()) == wanted)
                return path;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<SystemFont> try_file(const std::string_view name,
                                                     const std::uint32_t code_point) {
        const std::optional<std::filesystem::path> path = path_of(name);
        if (!path.has_value())
            return std::nullopt;
        const std::optional<OpenTypeFont> face = load(*path);
        if (!face.has_value() || face->glyph_id(code_point) == 0U)
            return std::nullopt;
        return SystemFont{id_of(*path), *face};
    }

    /** A font file mapped into memory, unmapped when its last font goes. */
    [[nodiscard]] static std::optional<OpenTypeFont> map(const std::filesystem::path& path) {
#ifdef _WIN32
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return std::nullopt;
        LARGE_INTEGER size{};
        const HANDLE mapping =
            GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 64LL * 1024 * 1024
                ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)
                : nullptr;
        CloseHandle(file);
        if (mapping == nullptr)
            return std::nullopt;
        const void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(mapping);
        if (view == nullptr)
            return std::nullopt;
        std::shared_ptr<const void> owner(view,
                                          [](const void* mapped) { UnmapViewOfFile(mapped); });
        return OpenTypeFont::parse_borrowed(
            std::move(owner),
            std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(view),
                                          static_cast<std::size_t>(size.QuadPart)));
#else
        std::optional<resource::ResourceBytes> bytes = read(path);
        if (!bytes.has_value())
            return std::nullopt;
        return OpenTypeFont::parse(std::move(*bytes));
#endif
    }

    [[nodiscard]] static std::optional<resource::ResourceBytes>
    read(const std::filesystem::path& path) {
        // One read of the whole file: a stream iterator a byte at a time takes a hundred
        // milliseconds over a CJK font.
        std::error_code error;
        const std::uintmax_t size = std::filesystem::file_size(path, error);
        if (error || size == 0U || size > 64U * 1024U * 1024U)
            return std::nullopt;
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return std::nullopt;
        resource::ResourceBytes bytes(static_cast<std::size_t>(size));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
            return std::nullopt;
        return bytes;
    }

    /** Loads and keeps a font; a file that is no font of ours is skipped from then on. */
    [[nodiscard]] std::optional<OpenTypeFont> load(const std::filesystem::path& path) {
        if (const auto found = loaded_.find(path); found != loaded_.end())
            return found->second;
        if (coverage_.contains(path) && coverage_.at(path).empty())
            return std::nullopt;
        try {
            std::optional<OpenTypeFont> mapped = map(path);
            if (!mapped.has_value())
                throw FontError("unreadable");
            OpenTypeFont face = std::move(*mapped);
            loaded_.emplace(path, face);
            return face;
        } catch (const std::exception&) {
            coverage_[path] = {};
            return std::nullopt;
        }
    }

    /** Reads a font only for its coverage, kept instead of the font; whether it covers. */
    [[nodiscard]] bool summarize(const std::filesystem::path& path,
                                 const std::uint32_t code_point) {
        try {
            const std::optional<OpenTypeFont> face = map(path);
            if (!face.has_value())
                throw FontError("unreadable");
            Ranges ranges = face->coverage_ranges();
            const bool covered = covers(ranges, code_point);
            coverage_[path] = std::move(ranges);
            return covered;
        } catch (const std::exception&) {
            coverage_[path] = {};
            return false;
        }
    }

    std::mutex mutex_;
    std::jthread preparing_;
    std::vector<std::filesystem::path> directories_;
    std::vector<std::filesystem::path> files_;
    bool enumerated_ = false;
    std::map<std::filesystem::path, OpenTypeFont> loaded_;
    std::map<std::filesystem::path, Ranges> coverage_;
    std::set<std::uint32_t> missing_;
};

[[nodiscard]] Catalog& catalog() {
    static Catalog instance;
    return instance;
}

} // namespace

void set_system_font_directories(std::vector<std::filesystem::path> directories) {
    catalog().set_directories(std::move(directories));
}

void prepare_system_fonts() {
    catalog().prepare();
}

std::optional<SystemFont> system_font_for(const std::uint32_t code_point) {
    return catalog().font_for(code_point);
}

std::optional<SystemFont> system_font_named(const std::string_view file) {
    return catalog().font_named(file);
}

} // namespace strata::font
