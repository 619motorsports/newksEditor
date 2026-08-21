#include "apex/core/parse_limits.hpp"
#include "apex/formats/ksanim.hpp"
#include "apex/render/device.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage(std::ostream& output) {
    output << "Usage:\n"
           << "  apex-native --backend vulkan|d3d12 [--validation]\n"
           << "  apex-native --inspect-ksanim <file>\n";
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error("cannot determine file size for " + path.string());
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > apex::core::ParseLimits{}.maxInputBytes)
        throw std::runtime_error("input exceeds the native parser size limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

apex::render::Backend parse_backend(std::string_view value) {
    if (value == "vulkan") return apex::render::Backend::Vulkan;
    if (value == "d3d12" || value == "directx") return apex::render::Backend::D3D12;
    throw std::runtime_error("backend must be vulkan or d3d12");
}

int inspect_ksanim(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    const auto animation = apex::formats::parseKsAnimation(bytes, path.string());
    std::cout << path.string() << ": KSANIM v" << animation.version << ", "
              << animation.tracks.size() << (animation.tracks.size() == 1 ? " track, " : " tracks, ")
              << animation.frameCount
              << " frames in the first track\n";
    for (const auto& warning : animation.warnings) std::cout << "warning: " << warning << '\n';
    return 0;
}

int probe_backend(apex::render::Backend backend, bool validation) {
    apex::render::DeviceOptions options;
    options.enable_validation = validation;
    auto result = apex::render::create_device(backend, options);
    if (!result.ok()) {
        std::cerr << apex::render::backend_name(backend) << ": " << result.diagnostic.code
                  << ": " << result.diagnostic.message << '\n';
        return result.status == apex::render::DeviceStatus::unavailable ? 77 : 1;
    }
    std::cout << apex::render::backend_name(backend) << ": " << result.device->info().name << '\n';
    result.device->wait_idle();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "--inspect-ksanim")
            return inspect_ksanim(argv[2]);
        if (argc >= 3 && argc <= 4 && std::string_view(argv[1]) == "--backend") {
            const bool validation = argc == 4 && std::string_view(argv[3]) == "--validation";
            if (argc == 4 && !validation) throw std::runtime_error("unknown backend option");
            return probe_backend(parse_backend(argv[2]), validation);
        }
        usage(std::cerr);
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "apex-native: " << error.what() << '\n';
        return 1;
    }
}
