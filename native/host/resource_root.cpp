#include "host/resource_root.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <system_error>
#include <utility>

#include "host/system_fonts.hpp"

namespace strata::host {
namespace {

/** Resolves links and junctions; the missing tail of a path is kept lexically. */
[[nodiscard]] std::filesystem::path resolved(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, error);
    if (error)
        result = std::filesystem::absolute(path).lexically_normal();
    // "C:\root\" iterates with a trailing empty element that no child path shares.
    if (!result.has_filename() && result.has_parent_path() && result != result.root_path())
        result = result.parent_path();
    return result;
}

[[nodiscard]] bool contains(const std::filesystem::path& root, const std::filesystem::path& path) {
    return std::mismatch(root.begin(), root.end(), path.begin(), path.end()).first == root.end();
}

[[nodiscard]] std::string display(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

} // namespace

std::filesystem::path utf8_path(const std::string_view value) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

ResourceRoot::ResourceRoot(std::filesystem::path root)
    : root_(std::move(root)), resolved_root_(resolved(root_)) {}

const std::filesystem::path& ResourceRoot::root() const noexcept {
    return root_;
}

std::filesystem::path ResourceRoot::resolve(const std::filesystem::path& relative) const {
    // On Windows "/x" (root directory, no drive) and "D:x" (drive-relative) are not absolute, yet
    // either one joined to the resource root leaves it.
    if (relative.empty() || relative.has_root_name() || relative.has_root_directory())
        throw std::invalid_argument("resource id must be relative: " + display(relative));
    for (const std::filesystem::path& component : relative) {
        if (component == "..")
            throw std::invalid_argument("resource id escapes its root: " + display(relative));
    }
    const std::filesystem::path file = resolved(root_ / relative);
    if (!contains(resolved_root_, file)) {
        throw std::invalid_argument("resource id resolves outside its root through a link: " +
                                    display(relative));
    }
    return file;
}

std::filesystem::path ResourceRoot::resolve(const std::string_view id) const {
    return resolve(utf8_path(id));
}

std::vector<std::uint8_t> ResourceRoot::read(const std::filesystem::path& relative) const {
    const std::filesystem::path path = resolve(relative);
    std::error_code status_error;
    if (!std::filesystem::is_regular_file(path, status_error))
        throw ResourceNotFound("resource not found: " + display(relative));
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("could not open resource: " + display(relative));
    const std::streampos end = input.tellg();
    if (end < 0)
        throw std::runtime_error("could not size resource: " + display(relative));
    const auto size = static_cast<std::uint64_t>(end);
    if (size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) ||
        size > std::vector<std::uint8_t>{}.max_size()) {
        throw std::length_error("resource is too large: " + display(relative));
    }
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    if (!result.empty()) {
        input.read(reinterpret_cast<char*>(result.data()),
                   static_cast<std::streamsize>(result.size()));
    }
    if (!input)
        throw std::runtime_error("could not read resource: " + display(relative));
    return result;
}

std::string ResourceRoot::read_text(const std::filesystem::path& relative) const {
    const std::vector<std::uint8_t> bytes = read(relative);
    return std::string(bytes.begin(), bytes.end());
}

strata_status ResourceRoot::load(const std::string_view id, strata_bytes_view& output) noexcept {
    output = strata_bytes_view{nullptr, 0U};
    try {
        auto found = cache_.find(id);
        if (found == cache_.end()) {
            std::optional<std::vector<std::uint8_t>> bytes = read_system_font(id);
            if (!bytes.has_value())
                bytes = read(utf8_path(id));
            if (bytes->empty())
                return STRATA_STATUS_NOT_FOUND;
            found = cache_.emplace(std::string(id), std::move(*bytes)).first;
        }
        output = strata_bytes_view{found->second.data(), found->second.size()};
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (const ResourceNotFound&) {
        return STRATA_STATUS_NOT_FOUND;
    } catch (const std::invalid_argument&) {
        return STRATA_STATUS_INVALID_ARGUMENT;
    } catch (...) {
        return STRATA_STATUS_SERVICE_UNAVAILABLE;
    }
}

} // namespace strata::host
