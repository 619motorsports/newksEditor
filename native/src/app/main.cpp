#include "apex/core/parse_limits.hpp"
#include "apex/formats/acd.hpp"
#include "apex/formats/dds.hpp"
#include "apex/formats/ini.hpp"
#include "apex/formats/kn5.hpp"
#include "apex/formats/ksanim.hpp"
#include "apex/formats/vao.hpp"
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
           << "  apex-native --inspect-kn5 <file>\n"
           << "  apex-native --inspect-dds <file>\n"
           << "  apex-native --inspect-acd <asset-directory-name> <file>\n"
           << "  apex-native --inspect-ini <file>\n"
           << "  apex-native --inspect-vao <file>\n"
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

int inspect_kn5(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    apex::formats::Kn5ParseOptions options;
    options.metadataOnly = true;
    const auto model = apex::formats::parseKn5(bytes, path.string(), options);
    const auto nodes = apex::formats::walkKn5(model.root);
    std::cout << path.string() << ": KN5 v" << model.version << ", " << model.textures.size()
              << " textures, " << model.materials.size() << " materials, " << nodes.size() << " nodes";
    if (model.encryption.has_value())
        std::cout << ", " << model.encryption->format << (model.encryption->valid ? " valid" : " malformed");
    std::cout << '\n';
    return 0;
}

int inspect_dds(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    const auto descriptor = apex::formats::inspectDds(bytes, path.string());
    if (!descriptor.has_value()) throw std::runtime_error("unsupported or malformed DDS header");
    std::cout << path.string() << ": " << apex::formats::ddsFormatName(descriptor->format) << ", "
              << descriptor->width << 'x' << descriptor->height << ", " << descriptor->mipCount
              << (descriptor->mipCount == 1 ? " mip" : " mips")
              << (descriptor->gpuRequired ? ", GPU decode required" : ", CPU decode available") << '\n';
    return 0;
}

int inspect_acd(std::string_view asset_name, const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    const auto archive = apex::formats::parseAcd(bytes, asset_name, path.string());
    std::cout << path.string() << ": " << archive.entries.size() << " records, " << archive.byPath.size()
              << " safe unique paths, " << archive.warnings.size() << " warnings\n";
    return 0;
}

int inspect_ini(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    const auto document = apex::formats::parse_ini(bytes, path.string());
    std::size_t entries = 0;
    for (const auto& section : document.sections) entries += section.entries.size();
    std::cout << path.string() << ": " << document.sections.size() << " sections, " << entries
              << " entries, " << document.warnings.size() << " warnings\n";
    return 0;
}

int inspect_vao(const std::filesystem::path& path) {
    const auto bytes = read_file(path);
    const auto patch = apex::formats::parseVaoPatch(bytes, path.string());
    std::cout << path.string() << ": VAO v" << patch.version << ", "
              << patch.data.records.size() << " records, "
              << patch.archiveEntries.size() << " archive entries, "
              << patch.splitAo.warnings.size() << " split-AO warnings\n";
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
        if (argc == 3 && std::string_view(argv[1]) == "--inspect-kn5")
            return inspect_kn5(argv[2]);
        if (argc == 3 && std::string_view(argv[1]) == "--inspect-dds")
            return inspect_dds(argv[2]);
        if (argc == 4 && std::string_view(argv[1]) == "--inspect-acd")
            return inspect_acd(argv[2], argv[3]);
        if (argc == 3 && std::string_view(argv[1]) == "--inspect-ini")
            return inspect_ini(argv[2]);
        if (argc == 3 && std::string_view(argv[1]) == "--inspect-vao")
            return inspect_vao(argv[2]);
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
