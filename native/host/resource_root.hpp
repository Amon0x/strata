#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <strata/strata.h>

namespace strata::host {

/** A resource id that names no file below its root. */
class ResourceNotFound final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/**
 * Host resource files below one root directory, shared by the desktop and headless hosts.
 *
 * Ids are relative UTF-8 paths. An id that is absolute, drive- or root-relative, or contains a
 * parent segment is rejected lexically, and the file it names is then resolved through symbolic
 * links and junctions and must still lie inside the root's own resolved location, so a link inside
 * the root cannot reach outside it. `system-font:` ids resolve installed fonts instead.
 *
 * Successful loads are cached for the adapter's borrowed-bytes contract; failures are not, so a
 * file that appears later loads on the next request.
 */
class ResourceRoot final {
  public:
    explicit ResourceRoot(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept;

    /** Resolves `relative` inside the root; throws std::invalid_argument when it escapes. */
    [[nodiscard]] std::filesystem::path resolve(const std::filesystem::path& relative) const;
    /** Resolves a UTF-8 resource id inside the root; throws std::invalid_argument on escape. */
    [[nodiscard]] std::filesystem::path resolve(std::string_view id) const;

    /** Reads one file; throws ResourceNotFound when it is absent. */
    [[nodiscard]] std::vector<std::uint8_t> read(const std::filesystem::path& relative) const;
    [[nodiscard]] std::string read_text(const std::filesystem::path& relative) const;

    /**
     * Resource-adapter body. Missing or empty resources are NOT_FOUND, escaping or malformed ids
     * are INVALID_ARGUMENT, allocation failure is OUT_OF_MEMORY, and I/O failure on an existing
     * file is SERVICE_UNAVAILABLE. The returned bytes stay valid until the root is destroyed.
     */
    [[nodiscard]] strata_status load(std::string_view id, strata_bytes_view& output) noexcept;

  private:
    std::filesystem::path root_;
    std::filesystem::path resolved_root_;
    std::map<std::string, std::vector<std::uint8_t>, std::less<>> cache_;
};

/** Converts a UTF-8 resource id to a path without the platform's narrow code page. */
[[nodiscard]] std::filesystem::path utf8_path(std::string_view value);

} // namespace strata::host
