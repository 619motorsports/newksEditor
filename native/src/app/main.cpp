#include "apex/app/authoring_service.hpp"
#include "apex/app/presentation_recreation.hpp"
#include "apex/app/workspace_selection.hpp"
#include "apex/app/workspace_viewport.hpp"
#include "apex/assets/asset_source.hpp"
#include "apex/core/parse_error.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/domain/analog_instruments.hpp"
#include "apex/domain/animation_preview.hpp"
#include "apex/formats/acd.hpp"
#include "apex/formats/dds.hpp"
#include "apex/formats/ini.hpp"
#include "apex/formats/kn5.hpp"
#include "apex/formats/ksanim.hpp"
#include "apex/formats/vao.hpp"
#include "apex/platform/window.hpp"
#include "apex/render/device.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

void usage(std::ostream& output) {
    output << "Usage:\n"
           << "  apex-native --backend vulkan|d3d12 [--validation]\n"
           << "  apex-native --window vulkan|d3d12 [--frames <count>] [--validation]\n"
           << "  apex-native --window vulkan|d3d12 [--model <file>] [--workspace-root <dir> --manifest <file> --kind track|carLods]\n"
              "                       [--analog-instruments <file> [--rpm <value>]]\n"
              "                       [--animation <file> [--animation-position <value>]]\n"
              "                       [--lod-index <index>]\n"
              "                       [--node-search <query>] [--selected-node <id> [--isolate-selected]]\n"
              "                       [--show-hidden] [--wireframe]\n"
              "                       [--shader-family <name> --shader-vertex <file> --shader-fragment <file>]\n"
           << "  apex-native --inspect-kn5 <file>\n"
           << "  apex-native --inspect-dds <file>\n"
           << "  apex-native --inspect-acd <asset-directory-name> <file>\n"
           << "  apex-native --inspect-ini <file>\n"
           << "  apex-native --inspect-vao <file>\n"
           << "  apex-native --inspect-ksanim <file>\n"
           << "  apex-native --export-project kn5|csp <source.kn5> <project.apex.json> <output>\n"
           << "  apex-native --export-project collider|damage|bottom-colliders|surfaces|models|lods <source.kn5> "
              "<project.apex.json> <secondary-input> <output>\n";
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

std::string bytes_as_text(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::filesystem::path write_temporary_exclusive(
    const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    constexpr std::size_t maxAttempts = 1'024U;
    for (std::size_t attempt = 0; attempt < maxAttempts; ++attempt) {
        auto temporary = path;
        temporary += ".apex-tmp-" + std::to_string(attempt);

#if defined(_WIN32)
        const HANDLE handle = CreateFileW(
            temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "cannot create " + temporary.string());
        }
        std::size_t offset = 0;
        bool succeeded = true;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
                remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
            DWORD written = 0;
            if (WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) == FALSE ||
                written == 0U) {
                succeeded = false;
                break;
            }
            offset += written;
        }
        if (succeeded) succeeded = FlushFileBuffers(handle) != FALSE;
        const auto writeError = succeeded ? ERROR_SUCCESS : GetLastError();
        const bool closed = CloseHandle(handle) != FALSE;
        if (!succeeded || !closed) {
            std::error_code ignored;
            (void)std::filesystem::remove(temporary, ignored);
            const auto error = writeError != ERROR_SUCCESS ? writeError : GetLastError();
            throw std::system_error(static_cast<int>(error), std::system_category(),
                                    "cannot write " + temporary.string());
        }
#else
        const int descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                      0666);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            throw std::system_error(errno, std::generic_category(),
                                    "cannot create " + temporary.string());
        }
        std::size_t offset = 0;
        int writeError = 0;
        while (offset < bytes.size()) {
            const auto remaining = bytes.size() - offset;
            const auto chunk = std::min<std::size_t>(
                remaining, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const auto written = ::write(descriptor, bytes.data() + offset, chunk);
            if (written < 0) {
                if (errno == EINTR) continue;
                writeError = errno;
                break;
            }
            if (written == 0) {
                writeError = EIO;
                break;
            }
            offset += static_cast<std::size_t>(written);
        }
        if (writeError == 0 && ::fsync(descriptor) != 0) writeError = errno;
        if (::close(descriptor) != 0 && writeError == 0) writeError = errno;
        if (writeError != 0) {
            std::error_code ignored;
            (void)std::filesystem::remove(temporary, ignored);
            throw std::system_error(writeError, std::generic_category(),
                                    "cannot write " + temporary.string());
        }
#endif
        return temporary;
    }
    throw std::runtime_error("cannot allocate a temporary output beside " + path.string());
}

void promote_temporary_no_replace(const std::filesystem::path& temporary,
                                  const std::filesystem::path& path) {
#if defined(_WIN32)
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            throw std::runtime_error("output already exists: " + path.string());
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "cannot finalize " + path.string());
    }
#else
    if (::link(temporary.c_str(), path.c_str()) != 0) {
        if (errno == EEXIST)
            throw std::runtime_error("output already exists: " + path.string());
        throw std::system_error(errno, std::generic_category(),
                                "cannot finalize " + path.string());
    }
    if (::unlink(temporary.c_str()) != 0) {
        const auto cleanupError = errno;
        (void)::unlink(path.c_str());
        throw std::system_error(cleanupError, std::generic_category(),
                                "cannot remove temporary output " + temporary.string());
    }
#endif
}

void write_file_exclusive(const std::filesystem::path& path,
                          std::span<const std::uint8_t> bytes) {
    if (path.empty()) throw std::runtime_error("output path is empty");
    const auto temporary = write_temporary_exclusive(path, bytes);
    try {
        promote_temporary_no_replace(temporary, path);
    } catch (...) {
        std::error_code ignored;
        (void)std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void write_file_exclusive(const std::filesystem::path& path,
                          std::string_view text) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
    write_file_exclusive(path, std::span<const std::uint8_t>(data, text.size()));
}

template <typename Result>
void report_authoring_diagnostics(const Result& result) {
    for (const auto& item : result.diagnostics) {
        std::cerr << item.code;
        if (!item.path.empty()) std::cerr << " [" << item.path << ']';
        if (item.line != 0U) std::cerr << ":" << item.line;
        std::cerr << ": " << item.message << '\n';
    }
}

template <typename Result>
int report_authoring_failure(std::string_view operation, const Result& result) {
    std::cerr << operation << ": "
              << apex::app::authoring_service_status_name(result.status) << '\n';
    report_authoring_diagnostics(result);
    return 1;
}

std::string secondary_logical_name(const apex::app::AuthoringService& service,
                                   std::string_view kind,
                                   const std::filesystem::path& path) {
    const auto* state = service.state();
    if (state != nullptr) {
        if (kind == "collider" && state->colliderAsset)
            return state->colliderAsset->name;
        if (kind == "damage" && state->damageAsset)
            return state->damageAsset->name;
        if (kind == "bottom-colliders" && state->bottomColliderAsset)
            return state->bottomColliderAsset->name;
    }
    return path.filename().generic_string();
}

int export_project(int argc, char** argv) {
    if (argc < 3) throw std::runtime_error("invalid --export-project arguments");
    const std::string_view kind = argv[2];
    const bool primaryOutput = kind == "kn5" || kind == "csp";
    const bool secondaryOutput = kind == "collider" || kind == "damage" ||
                                 kind == "bottom-colliders" || kind == "surfaces" ||
                                 kind == "models" || kind == "lods";
    if ((!primaryOutput && !secondaryOutput) ||
        (primaryOutput && argc != 6) || (secondaryOutput && argc != 7)) {
        throw std::runtime_error("invalid --export-project arguments");
    }

    const std::filesystem::path sourcePath = argv[3];
    const std::filesystem::path projectPath = argv[4];
    const auto sourceBytes = read_file(sourcePath);
    const auto projectBytes = read_file(projectPath);

    apex::app::AuthoringService service;
    const auto opened = service.openPrimary(
        sourcePath.filename().generic_string(), sourceBytes);
    if (!opened.ok()) return report_authoring_failure("open primary", opened);
    const auto loaded = service.loadProject(bytes_as_text(projectBytes));
    if (!loaded.ok()) return report_authoring_failure("load project", loaded);

    if (primaryOutput) {
        const std::filesystem::path outputPath = argv[5];
        if (kind == "kn5") {
            const auto exported = service.exportPrimaryKn5();
            if (!exported.ok()) return report_authoring_failure("export KN5", exported);
            report_authoring_diagnostics(exported);
            write_file_exclusive(outputPath, exported.bytes);
            std::cout << outputPath.string() << ": " << exported.bytes.size()
                      << " bytes, revision " << exported.revision << '\n';
        } else {
            const auto exported = service.exportCsp();
            if (!exported.ok()) return report_authoring_failure("export CSP", exported);
            report_authoring_diagnostics(exported);
            write_file_exclusive(outputPath, std::string_view(exported.text));
            std::cout << outputPath.string() << ": " << exported.text.size()
                      << " bytes, revision " << exported.revision << '\n';
        }
        return 0;
    }

    const std::filesystem::path secondaryPath = argv[5];
    const std::filesystem::path outputPath = argv[6];
    const auto secondaryBytes = read_file(secondaryPath);
    const auto logicalName = secondary_logical_name(service, kind, secondaryPath);
    if (kind == "collider") {
        const auto bound = service.openCollider(logicalName, secondaryBytes);
        if (!bound.ok()) return report_authoring_failure("open collider", bound);
        report_authoring_diagnostics(bound);
        const auto exported = service.exportColliderKn5();
        if (!exported.ok()) return report_authoring_failure("export collider", exported);
        report_authoring_diagnostics(exported);
        write_file_exclusive(outputPath, exported.bytes);
        std::cout << outputPath.string() << ": " << exported.bytes.size()
                  << " bytes, revision " << exported.revision << '\n';
    } else if (kind == "damage") {
        const auto bound = service.openDamage(logicalName, secondaryBytes);
        if (!bound.ok()) return report_authoring_failure("open damage.ini", bound);
        report_authoring_diagnostics(bound);
        const auto exported = service.exportDamageIni();
        if (!exported.ok()) return report_authoring_failure("export damage.ini", exported);
        report_authoring_diagnostics(exported);
        write_file_exclusive(outputPath, std::string_view(exported.text));
        std::cout << outputPath.string() << ": " << exported.text.size()
                  << " bytes, revision " << exported.revision << '\n';
    } else if (kind == "bottom-colliders") {
        const auto bound = service.openBottomColliders(logicalName, secondaryBytes);
        if (!bound.ok()) return report_authoring_failure("open colliders.ini", bound);
        report_authoring_diagnostics(bound);
        const auto exported = service.exportBottomCollidersIni();
        if (!exported.ok())
            return report_authoring_failure("export colliders.ini", exported);
        report_authoring_diagnostics(exported);
        write_file_exclusive(outputPath, std::string_view(exported.text));
        std::cout << outputPath.string() << ": " << exported.text.size()
                  << " bytes, revision " << exported.revision << '\n';
    } else if (kind == "surfaces") {
        const auto bound = service.openSurfaces(logicalName, secondaryBytes);
        if (!bound.ok()) return report_authoring_failure("open surfaces.ini", bound);
        report_authoring_diagnostics(bound);
        const auto exported = service.exportSurfacesIni();
        if (!exported.ok())
            return report_authoring_failure("export surfaces.ini", exported);
        report_authoring_diagnostics(exported);
        write_file_exclusive(outputPath, std::string_view(exported.text));
        std::cout << outputPath.string() << ": " << exported.text.size()
                  << " bytes, revision " << exported.revision << '\n';
    } else {
        const auto workspaceKind = kind == "models"
                                       ? apex::authoring::ProjectWorkspaceKind::trackModels
                                       : apex::authoring::ProjectWorkspaceKind::carLods;
        const auto bound = service.openWorkspace(
            workspaceKind, logicalName, secondaryBytes);
        if (!bound.ok())
            return report_authoring_failure("open workspace manifest", bound);
        report_authoring_diagnostics(bound);
        const auto exported = service.exportWorkspaceIni();
        if (!exported.ok())
            return report_authoring_failure("export workspace manifest", exported);
        report_authoring_diagnostics(exported);
        write_file_exclusive(outputPath, std::string_view(exported.text));
        std::cout << outputPath.string() << ": " << exported.text.size()
                  << " bytes, revision " << exported.revision << '\n';
    }
    return 0;
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
    const apex::render::PresentationCapabilities presentation =
        result.device->presentation_capabilities();
    std::cout << apex::render::backend_name(backend) << ": "
              << result.device->info().name
              << ", offscreen=" << (presentation.offscreen ? "yes" : "no")
              << ", swapchain-api="
              << (presentation.swapchain_api_available ? "yes" : "no")
              << ", native-surface-api="
              << (presentation.native_surface_api_available ? "yes" : "no")
              << ", headless-surface-api="
              << (presentation.headless_surface_api_available ? "yes" : "no")
              << '\n';
    result.device->wait_idle();
    return 0;
}

std::uint64_t parse_frame_count(std::string_view value) {
    constexpr std::uint64_t maximum = 1'000'000U;
    std::uint64_t result = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result > maximum) {
        throw std::runtime_error("frame count must be between 0 and 1000000");
    }
    return result;
}

apex::scene::NodeId parse_scene_node_id(std::string_view value) {
    std::uint64_t result = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() ||
        result >= static_cast<std::uint64_t>(apex::scene::invalid_node_id)) {
        throw std::runtime_error("selected node ID must be a valid unsigned integer");
    }
    return static_cast<apex::scene::NodeId>(result);
}

std::uint32_t parse_lod_index(std::string_view value) {
    std::uint64_t result = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() ||
        result > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("LOD index must be a valid unsigned 32-bit integer");
    }
    return static_cast<std::uint32_t>(result);
}

struct WindowShaderSpec {
    std::string family;
    std::filesystem::path vertex;
    std::filesystem::path fragment;
};

struct WindowWorkspaceOptions {
    std::optional<std::filesystem::path> model;
    std::optional<std::filesystem::path> workspaceRoot;
    std::optional<std::filesystem::path> manifest;
    apex::app::WorkspaceSessionKind kind = apex::app::WorkspaceSessionKind::generic;
    bool kindSpecified = false;
    std::optional<std::filesystem::path> analogInstruments;
    double rpm = 1'000.0;
    bool rpmSpecified = false;
    std::optional<std::filesystem::path> animation;
    double animationPosition = 0.0;
    bool animationPositionSpecified = false;
    std::optional<std::uint32_t> lodIndex;
    std::optional<std::string> nodeSearch;
    std::optional<apex::scene::NodeId> selectedNode;
    bool isolateSelected = false;
    bool showHidden = false;
    bool wireframe = false;
    std::vector<WindowShaderSpec> shaders;
};

struct LoadedWindowWorkspace {
    std::optional<apex::app::WorkspaceSessionDocument> document;
    struct ShaderSet {
        std::vector<apex::render::PipelineShaderModule> modules;
        apex::render::StockMaterialShaderModules descriptor;
    };
    std::vector<ShaderSet> shaderSets;
    std::vector<apex::render::StockMaterialShaderModules> descriptors;
    apex::app::WorkspaceSelectionState selection;
};

apex::app::WorkspaceSessionKind parse_workspace_kind(std::string_view value) {
    if (value == "track") return apex::app::WorkspaceSessionKind::track;
    if (value == "carLods" || value == "car-lods")
        return apex::app::WorkspaceSessionKind::carLods;
    throw std::runtime_error("workspace kind must be track or carLods");
}

double parse_finite_number(std::string_view value, std::string_view label) {
    double result = 0.0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() || !std::isfinite(result)) {
        throw std::runtime_error(std::string(label) + " must be a finite number");
    }
    return result;
}

void write_cli_text(std::ostream& output, std::string_view value) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    output << '"';
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        if (character == static_cast<unsigned char>('\\')) {
            output << "\\\\";
        } else if (character == static_cast<unsigned char>('"')) {
            output << "\\\"";
        } else if (character >= 0x20U && character <= 0x7eU) {
            output << static_cast<char>(character);
        } else {
            output << "\\x" << hex[character >> 4U] << hex[character & 0x0fU];
        }
    }
    output << '"';
}

std::string workspace_kind_name(apex::app::WorkspaceSessionKind kind) {
    switch (kind) {
    case apex::app::WorkspaceSessionKind::generic: return "generic";
    case apex::app::WorkspaceSessionKind::track: return "track";
    case apex::app::WorkspaceSessionKind::carLods: return "carLods";
    }
    return "generic";
}

std::optional<apex::app::WorkspaceViewportCameraMove>
workspace_camera_move_for_key(std::uint32_t key) noexcept {
    switch (key) {
    case static_cast<std::uint32_t>('w'):
    case static_cast<std::uint32_t>('W'):
        return apex::app::WorkspaceViewportCameraMove::forward;
    case static_cast<std::uint32_t>('s'):
    case static_cast<std::uint32_t>('S'):
        return apex::app::WorkspaceViewportCameraMove::backward;
    case static_cast<std::uint32_t>('a'):
    case static_cast<std::uint32_t>('A'):
        return apex::app::WorkspaceViewportCameraMove::left;
    case static_cast<std::uint32_t>('d'):
    case static_cast<std::uint32_t>('D'):
        return apex::app::WorkspaceViewportCameraMove::right;
    case static_cast<std::uint32_t>('q'):
    case static_cast<std::uint32_t>('Q'):
        return apex::app::WorkspaceViewportCameraMove::down;
    case static_cast<std::uint32_t>('e'):
    case static_cast<std::uint32_t>('E'):
        return apex::app::WorkspaceViewportCameraMove::up;
    default:
        return std::nullopt;
    }
}

WindowWorkspaceOptions parse_window_workspace_options(int argc, char** argv,
                                                       int first_option,
                                                       bool& validation,
                                                       std::uint64_t& frame_limit) {
    WindowWorkspaceOptions result;
    for (int index = first_option; index < argc; ++index) {
        const std::string_view option = argv[index];
        auto require_value = [&](std::string_view name) -> std::string_view {
            if (index + 1 >= argc) throw std::runtime_error(std::string(name) + " requires a value");
            return argv[++index];
        };
        if (option == "--validation") {
            if (validation) throw std::runtime_error("duplicate --validation option");
            validation = true;
        } else if (option == "--frames") {
            frame_limit = parse_frame_count(require_value("--frames"));
        } else if (option == "--model") {
            if (result.model.has_value()) throw std::runtime_error("duplicate --model option");
            result.model = std::filesystem::path(require_value("--model"));
        } else if (option == "--workspace-root") {
            if (result.workspaceRoot.has_value())
                throw std::runtime_error("duplicate --workspace-root option");
            result.workspaceRoot = std::filesystem::path(require_value("--workspace-root"));
        } else if (option == "--manifest") {
            if (result.manifest.has_value()) throw std::runtime_error("duplicate --manifest option");
            result.manifest = std::filesystem::path(require_value("--manifest"));
        } else if (option == "--kind") {
            if (result.kindSpecified) throw std::runtime_error("duplicate --kind option");
            result.kind = parse_workspace_kind(require_value("--kind"));
            result.kindSpecified = true;
        } else if (option == "--analog-instruments") {
            if (result.analogInstruments.has_value())
                throw std::runtime_error("duplicate --analog-instruments option");
            result.analogInstruments =
                std::filesystem::path(require_value("--analog-instruments"));
        } else if (option == "--rpm") {
            if (result.rpmSpecified) throw std::runtime_error("duplicate --rpm option");
            result.rpm = parse_finite_number(require_value("--rpm"), "RPM value");
            result.rpmSpecified = true;
        } else if (option == "--animation") {
            if (result.animation.has_value())
                throw std::runtime_error("duplicate --animation option");
            result.animation = std::filesystem::path(require_value("--animation"));
        } else if (option == "--animation-position") {
            if (result.animationPositionSpecified)
                throw std::runtime_error("duplicate --animation-position option");
            result.animationPosition = parse_finite_number(
                require_value("--animation-position"), "animation position");
            result.animationPositionSpecified = true;
        } else if (option == "--lod-index") {
            if (result.lodIndex.has_value())
                throw std::runtime_error("duplicate --lod-index option");
            result.lodIndex = parse_lod_index(require_value("--lod-index"));
        } else if (option == "--node-search") {
            if (result.nodeSearch.has_value())
                throw std::runtime_error("duplicate --node-search option");
            result.nodeSearch = std::string(require_value("--node-search"));
        } else if (option == "--selected-node") {
            if (result.selectedNode.has_value())
                throw std::runtime_error("duplicate --selected-node option");
            result.selectedNode = parse_scene_node_id(
                require_value("--selected-node"));
        } else if (option == "--isolate-selected") {
            if (result.isolateSelected)
                throw std::runtime_error("duplicate --isolate-selected option");
            result.isolateSelected = true;
        } else if (option == "--show-hidden") {
            if (result.showHidden)
                throw std::runtime_error("duplicate --show-hidden option");
            result.showHidden = true;
        } else if (option == "--wireframe") {
            if (result.wireframe)
                throw std::runtime_error("duplicate --wireframe option");
            result.wireframe = true;
        } else if (option == "--shader-family") {
            const auto family = std::string(require_value("--shader-family"));
            if (family.empty()) throw std::runtime_error("shader family cannot be empty");
            result.shaders.push_back({std::move(family), {}, {}});
        } else if (option == "--shader-vertex") {
            if (result.shaders.empty() || !result.shaders.back().vertex.empty())
                throw std::runtime_error("--shader-vertex must follow one shader family once");
            result.shaders.back().vertex = require_value("--shader-vertex");
        } else if (option == "--shader-fragment") {
            if (result.shaders.empty() || !result.shaders.back().fragment.empty())
                throw std::runtime_error("--shader-fragment must follow one shader family once");
            result.shaders.back().fragment = require_value("--shader-fragment");
        } else {
            throw std::runtime_error("unknown window option");
        }
    }
    if (result.model.has_value() && result.workspaceRoot.has_value())
        throw std::runtime_error("--model and --workspace-root are mutually exclusive");
    if (result.model.has_value() && result.kind != apex::app::WorkspaceSessionKind::generic)
        throw std::runtime_error("--model accepts only the generic workspace kind");
    if (result.manifest.has_value() != result.workspaceRoot.has_value())
        throw std::runtime_error("--workspace-root and --manifest must be supplied together");
    if (result.kindSpecified && !result.workspaceRoot.has_value())
        throw std::runtime_error("--kind requires --workspace-root");
    if (result.workspaceRoot.has_value() && result.kind == apex::app::WorkspaceSessionKind::generic)
        throw std::runtime_error("workspace roots require --kind track or carLods");
    if (!result.shaders.empty()) {
        std::set<std::string> families;
        for (const auto& shader : result.shaders) {
            if (shader.vertex.empty() || shader.fragment.empty())
                throw std::runtime_error("each shader family requires vertex and fragment modules");
            if (!families.insert(shader.family).second)
                throw std::runtime_error("duplicate shader family: " + shader.family);
        }
    }
    if ((!result.model.has_value() && !result.workspaceRoot.has_value()) &&
        (!result.shaders.empty()))
        throw std::runtime_error("shader modules require a workspace model");
    if (result.rpmSpecified && !result.analogInstruments.has_value())
        throw std::runtime_error("--rpm requires --analog-instruments");
    if (result.analogInstruments.has_value() &&
        !result.model.has_value() && !result.workspaceRoot.has_value())
        throw std::runtime_error("--analog-instruments requires a workspace model");
    if (result.animationPositionSpecified && !result.animation.has_value())
        throw std::runtime_error("--animation-position requires --animation");
    if (result.animation.has_value() &&
        !result.model.has_value() && !result.workspaceRoot.has_value())
        throw std::runtime_error("--animation requires a workspace model");
    if (result.lodIndex.has_value() &&
        (!result.workspaceRoot.has_value() ||
         result.kind != apex::app::WorkspaceSessionKind::carLods)) {
        throw std::runtime_error("--lod-index requires a carLods workspace");
    }
    if (result.isolateSelected && !result.selectedNode.has_value())
        throw std::runtime_error("--isolate-selected requires --selected-node");
    const bool selection_options = result.nodeSearch.has_value() ||
                                   result.selectedNode.has_value() ||
                                   result.showHidden || result.wireframe;
    if (selection_options && !result.model.has_value() &&
        !result.workspaceRoot.has_value())
        throw std::runtime_error("hierarchy options require a workspace model");
    return result;
}

void report_workspace_diagnostics(const apex::app::WorkspaceSessionResult& result) {
    for (const auto& item : result.diagnostics)
        std::cerr << item.code << " [" << item.path << "]: " << item.message << '\n';
}

void load_window_workspace(const WindowWorkspaceOptions& options,
                           apex::render::Backend backend,
                           LoadedWindowWorkspace& loaded) {
    if (!options.model.has_value() && !options.workspaceRoot.has_value()) return;

    apex::app::WorkspaceSessionResult opened;
    if (options.model.has_value()) {
        const auto bytes = read_file(*options.model);
        apex::app::WorkspaceSessionFile file;
        file.name = options.model->filename().generic_string();
        file.bytes = bytes;
        apex::app::WorkspaceSessionOpenRequest request;
        request.kind = apex::app::WorkspaceSessionKind::generic;
        request.name = file.name;
        request.modelFiles = std::span<const apex::app::WorkspaceSessionFile>(&file, 1U);
        opened = apex::app::WorkspaceSession{}.open(request);
    } else {
        apex::assets::AssetSource source;
        source.addDirectory(*options.workspaceRoot);
        const auto manifestName = options.manifest->generic_string();
        opened = apex::app::WorkspaceSession{}.openAssetSource(
            options.kind, options.workspaceRoot->filename().generic_string(),
            manifestName, source);
    }
    if (!opened.ok()) {
        report_workspace_diagnostics(opened);
        throw std::runtime_error("workspace open failed");
    }
    loaded.document = std::move(opened.document);
    if (options.lodIndex.has_value()) {
        const auto& files = loaded.document->assembly.workspace.files;
        const bool present = std::any_of(
            files.begin(), files.end(), [&](const auto& file) {
                return file.lod.has_value() &&
                       file.lod->index == *options.lodIndex;
            });
        if (!present)
            throw std::runtime_error("selected workspace LOD index is not present");
    }
    bool model_changed = false;
    if (options.analogInstruments.has_value()) {
        const auto bytes = read_file(*options.analogInstruments);
        const auto text = std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const auto config = apex::domain::parse_analog_instruments(
            text, options.analogInstruments->generic_string());
        for (const auto& item : config.diagnostics) {
            std::cerr << item.code << " [" << item.source << ':' << item.line
                      << "]: " << item.message << '\n';
        }
        if (!config.rpm.has_value())
            throw std::runtime_error("analog instrument configuration has no valid RPM indicator");
        if (!config.rpm->preview_supported)
            throw std::runtime_error("analog RPM LUT preview is unsupported");
        const auto applied = apex::domain::apply_analog_rpm(
            loaded.document->assembly.model, &*config.rpm, options.rpm);
        if (applied.applied_nodes == 0U) {
            throw std::runtime_error(
                std::string("analog RPM node binding is ") +
                apex::domain::analog_rpm_binding_status_name(applied.status));
        }
        model_changed = true;
        std::cout << "analog RPM: node=" << applied.object_name
                  << ", matches=" << applied.matches
                  << ", rpm=" << applied.rpm << '\n';
    }
    if (options.animation.has_value()) {
        const auto bytes = read_file(*options.animation);
        const auto animation = apex::formats::parseKsAnimation(
            bytes, options.animation->generic_string());
        const auto clamped_position = static_cast<float>(
            std::clamp(options.animationPosition, 0.0, 1.0));
        const auto applied = apex::domain::apply_animation_preview(
            loaded.document->assembly.model, animation, clamped_position);
        for (const auto& item : applied.diagnostics) {
            std::cerr << item.code << " [" << item.source
                      << "]: " << item.message << '\n';
        }
        model_changed = applied.matched_nodes > 0U || model_changed;
        std::cout << "animation: tracks=" << applied.tracks
                  << ", animated=" << applied.animated_tracks
                  << ", matched-tracks=" << applied.matched_tracks
                  << ", matched-nodes=" << applied.matched_nodes
                  << ", position=" << applied.position << '\n';
    }
    if (model_changed) {
        loaded.document->scene = apex::scene::convertKn5Scene(
            loaded.document->assembly.model);
        loaded.document->sceneBinding = apex::workspace::bindWorkspaceScene(
            loaded.document->scene.snapshot,
            loaded.document->assembly.workspace);
    }
    const bool selection_options = options.nodeSearch.has_value() ||
                                   options.selectedNode.has_value() ||
                                   options.showHidden || options.wireframe;
    if (selection_options) {
        apex::app::WorkspaceSelectionRequest request;
        request.query = options.nodeSearch.has_value()
                            ? std::string_view(*options.nodeSearch)
                            : std::string_view{};
        request.collect_matches = options.nodeSearch.has_value();
        request.selected_node = options.selectedNode.value_or(
            apex::scene::invalid_node_id);
        request.isolate_selected = options.isolateSelected;
        request.show_hidden = options.showHidden;
        request.wireframe = options.wireframe;
        const auto selection = apex::app::resolve_workspace_selection(
            loaded.document->scene.snapshot, request);
        if (!selection.ok()) {
            std::cerr << selection.diagnostic.code;
            if (selection.diagnostic.node != apex::scene::invalid_node_id)
                std::cerr << " [node " << selection.diagnostic.node << ']';
            std::cerr << ": " << selection.diagnostic.message << '\n';
            throw std::runtime_error("workspace hierarchy selection failed");
        }
        loaded.selection = selection.state;
        if (options.nodeSearch.has_value()) {
            std::cout << "hierarchy: matches=" << selection.matches.size()
                      << '\n';
            for (const auto& match : selection.matches) {
                std::cout << "node: id=" << match.node
                          << ", depth=" << match.depth
                          << ", kind="
                          << apex::app::workspace_selection_node_kind_name(
                                 match.kind)
                          << ", name=";
                write_cli_text(std::cout, match.name);
                std::cout << '\n';
            }
        }
    }
    if (options.shaders.empty())
        throw std::runtime_error("workspace rendering requires caller-supplied shader modules");

    const auto shader_format = backend == apex::render::Backend::Vulkan
                                   ? apex::render::PipelineShaderFormat::spirv
                                   : apex::render::PipelineShaderFormat::dxil;
    constexpr std::uint64_t max_shader_bytes =
        apex::render::StaticSceneResourceLimits{}.max_total_shader_bytes;
    std::uint64_t shader_bytes = 0U;
    loaded.shaderSets.reserve(options.shaders.size());
    for (const auto& shader : options.shaders) {
        LoadedWindowWorkspace::ShaderSet set;
        set.modules.push_back({apex::render::PipelineShaderStage::vertex, shader_format,
                               read_file(shader.vertex)});
        set.modules.push_back({apex::render::PipelineShaderStage::fragment, shader_format,
                               read_file(shader.fragment)});
        for (const auto& module : set.modules) {
            if (module.bytes.size() > apex::render::max_shader_module_bytes)
                throw std::runtime_error("a caller-supplied shader module exceeds the native module budget");
            if (module.bytes.size() > max_shader_bytes ||
                shader_bytes > max_shader_bytes - module.bytes.size())
                throw std::runtime_error("caller-supplied shader modules exceed the native shader budget");
            shader_bytes += module.bytes.size();
        }
        loaded.shaderSets.push_back(std::move(set));
    }
    loaded.descriptors.reserve(loaded.shaderSets.size());
    for (std::size_t index = 0; index < loaded.shaderSets.size(); ++index) {
        loaded.shaderSets[index].descriptor = {
            apex::render::StockMaterialShaderKeyKind::shader_family,
            options.shaders[index].family, loaded.shaderSets[index].modules};
        loaded.descriptors.push_back(loaded.shaderSets[index].descriptor);
    }
}

int run_window(int argc, char** argv) {
    if (argc < 3) throw std::runtime_error("invalid --window arguments");
    const auto backend = parse_backend(argv[2]);
    bool validation = false;
    std::uint64_t frame_limit = 0U;
    const auto workspace_options = parse_window_workspace_options(
        argc, argv, 3, validation, frame_limit);
    LoadedWindowWorkspace loaded_workspace;
    load_window_workspace(workspace_options, backend, loaded_workspace);

    apex::platform::WindowDescription window_description;
    window_description.title = "Apex Editor native shell";
    window_description.vulkan = backend == apex::render::Backend::Vulkan;
    auto window_result = apex::platform::Window::create(window_description);
    if (!window_result.ok()) {
        std::cerr << "window: " << window_result.diagnostic.code << ": "
                  << window_result.diagnostic.message << '\n';
        return window_result.status == apex::platform::WindowStatus::unavailable ? 77 : 1;
    }

    apex::render::DeviceOptions options;
    options.headless = false;
    options.enable_validation = validation;
    options.native_surface = window_result.window->native_surface_source();
    auto device_result = apex::render::create_device(backend, options);
    if (!device_result.ok()) {
        std::cerr << apex::render::backend_name(backend) << ": "
                  << device_result.diagnostic.code << ": "
                  << device_result.diagnostic.message << '\n';
        return device_result.status == apex::render::DeviceStatus::unavailable ? 77 : 1;
    }

    auto create_target = [&]() {
        apex::render::PresentationTargetDescription description;
        description.width = window_result.window->pixel_width();
        description.height = window_result.window->pixel_height();
        return device_result.device->create_presentation_target(description);
    };
    auto target_result = create_target();
    if (!target_result.ok()) {
        std::cerr << "presentation: " << target_result.diagnostic.code << ": "
                  << target_result.diagnostic.message << '\n';
        return target_result.status == apex::render::PresentationTargetStatus::unsupported ? 77 : 1;
    }

    const auto clip_space = backend == apex::render::Backend::Vulkan
                                ? apex::render::CameraClipSpace::vulkan
                                : apex::render::CameraClipSpace::d3d12;
    apex::app::WorkspaceViewportCameraController camera_controller;
    if (loaded_workspace.document.has_value() &&
        loaded_workspace.document->scene.preview_bounds.has_value()) {
        const auto& bounds = *loaded_workspace.document->scene.preview_bounds;
        const double distance = std::max(
            static_cast<double>(bounds.radius) * 2.35, 0.2);
        if (!std::isfinite(distance) || distance > 1.0e7) {
            throw std::runtime_error(
                "workspace preview bounds exceed the native camera range");
        }
        camera_controller.target = bounds.center;
        camera_controller.distance = static_cast<float>(distance);
    }

    auto current_camera = [&]() {
        const auto description = target_result.target->info().description;
        return camera_controller.frame(
            static_cast<float>(description.width) /
                static_cast<float>(description.height),
            clip_space);
    };
    auto resolve_current_lods = [&](const apex::render::CameraFrame& camera)
        -> std::optional<apex::workspace::WorkspaceLodResolution> {
        if (!loaded_workspace.document.has_value() ||
            loaded_workspace.document->assembly.workspace.kind != "carLods") {
            return std::nullopt;
        }
        if (!loaded_workspace.document->scene.preview_bounds.has_value()) {
            throw std::runtime_error(
                "carLods workspace has no preview-visible geometry bounds");
        }
        apex::workspace::WorkspaceLodResolutionRequest request;
        request.workspace = &loaded_workspace.document->assembly.workspace;
        request.scene = &loaded_workspace.document->scene.snapshot;
        request.file_root_nodes =
            loaded_workspace.document->sceneBinding.file_root_nodes;
        request.bounds_center =
            loaded_workspace.document->scene.preview_bounds->center;
        request.camera_position = camera.position;
        request.selected_index = workspace_options.lodIndex;
        return apex::workspace::resolveWorkspaceLod(request);
    };

    std::unique_ptr<apex::app::WorkspaceViewport> viewport;
    std::vector<std::uint32_t> active_lod_indices;
    auto prepare_viewport = [&]() {
        if (!loaded_workspace.document.has_value()) {
            viewport.reset();
            active_lod_indices.clear();
            return true;
        }
        const auto camera = current_camera();
        if (!camera.ok()) {
            std::cerr << "workspace camera: " << camera.code << ": "
                      << camera.message << '\n';
            return false;
        }
        const auto lod = resolve_current_lods(*camera.frame);
        apex::app::WorkspaceViewportPrepareRequest request;
        request.presentation = target_result.target->info().description;
        request.shader_modules = loaded_workspace.descriptors;
        request.render.camera_position = camera.frame->position;
        request.render.isolated = loaded_workspace.selection.isolate_selected;
        request.render.isolated_node = loaded_workspace.selection.selected_node;
        request.render.show_hidden = loaded_workspace.selection.show_hidden;
        request.packets.selected_node = loaded_workspace.selection.selected_node;
        request.packets.wireframe = loaded_workspace.selection.wireframe;
        request.wireframe = loaded_workspace.selection.wireframe;
        if (lod.has_value()) {
            request.workspace.lod_bounds_center =
                loaded_workspace.document->scene.preview_bounds->center;
            request.workspace.lod_index = workspace_options.lodIndex;
        }
        auto prepared = apex::app::prepareWorkspaceViewport(
            *device_result.device, *loaded_workspace.document, request);
        if (!prepared.ok()) {
            std::cerr << "workspace render: " << prepared.diagnostic.code << ": "
                      << prepared.diagnostic.message << '\n';
            return false;
        }
        viewport = std::move(prepared.viewport);
        active_lod_indices = lod.has_value()
                                 ? std::move(lod->active_indices)
                                 : std::vector<std::uint32_t>{};
        return true;
    };
    if (!prepare_viewport()) return 1;

    std::array<apex::platform::WindowEvent, 64U> events{};
    std::uint64_t frames = 0U;
    apex::app::PresentationRecreationController recreation;
    while (!window_result.window->close_requested() &&
           (frame_limit == 0U || frames < frame_limit)) {
        bool resized = false;
        bool camera_changed = false;
        const auto event_count = window_result.window->poll_events(events);
        for (std::size_t index = 0U; index < event_count; ++index) {
            if (events[index].type == apex::platform::WindowEventType::pixel_size_changed)
                resized = true;
            switch (events[index].type) {
            case apex::platform::WindowEventType::key_down:
                if (const auto movement = workspace_camera_move_for_key(
                        events[index].key); movement.has_value()) {
                    camera_changed = camera_controller.move(*movement) ||
                                     camera_changed;
                }
                break;
            case apex::platform::WindowEventType::mouse_button_down:
                if (events[index].button == 1U) {
                    (void)camera_controller.apply({
                        apex::app::WorkspaceViewportCameraGesture::begin_orbit,
                        0.0F, 0.0F});
                } else if (events[index].button == 2U) {
                    (void)camera_controller.apply({
                        apex::app::WorkspaceViewportCameraGesture::begin_pan,
                        0.0F, 0.0F});
                }
                break;
            case apex::platform::WindowEventType::mouse_button_up:
                (void)camera_controller.apply({
                    apex::app::WorkspaceViewportCameraGesture::end_drag,
                    0.0F, 0.0F});
                break;
            case apex::platform::WindowEventType::mouse_motion:
                camera_changed = camera_controller.apply({
                    apex::app::WorkspaceViewportCameraGesture::drag,
                    events[index].x_relative, events[index].y_relative}) ||
                                 camera_changed;
                break;
            case apex::platform::WindowEventType::mouse_wheel:
                camera_changed = camera_controller.apply({
                    apex::app::WorkspaceViewportCameraGesture::wheel,
                    events[index].x_relative, events[index].y_relative}) ||
                                 camera_changed;
                break;
            default:
                break;
            }
        }
        if (window_result.window->close_requested()) break;
        const auto pixel_width = window_result.window->pixel_width();
        const auto pixel_height = window_result.window->pixel_height();
        if (apex::app::presentation_surface_is_zero_sized(pixel_width, pixel_height)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        if (apex::app::presentation_resize_requires_recreation(
                resized, pixel_width, pixel_height)) {
            target_result.target.reset();
            target_result = create_target();
            if (!target_result.ok()) {
                std::cerr << "presentation resize: " << target_result.diagnostic.code << ": "
                          << target_result.diagnostic.message << '\n';
                return 1;
            }
            if (!prepare_viewport()) return 1;
        }
        if (camera_changed && !workspace_options.lodIndex.has_value() &&
            loaded_workspace.document.has_value() &&
            loaded_workspace.document->assembly.workspace.kind == "carLods") {
            const auto camera = current_camera();
            if (!camera.ok()) {
                std::cerr << "workspace camera: " << camera.code << ": "
                          << camera.message << '\n';
                return 1;
            }
            const auto lod = resolve_current_lods(*camera.frame);
            if (lod.has_value() && lod->active_indices != active_lod_indices &&
                !prepare_viewport()) {
                return 1;
            }
        }
        apex::render::PresentationFrameResult blank_frame;
        apex::app::WorkspaceViewportFrameStatus viewport_status =
            apex::app::WorkspaceViewportFrameStatus::ready;
        apex::render::Diagnostic viewport_diagnostic;
        if (viewport != nullptr) {
            const auto camera = camera_controller.frame(
                static_cast<float>(pixel_width) / static_cast<float>(pixel_height),
                clip_space);
            if (!camera.ok()) {
                std::cerr << "workspace camera: " << camera.code << ": "
                          << camera.message << '\n';
                return 1;
            }
            apex::app::WorkspaceViewportFrameRequest frame_request;
            frame_request.camera = *camera.frame;
            frame_request.frame_constants = apex::render::KsPerPixelFrameConstants{};
            viewport_status = viewport->drawAndPresent(
                *device_result.device, *target_result.target, frame_request,
                viewport_diagnostic);
        } else {
            blank_frame = device_result.device->clear_and_present(
                *target_result.target, {0.035F, 0.055F, 0.085F, 1.0F});
        }
        const bool frame_ok = viewport != nullptr
                                  ? viewport_status == apex::app::WorkspaceViewportFrameStatus::ready
                                  : blank_frame.ok();
        const auto& frame_diagnostic = viewport != nullptr
                                           ? viewport_diagnostic
                                           : blank_frame.diagnostic;
        if (!frame_ok && recreation.begin_out_of_date_recreation(frame_diagnostic.code)) {
            target_result.target.reset();
            target_result = create_target();
            if (!target_result.ok()) {
                std::cerr << "presentation recreate: " << target_result.diagnostic.code << ": "
                          << target_result.diagnostic.message << '\n';
                return 1;
            }
            if (!prepare_viewport()) return 1;
            continue;
        }
        if (!frame_ok) {
            std::cerr << (viewport != nullptr ? "workspace frame: " : "presentation frame: ")
                      << frame_diagnostic.code << ": " << frame_diagnostic.message << '\n';
            return 1;
        }
        recreation.record_successful_frame();
        ++frames;
    }
    device_result.device->wait_idle();
    std::cout << apex::render::backend_name(backend) << ": "
              << device_result.device->info().name << ", window="
              << window_result.window->pixel_width() << 'x'
              << window_result.window->pixel_height() << ", frames=" << frames;
    if (loaded_workspace.document.has_value())
        std::cout << ", workspace=" << workspace_kind_name(
            workspace_options.kind);
    std::cout << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "--export-project") {
            if (argc < 3) {
                usage(std::cerr);
                return 2;
            }
            return export_project(argc, argv);
        }
        if (argc >= 3 && std::string_view(argv[1]) == "--window")
            return run_window(argc, argv);
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
    } catch (const apex::core::ParseError& error) {
        std::cerr << "apex-native: " << error.code() << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "apex-native: " << error.what() << '\n';
        return 1;
    }
}
