#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <strata/svg.hpp>

namespace {

[[nodiscard]] std::uint32_t dimension(const std::string_view value) {
    std::uint32_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0U) {
        throw std::invalid_argument("raster dimensions must be positive integers");
    }
    return parsed;
}

[[nodiscard]] std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open SVG input: " + path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("could not open PAM output: " + path);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output)
        throw std::runtime_error("could not write PAM output: " + path);
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count != 3 && argument_count != 5) {
            std::cerr << "usage: strata_svg_render INPUT.svg OUTPUT.pam [WIDTH HEIGHT]\n";
            return 2;
        }
        strata::svg::RasterOptions raster_options;
        if (argument_count == 5) {
            raster_options.width = dimension(arguments[3]);
            raster_options.height = dimension(arguments[4]);
        }
        const strata::svg::Document document = strata::svg::parse(read_file(arguments[1]));
        const strata::svg::Image image = strata::svg::rasterize(document, raster_options);
        const std::vector<std::uint8_t> encoded = strata::svg::encode_pam(image);
        write_file(arguments[2], encoded);
        std::cout << "rendered " << document.commands.size() << " draw commands to " << image.width
                  << 'x' << image.height << " PAM\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_svg_render: " << exception.what() << '\n';
        return 1;
    }
}
