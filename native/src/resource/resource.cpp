#include "resource/resource.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "core/utf8.hpp"

namespace strata::resource {
namespace {

[[nodiscard]] bool within_root(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate
) {
    auto root_part = root.begin();
    auto candidate_part = candidate.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *candidate_part != *root_part) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::filesystem::path resolve_resource(
    const std::filesystem::path& root,
    const ResourceId& resource,
    const ResourceLimits& limits,
    std::uintmax_t& size
) {
    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error) {
        throw ResourceError("resource root cannot be resolved");
    }
    const std::filesystem::path candidate =
        std::filesystem::weakly_canonical(canonical_root / resource.relative_path(), error);
    if (error || !within_root(canonical_root, candidate)) {
        throw ResourceError("resource path escapes its configured root");
    }
    size = std::filesystem::file_size(candidate, error);
    if (error) {
        throw ResourceError("resource does not exist or is not a regular file");
    }
    if (size > limits.maximum_bytes) {
        throw ResourceError("resource exceeds the configured byte limit");
    }
    return candidate;
}

} // namespace

ResourceId::ResourceId(std::string value) : value_(std::move(value)) {}

ResourceId ResourceId::parse(const std::string_view value) {
    if (value.empty()) {
        throw ResourceError("resource identity must not be empty");
    }
    if (!strata::core::valid_utf8(value)) {
        throw ResourceError("resource identity must be valid UTF-8");
    }
    if (value.front() == '/' || value.back() == '/' || value.contains('\\') ||
        value.contains('\0') || value.contains(':')) {
        throw ResourceError("resource identity must be a relative forward-slash path");
    }
    std::size_t segment_start = 0U;
    while (segment_start < value.size()) {
        const std::size_t separator = value.find('/', segment_start);
        const std::size_t segment_end =
            separator == std::string_view::npos ? value.size() : separator;
        const std::string_view segment = value.substr(segment_start, segment_end - segment_start);
        if (segment.empty() || segment == "." || segment == "..") {
            throw ResourceError("resource identity contains an unsafe path segment");
        }
        segment_start = segment_end + 1U;
    }
    return ResourceId(std::string(value));
}

const std::string& ResourceId::value() const noexcept {
    return value_;
}

std::filesystem::path ResourceId::relative_path() const {
    std::filesystem::path result;
    std::size_t segment_start = 0U;
    while (segment_start < value_.size()) {
        const std::size_t separator = value_.find('/', segment_start);
        const std::size_t segment_end = separator == std::string::npos ? value_.size() : separator;
        const auto* const utf8_begin = reinterpret_cast<const char8_t*>(
            value_.data() + segment_start
        );
        const std::u8string segment(utf8_begin, utf8_begin + (segment_end - segment_start));
        result /= std::filesystem::path(segment);
        segment_start = segment_end + 1U;
    }
    return result;
}

ResourceBytes load_binary_resource(
    const std::filesystem::path& root,
    const ResourceId& resource,
    const ResourceLimits& limits
) {
    std::uintmax_t file_size = 0U;
    const std::filesystem::path candidate = resolve_resource(root, resource, limits, file_size);
    std::ifstream stream(candidate, std::ios::binary);
    if (!stream) {
        throw ResourceError("resource cannot be opened");
    }
    ResourceBytes bytes(static_cast<std::size_t>(file_size));
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    if (!stream && !stream.eof()) {
        throw ResourceError("resource could not be read completely");
    }
    return bytes;
}

std::string load_utf8_resource(
    const std::filesystem::path& root,
    const ResourceId& resource,
    const ResourceLimits& limits
) {
    const ResourceBytes binary = load_binary_resource(root, resource, limits);
    const std::string bytes(binary.begin(), binary.end());
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        throw ResourceError("UTF-8 byte-order marks are not permitted in Strata resources");
    }
    if (!strata::core::valid_utf8(bytes)) {
        throw ResourceError("resource is not valid UTF-8");
    }
    return bytes;
}

} // namespace strata::resource
