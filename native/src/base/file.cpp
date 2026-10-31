#include "base/file.hpp"

#include <cstdio>
#include <memory>
#include <system_error>

namespace strata::base {

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path) {
    std::FILE* opened = nullptr;
#if defined(_WIN32)
    if (_wfopen_s(&opened, path.c_str(), L"rb") != 0)
        opened = nullptr;
#else
    opened = std::fopen(path.c_str(), "rb");
#endif
    if (opened == nullptr)
        return std::nullopt;
    const std::unique_ptr<std::FILE, decltype(&std::fclose)> file(opened, &std::fclose);
    // Unbuffered, a read of the whole file goes straight to the system.
    static_cast<void>(std::setvbuf(file.get(), nullptr, _IONBF, 0U));
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > std::vector<std::uint8_t>{}.max_size())
        return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && std::fread(bytes.data(), 1U, bytes.size(), file.get()) != bytes.size())
        return std::nullopt;
    return bytes;
}

} // namespace strata::base
