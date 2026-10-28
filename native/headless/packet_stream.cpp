#include "packet_stream.hpp"

#include <bit>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace strata::headless {
namespace {

constexpr std::string_view magic = "STRATAPS";
constexpr std::uint32_t version = 1U;

template <typename Integer>
void put(std::vector<std::uint8_t>& output, const Integer value) {
    for (std::size_t byte = 0U; byte < sizeof(Integer); ++byte)
        output.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (byte * 8U)));
}

class Input final {
  public:
    explicit Input(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    [[nodiscard]] std::uint64_t integer(const std::size_t size) {
        require(size);
        std::uint64_t value = 0U;
        for (std::size_t byte = 0U; byte < size; ++byte)
            value |= static_cast<std::uint64_t>(bytes_[offset_ + byte]) << (byte * 8U);
        offset_ += size;
        return value;
    }
    [[nodiscard]] std::vector<std::uint8_t> raw(const std::size_t size) {
        require(size);
        std::vector<std::uint8_t> result(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                                         bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += size;
        return result;
    }
    [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }

  private:
    void require(const std::size_t size) const {
        if (size > bytes_.size() - offset_)
            throw std::invalid_argument("packet stream is truncated");
    }
    std::vector<std::uint8_t> bytes_;
    std::size_t offset_ = 0U;
};

} // namespace

void write_packet_stream(const std::filesystem::path& path,
                         const std::span<const RecordedPacket> frames) {
    std::vector<std::uint8_t> output(magic.begin(), magic.end());
    put(output, version);
    put(output, static_cast<std::uint32_t>(frames.size()));
    for (const RecordedPacket& frame : frames) {
        put(output, static_cast<std::uint64_t>(frame.time_nanoseconds));
        put(output, frame.framebuffer_width);
        put(output, frame.framebuffer_height);
        put(output, std::bit_cast<std::uint64_t>(frame.logical_width));
        put(output, std::bit_cast<std::uint64_t>(frame.logical_height));
        put(output, static_cast<std::uint32_t>(frame.bytes.size()));
        output.insert(output.end(), frame.bytes.begin(), frame.bytes.end());
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(output.data()),
               static_cast<std::streamsize>(output.size()));
    if (!file)
        throw std::runtime_error("packet stream could not be written: " + path.string());
}

std::vector<RecordedPacket> read_packet_stream(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("packet stream could not be opened: " + path.string());
    Input input(std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {}));
    const std::vector<std::uint8_t> header = input.raw(magic.size());
    if (std::string_view(reinterpret_cast<const char*>(header.data()), header.size()) != magic ||
        input.integer(4U) != version) {
        throw std::invalid_argument("not a version 1 Strata packet stream");
    }
    const std::uint64_t count = input.integer(4U);
    std::vector<RecordedPacket> frames;
    for (std::uint64_t index = 0U; index < count; ++index) {
        RecordedPacket frame;
        frame.time_nanoseconds = static_cast<std::int64_t>(input.integer(8U));
        frame.framebuffer_width = static_cast<std::uint32_t>(input.integer(4U));
        frame.framebuffer_height = static_cast<std::uint32_t>(input.integer(4U));
        frame.logical_width = std::bit_cast<double>(input.integer(8U));
        frame.logical_height = std::bit_cast<double>(input.integer(8U));
        frame.bytes = input.raw(static_cast<std::size_t>(input.integer(4U)));
        frames.push_back(std::move(frame));
    }
    if (!input.done())
        throw std::invalid_argument("packet stream has trailing bytes");
    return frames;
}

} // namespace strata::headless
