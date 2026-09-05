#include "renderer_internal.hpp"
#include <cstring>
#include <fstream>

namespace strata::vulkan {
namespace {
constexpr std::size_t max_cache_bytes = 64 * 1024 * 1024;
constexpr std::array<char, 8> magic{'S', 'T', 'R', 'V', 'K', 'C', '0', '1'};
std::uint64_t checksum(std::span<const char> bytes) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        value ^= static_cast<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}
// This is a local cache envelope, not a portable interchange format. Vulkan's
// cache header additionally verifies the GPU and pipelineCacheUUID (driver).
struct Header {
    std::array<char, 8> signature;
    std::uint64_t size;
    std::uint64_t hash;
};
} // namespace

std::size_t Renderer::prepare(VkFormat format) {
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM &&
        format != VK_FORMAT_R16G16B16A16_SFLOAT)
        throw std::invalid_argument("Vulkan target requires RGBA8/BGRA8 UNORM or RGBA16F");
    const auto before = pipeline_count();
    constexpr std::array blends{"opaque",   "straight_alpha", "premultiplied_alpha",
                                "additive", "multiply",       "rounded_multiply"};
    for (const auto blend : blends) {
        impl_->pipeline("builtin", blend, format, false);
        for (const auto& [id, program] : impl_->materials) {
            (void)id;
            impl_->pipeline(program, blend, format, false);
        }
    }
    impl_->pipeline("blur", "opaque", format, true);
    impl_->pipeline("composite", "premultiplied_alpha", format, true);
    for (const auto& [id, passes] : impl_->effects) {
        (void)id;
        for (const auto& [index, pass] : passes) {
            (void)index;
            if (pass.kind == STRATA_EFFECT_PASS_SHADER)
                impl_->pipeline(pass.program, "opaque", format, true);
        }
    }
    return pipeline_count() - before;
}

std::size_t Renderer::pipeline_count() const noexcept {
    return impl_->pipelines.size();
}

bool Renderer::load_pipeline_cache(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return false;
    const auto length = file.tellg();
    if (length < static_cast<std::streamoff>(sizeof(Header)) ||
        length > static_cast<std::streamoff>(max_cache_bytes + sizeof(Header)))
        return false;
    file.seekg(0);
    Header envelope{};
    file.read(reinterpret_cast<char*>(&envelope), sizeof(envelope));
    if (!file || envelope.signature != magic ||
        envelope.size != static_cast<std::uint64_t>(length) - sizeof(Header) ||
        envelope.size < sizeof(VkPipelineCacheHeaderVersionOne))
        return false;
    std::vector<char> bytes(static_cast<std::size_t>(envelope.size));
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file || checksum(bytes) != envelope.hash)
        return false;
    VkPipelineCacheHeaderVersionOne header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(impl_->device.physical_device, &properties);
    if (header.headerSize != sizeof(header) ||
        header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        header.vendorID != properties.vendorID || header.deviceID != properties.deviceID ||
        std::memcmp(header.pipelineCacheUUID, properties.pipelineCacheUUID, VK_UUID_SIZE) != 0)
        return false;
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = bytes.size();
    info.pInitialData = bytes.data();
    VkPipelineCache imported{};
    check(vkCreatePipelineCache(impl_->device.device, &info, nullptr, &imported),
          "import pipeline cache");
    const auto result =
        vkMergePipelineCaches(impl_->device.device, impl_->pipeline_cache, 1, &imported);
    vkDestroyPipelineCache(impl_->device.device, imported, nullptr);
    check(result, "merge pipeline cache");
    return true;
}

bool Renderer::save_pipeline_cache(const std::filesystem::path& path) const {
    std::size_t size = 0;
    check(vkGetPipelineCacheData(impl_->device.device, impl_->pipeline_cache, &size, nullptr),
          "query pipeline cache");
    if (size > max_cache_bytes)
        return false;
    std::vector<char> bytes(size);
    check(vkGetPipelineCacheData(impl_->device.device, impl_->pipeline_cache, &size, bytes.data()),
          "read pipeline cache");
    bytes.resize(size);
    const Header header{magic, size, checksum(bytes)};
    auto temporary = path;
    temporary += ".tmp";
    std::error_code error;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (file)
        std::filesystem::rename(temporary, path, error);
    if (!file || error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}
} // namespace strata::vulkan
