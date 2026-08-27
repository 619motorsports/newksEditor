#include "apex/app/ai_spline_legacy_conversion.hpp"
#include "apex/app/authoring_service.hpp"
#include "apex/app/fbx_preview_document.hpp"
#include "apex/app/presentation_recreation.hpp"
#include "apex/app/workspace_ai_spline.hpp"
#include "apex/app/workspace_ai_spline_commands.hpp"
#include "apex/app/workspace_ai_spline_controller.hpp"
#include "apex/app/workspace_selection.hpp"
#include "apex/app/workspace_shadow_programs.hpp"
#include "apex/app/workspace_track_camera.hpp"
#include "apex/app/workspace_viewport.hpp"
#include "apex/assets/asset_source.hpp"
#include "apex/authoring/ai_spline.hpp"
#include "apex/authoring/ai_spline_session.hpp"
#include "apex/core/parse_error.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/domain/analog_instruments.hpp"
#include "apex/domain/animation_preview.hpp"
#include "apex/domain/track_data.hpp"
#include "apex/formats/acd.hpp"
#include "apex/formats/ai_spline.hpp"
#include "apex/formats/dds.hpp"
#include "apex/formats/ini.hpp"
#include "apex/formats/kn5.hpp"
#include "apex/formats/ksanim.hpp"
#include "apex/formats/vao.hpp"
#include "apex/platform/file_output.hpp"
#include "apex/platform/window.hpp"
#include "apex/render/device.hpp"
#include "apex/render/picking.hpp"
#include "apex/render/stock_ks_per_pixel.hpp"

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
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void write_cli_text(std::ostream& output, std::string_view value);

void usage(std::ostream& output) {
    output << "Usage:\n"
           << "  apex-native --backend vulkan|d3d12 [--validation]\n"
           << "  apex-native --window vulkan|d3d12 [--frames <count>] [--validation]\n"
           << "  apex-native --window vulkan|d3d12 [--model <file>] [--workspace-root <dir> --manifest <file> --kind track|carLods]\n"
              "                       [--fbx <file> [--fbx-assets <directory>]]\n"
              "                       [--analog-instruments <file> [--rpm <value>]]\n"
              "                       [--animation <file> [--animation-position <value>]]\n"
              "                       [--fbx-animation <index> [--animation-position <value>]]\n"
           "                       [--lod-index <index>]\n"
              "                       [--track-camera-set <file> --track-camera-index <index>]\n"
              "                       [--track-camera-position <value>] [--track-camera-play]\n"
              "                       [--track-camera-mode webgl|installed-editor]\n"
              "                       [--ai-spline <file> [--ai-spline-mode raw|interpolated] [--ai-spline-interval <in> <out>]]\n"
              "                       [--ai-spline-show-left] [--ai-spline-show-right] [--ai-spline-index <index> ...] [--ai-spline-show-camber]\n"
              "                       [--ai-spline-edit-point <index> <x> <y> <z> ...] [--ai-spline-unlock-edit]\n"
              "                       [--ai-spline-save-on-exit <file>]\n"
              "                       [--node-search <query>] [--selected-node <id> [--isolate-selected]]\n"
              "                       [--show-hidden] [--wireframe] [--grid] [--view-axis] [--skeleton]\n"
              "                       [--weather <stock-id>] [--sun-heading <degrees>] [--sun-height <degrees>]\n"
              "                       [--cloud-assets <directory>]\n"
              "                       [--hdr [--exposure <value>] [--fxaa]]\n"
              "                       [--builtin-vulkan-ks-per-pixel]\n"
              "                       [--d3d12-ks-per-pixel-package <file>]\n"
              "                       [--d3d12-ks-per-pixel-at-package <file>]\n"
              "                       [--shader-family <name> --shader-vertex <file> --shader-fragment <file>]\n"
              "                       [--authoring-overlay-vertex <file> --authoring-overlay-fragment <file>]\n"
              "                       [--selected-mesh-vertex <file> --selected-mesh-fragment <file>]\n"
           "                       [--directional-shadow-vertex <file>]\n"
              "                       [--directional-shadow-alpha-vertex <file> --directional-shadow-alpha-fragment <file>]\n"
              "                       [--directional-shadow-skinned-vertex <file>]\n"
           << "  apex-native --inspect-kn5 <file>\n"
           << "  apex-native --inspect-fbx <file> [--fbx-assets <directory>]\n"
           << "  apex-native --inspect-dds <file>\n"
           << "  apex-native --inspect-acd <asset-directory-name> <file>\n"
           << "  apex-native --inspect-ini <file>\n"
           << "  apex-native --inspect-vao <file>\n"
           << "  apex-native --inspect-ksanim <file>\n"
           << "  apex-native --edit-ai-spline <input.ai> <output.ai> --index <point-index>\n"
              "                       [--set-radius <value>] [--set-side0 <value>] [--set-side1 <value>]\n"
              "                       [--set-camber-degrees <value>] [--set-length <value>] [--set-grade <value>]\n"
              "                       [--add-radius <value>] [--add-side0 <value>] [--add-side1 <value>]\n"
              "                       [--add-camber-degrees <value>] [--add-length <value>] [--add-grade <value>]\n"
           << "  apex-native --invert-ai-spline <input.ai> <output.ai> --index <point-index> [--index <point-index> ...]\n"
           << "  apex-native --set-ai-spline-point <input.ai> <output.ai> --index <point-index> --position <x> <y> <z>\n"
           << "  apex-native --set-ai-spline-points <input.ai> <output.ai> --point <point-index> <x> <y> <z> [--point ...]\n"
           << "  apex-native --save-ai-spline <input.ai> <output.ai>\n"
           << "  apex-native --convert-ai-spline-v2 <input.ai> <output.ai>\n"
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
            apex::platform::writeFileExclusive(outputPath, exported.bytes);
            std::cout << outputPath.string() << ": " << exported.bytes.size()
                      << " bytes, revision " << exported.revision << '\n';
        } else {
            const auto exported = service.exportCsp();
            if (!exported.ok()) return report_authoring_failure("export CSP", exported);
            report_authoring_diagnostics(exported);
            apex::platform::writeFileExclusive(
                outputPath, std::string_view(exported.text));
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
        apex::platform::writeFileExclusive(outputPath, exported.bytes);
        std::cout << outputPath.string() << ": " << exported.bytes.size()
                  << " bytes, revision " << exported.revision << '\n';
    } else if (kind == "damage") {
        const auto bound = service.openDamage(logicalName, secondaryBytes);
        if (!bound.ok()) return report_authoring_failure("open damage.ini", bound);
        report_authoring_diagnostics(bound);
        const auto exported = service.exportDamageIni();
        if (!exported.ok()) return report_authoring_failure("export damage.ini", exported);
        report_authoring_diagnostics(exported);
        apex::platform::writeFileExclusive(
            outputPath, std::string_view(exported.text));
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
        apex::platform::writeFileExclusive(
            outputPath, std::string_view(exported.text));
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
        apex::platform::writeFileExclusive(
            outputPath, std::string_view(exported.text));
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
        apex::platform::writeFileExclusive(
            outputPath, std::string_view(exported.text));
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

apex::render::PipelineRenderTargetFormat pipeline_color_format(
    apex::render::TextureFormat format) {
    switch (format) {
    case apex::render::TextureFormat::rgba8_unorm:
        return apex::render::PipelineRenderTargetFormat::rgba8_unorm;
    case apex::render::TextureFormat::rgba8_srgb:
        return apex::render::PipelineRenderTargetFormat::rgba8_srgb;
    case apex::render::TextureFormat::bgra8_unorm:
        return apex::render::PipelineRenderTargetFormat::bgra8_unorm;
    case apex::render::TextureFormat::bgra8_srgb:
        return apex::render::PipelineRenderTargetFormat::bgra8_srgb;
    default:
        throw std::runtime_error(
            "presentation format has no selection-axis pipeline contract");
    }
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

void report_fbx_diagnostics(
    const apex::app::FbxPreviewDocumentResult& result) {
    for (const auto& diagnostic : result.diagnostics) {
        write_cli_text(std::cerr, diagnostic.code);
        if (!diagnostic.path.empty()) {
            std::cerr << " [";
            write_cli_text(std::cerr, diagnostic.path);
            std::cerr << ']';
        }
        if (diagnostic.offset != 0U)
            std::cerr << " at byte " << diagnostic.offset;
        std::cerr << ": ";
        write_cli_text(std::cerr, diagnostic.message);
        std::cerr << '\n';
    }
}

int inspect_fbx(const std::filesystem::path& path,
                const std::optional<std::filesystem::path>& asset_root) {
    const auto bytes = read_file(path);
    apex::assets::AssetSource assets;
    apex::app::FbxPreviewDocumentRequest request;
    request.source = path.filename().generic_string();
    request.bytes = bytes;
    if (asset_root.has_value()) {
        assets.addDirectory(*asset_root);
        request.textures = apex::app::FbxPreviewTextureGrant{
            "cli-fbx-assets", &assets};
    }

    const auto result = apex::app::open_fbx_preview_document(request);
    report_fbx_diagnostics(result);

    write_cli_text(std::cout, path.string());
    std::cout << ": FBX "
              << apex::app::fbx_preview_document_status_name(result.status);
    if (result.document.has_value()) {
        const auto& model = result.document->assembly.model;
        std::cout << ", " << apex::formats::walkKn5(model.root).size()
                  << " nodes, " << model.materials.size() << " materials, "
                  << model.textures.size() << " textures, "
                  << result.animations.size() << " animations";
    }
    std::cout << '\n';
    for (std::size_t index = 0U; index < result.animations.size(); ++index) {
        const auto& clip = result.animations[index];
        std::cout << "animation[" << index << "]: name=";
        write_cli_text(std::cout, clip.name);
        std::cout << ", duration=" << clip.duration
                  << ", source-tracks=" << clip.source_track_count
                  << ", tracks=" << clip.animation.tracks.size()
                  << ", frames=" << clip.animation.frameCount << '\n';
    }
    return result.ok() ? 0 : 1;
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

std::uint32_t parse_unsigned_index(std::string_view value,
                                   std::string_view label) {
    std::uint64_t result = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() ||
        result > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string(label) +
            " must be a valid unsigned 32-bit integer");
    }
    return static_cast<std::uint32_t>(result);
}

std::uint32_t parse_lod_index(std::string_view value) {
    return parse_unsigned_index(value, "LOD index");
}

struct WindowShaderSpec {
    std::string family;
    std::filesystem::path vertex;
    std::filesystem::path fragment;
};

struct WindowWorkspaceOptions {
    std::optional<std::filesystem::path> model;
    std::optional<std::filesystem::path> fbx;
    std::optional<std::filesystem::path> fbxAssets;
    std::optional<std::filesystem::path> workspaceRoot;
    std::optional<std::filesystem::path> manifest;
    apex::app::WorkspaceSessionKind kind = apex::app::WorkspaceSessionKind::generic;
    bool kindSpecified = false;
    std::optional<std::filesystem::path> analogInstruments;
    double rpm = 1'000.0;
    bool rpmSpecified = false;
    std::optional<std::filesystem::path> animation;
    std::optional<std::uint32_t> fbxAnimation;
    double animationPosition = 0.0;
    bool animationPositionSpecified = false;
    std::optional<std::uint32_t> lodIndex;
    std::optional<std::filesystem::path> trackCameraSet;
    std::optional<std::uint32_t> trackCameraIndex;
    double trackCameraPosition = 0.0;
    bool trackCameraPositionSpecified = false;
    bool trackCameraPlay = false;
    apex::app::TrackCameraPreviewMode trackCameraMode =
        apex::app::TrackCameraPreviewMode::webgl;
    bool trackCameraModeSpecified = false;
    std::optional<std::filesystem::path> aiSpline;
    apex::app::WorkspaceAiSplineDisplayMode aiSplineMode =
        apex::app::WorkspaceAiSplineDisplayMode::raw;
    bool aiSplineModeSpecified = false;
    std::optional<apex::app::WorkspaceAiSplineInterval> aiSplineInterval;
    bool aiSplineShowLeft = false;
    bool aiSplineShowRight = false;
    std::vector<std::uint32_t> aiSplineIndices;
    bool aiSplineShowCamber = false;
    std::vector<apex::authoring::AiSplinePointPositionEdit> aiSplineEdits;
    bool aiSplineUnlockEdit = false;
    std::optional<std::filesystem::path> aiSplineSaveOnExit;
    std::optional<std::string> nodeSearch;
    std::optional<apex::scene::NodeId> selectedNode;
    bool isolateSelected = false;
    bool showHidden = false;
    bool wireframe = false;
    bool gridVisible = false;
    bool viewAxisVisible = false;
    bool skeletonVisible = false;
    std::string weather;
    bool weatherSpecified = false;
    std::optional<std::filesystem::path> cloudAssets;
    double sunHeading = apex::app::workspace_viewport_default_sun_heading_degrees;
    bool sunHeadingSpecified = false;
    double sunHeight = apex::app::workspace_viewport_default_sun_height_degrees;
    bool sunHeightSpecified = false;
    bool hdr = false;
    bool fxaa = false;
    std::optional<float> exposure;
    bool builtinVulkanKsPerPixel = false;
    std::optional<std::filesystem::path> d3d12KsPerPixelPackage;
    std::optional<std::filesystem::path> d3d12KsPerPixelAtPackage;
    std::vector<WindowShaderSpec> shaders;
    std::optional<std::filesystem::path> authoringOverlayVertex;
    std::optional<std::filesystem::path> authoringOverlayFragment;
    std::optional<std::filesystem::path> selectedMeshVertex;
    std::optional<std::filesystem::path> selectedMeshFragment;
    std::optional<std::filesystem::path> directionalShadowVertex;
    std::optional<std::filesystem::path> directionalShadowAlphaVertex;
    std::optional<std::filesystem::path> directionalShadowAlphaFragment;
    std::optional<std::filesystem::path> directionalShadowSkinnedVertex;
};

bool has_directional_shadow_modules(
    const WindowWorkspaceOptions& options) noexcept {
    return options.directionalShadowVertex.has_value() ||
           options.directionalShadowAlphaVertex.has_value() ||
           options.directionalShadowAlphaFragment.has_value() ||
           options.directionalShadowSkinnedVertex.has_value();
}

bool has_d3d12_native_package(
    const WindowWorkspaceOptions& options) noexcept {
    return options.d3d12KsPerPixelPackage.has_value() ||
           options.d3d12KsPerPixelAtPackage.has_value();
}

struct LoadedWindowWorkspace {
    std::optional<apex::app::WorkspaceSessionDocument> document;
    std::optional<apex::app::FbxPreviewDocumentResult> fbxPreview;
    struct ShaderSet {
        std::vector<apex::render::PipelineShaderModule> modules;
        apex::render::StockMaterialShaderModules descriptor;
    };
    std::vector<ShaderSet> shaderSets;
    std::vector<apex::render::StockMaterialShaderModules> descriptors;
    std::vector<apex::render::StockMaterialD3D12NativeProgram>
        d3d12NativePrograms;
    std::optional<std::vector<apex::render::PipelineShaderModule>>
        authoringOverlayModules;
    std::optional<std::vector<apex::render::PipelineShaderModule>>
        selectedMeshModules;
    std::optional<apex::app::WorkspaceViewportDirectionalShadowOptions>
        directionalShadows;
    struct TrackCamera {
        apex::domain::CameraData camera;
        std::vector<std::array<double, 3U>> rawSplinePoints;
        std::vector<std::array<double, 3U>> splinePoints;
        std::optional<apex::app::InstalledEditorTrackCameraSpline>
            installedEditorSpline;
    };
    std::optional<TrackCamera> trackCamera;
    std::unique_ptr<apex::app::WorkspaceAiSplineController>
        aiSplineController;
    std::optional<apex::app::WorkspaceAiSplineOverlaySet>
        readOnlyAiSplineOverlays;
    apex::app::WorkspaceSelectionState selection;
    bool animationSkinningRequired = false;

    [[nodiscard]] apex::app::WorkspaceSessionDocument* activeDocument()
        noexcept {
        if (fbxPreview.has_value())
            return fbxPreview->document.has_value()
                       ? &*fbxPreview->document
                       : nullptr;
        return document.has_value() ? &*document : nullptr;
    }

    [[nodiscard]] const apex::app::WorkspaceSessionDocument* activeDocument()
        const noexcept {
        if (fbxPreview.has_value())
            return fbxPreview->document.has_value()
                       ? &*fbxPreview->document
                       : nullptr;
        return document.has_value() ? &*document : nullptr;
    }

    [[nodiscard]] const apex::app::WorkspaceAiSplineOverlaySet*
    aiSplineOverlays() const noexcept {
        if (aiSplineController != nullptr)
            return &aiSplineController->overlays();
        return readOnlyAiSplineOverlays.has_value()
                   ? &*readOnlyAiSplineOverlays
                   : nullptr;
    }
};

apex::app::WorkspaceSessionKind parse_workspace_kind(std::string_view value) {
    if (value == "track") return apex::app::WorkspaceSessionKind::track;
    if (value == "carLods" || value == "car-lods")
        return apex::app::WorkspaceSessionKind::carLods;
    throw std::runtime_error("workspace kind must be track or carLods");
}

apex::app::TrackCameraPreviewMode
parse_track_camera_mode(std::string_view value) {
    if (value == "webgl")
        return apex::app::TrackCameraPreviewMode::webgl;
    if (value == "installed-editor")
        return apex::app::TrackCameraPreviewMode::installed_editor;
    throw std::runtime_error(
        "track-camera mode must be webgl or installed-editor");
}

apex::app::WorkspaceAiSplineDisplayMode parse_ai_spline_mode(
    std::string_view value) {
    if (value == "raw")
        return apex::app::WorkspaceAiSplineDisplayMode::raw;
    if (value == "interpolated")
        return apex::app::WorkspaceAiSplineDisplayMode::interpolated;
    throw std::runtime_error(
        "AI spline mode must be raw or interpolated");
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

float parse_finite_float(std::string_view value, std::string_view label) {
    const double parsed = parse_finite_number(value, label);
    const float result = static_cast<float>(parsed);
    if (!std::isfinite(result))
        throw std::runtime_error(std::string(label) + " is outside the finite float range");
    return result;
}

apex::formats::AiSpline normalize_ai_spline_for_editing(
    apex::formats::AiSpline source,
    const apex::authoring::AiSplineSessionLimits& session_limits = {}) {
    if (source.version == 7U) return source;
    if (source.version != 2U) {
        throw std::runtime_error(
            "AI spline editing supports native file versions 2 and 7");
    }
    auto converted = apex::app::convertAiSplineV2ToV7Model(
        source,
        apex::app::aiSplineLegacyConversionLimitsForSession(
            session_limits));
    if (!converted.ok()) {
        if (converted.diagnostics.empty())
            throw std::runtime_error(
                "AI spline version-2 conversion failed without a diagnostic");
        const auto& diagnostic = converted.diagnostics.back();
        throw std::runtime_error(diagnostic.code + ": " +
                                 diagnostic.message);
    }
    return std::move(*converted.model);
}

int edit_ai_spline(int argc, char** argv) {
    if (argc < 6)
        throw std::runtime_error("invalid --edit-ai-spline arguments");

    const std::filesystem::path input_path = argv[2];
    const std::filesystem::path output_path = argv[3];
    std::optional<std::uint32_t> point_index;
    apex::authoring::AiSplineWaypointEdit edit;
    std::unordered_set<std::string> specified;
    std::size_t edit_field_count = 0U;

    auto set_field = [&](std::string_view option, std::string_view value,
                         float& field) {
        const std::string owned_option(option);
        if (!specified.insert(owned_option).second)
            throw std::runtime_error("duplicate " + owned_option + " option");
        field = parse_finite_float(value, owned_option);
        ++edit_field_count;
    };

    for (int argument = 4; argument < argc; ++argument) {
        const std::string_view option = argv[argument];
        if (argument + 1 >= argc)
            throw std::runtime_error(std::string(option) + " requires a value");
        const std::string_view value = argv[++argument];
        if (option == "--index") {
            if (point_index.has_value())
                throw std::runtime_error("duplicate --index option");
            point_index = parse_unsigned_index(value, "AI spline point index");
        } else if (option == "--set-radius") {
            set_field(option, value, edit.replacement.radius);
        } else if (option == "--set-side0") {
            set_field(option, value, edit.replacement.side0);
        } else if (option == "--set-side1") {
            set_field(option, value, edit.replacement.side1);
        } else if (option == "--set-camber-degrees") {
            set_field(option, value, edit.replacement.camberDegrees);
        } else if (option == "--set-length") {
            set_field(option, value, edit.replacement.length);
        } else if (option == "--set-grade") {
            set_field(option, value, edit.replacement.grade);
        } else if (option == "--add-radius") {
            set_field(option, value, edit.additive.radius);
        } else if (option == "--add-side0") {
            set_field(option, value, edit.additive.side0);
        } else if (option == "--add-side1") {
            set_field(option, value, edit.additive.side1);
        } else if (option == "--add-camber-degrees") {
            set_field(option, value, edit.additive.camberDegrees);
        } else if (option == "--add-length") {
            set_field(option, value, edit.additive.length);
        } else if (option == "--add-grade") {
            set_field(option, value, edit.additive.grade);
        } else {
            throw std::runtime_error("unknown --edit-ai-spline option: " +
                                     std::string(option));
        }
    }

    if (!point_index.has_value())
        throw std::runtime_error("--edit-ai-spline requires --index");
    if (edit_field_count == 0U)
        throw std::runtime_error("--edit-ai-spline requires at least one edit field");

    const auto input_bytes = read_file(input_path);
    auto spline = normalize_ai_spline_for_editing(
        apex::formats::parseAiSpline(input_bytes, input_path.string()));
    apex::authoring::AiSplineSession session(std::move(spline));
    const auto result = session.commitWaypointEdit(*point_index, edit);
    if (!result.ok()) {
        if (result.diagnostics.empty())
            throw std::runtime_error("AI spline waypoint edit failed");
        const auto& diagnostic = result.diagnostics.back();
        throw std::runtime_error(diagnostic.code + ": " + diagnostic.message);
    }

    apex::platform::writeFileExclusive(output_path, session.currentBytes());
    std::cout << "AI spline waypoint edited: point=" << *result.pointIndex
              << ", payload=" << *result.payloadIndex
              << ", changed=" << (result.changed ? "yes" : "no")
              << ", output=";
    write_cli_text(std::cout, output_path.string());
    std::cout << '\n';
    return 0;
}

int save_ai_spline(int argc, char** argv) {
    if (argc != 4)
        throw std::runtime_error("invalid --save-ai-spline arguments");

    const std::filesystem::path input_path = argv[2];
    const std::filesystem::path output_path = argv[3];
    const auto input_bytes = read_file(input_path);
    auto spline = normalize_ai_spline_for_editing(
        apex::formats::parseAiSpline(input_bytes, input_path.string()));
    apex::authoring::AiSplineSession session(std::move(spline));
    const auto saved = session.buildSaveBytes();
    if (!saved.ok()) {
        if (saved.diagnostics.empty())
            throw std::runtime_error("AI spline save failed");
        const auto& diagnostic = saved.diagnostics.back();
        throw std::runtime_error(diagnostic.code + ": " +
                                 diagnostic.message);
    }

    apex::platform::writeFileAtomicReplace(output_path, saved.bytes);
    std::cout << "AI spline saved: revision=" << saved.revision
              << ", grid=rebuilt, output=";
    write_cli_text(std::cout, output_path.string());
    std::cout << '\n';
    return 0;
}

int convert_ai_spline_v2(int argc, char** argv) {
    if (argc != 4)
        throw std::runtime_error(
            "invalid --convert-ai-spline-v2 arguments");

    const std::filesystem::path input_path = argv[2];
    const std::filesystem::path output_path = argv[3];
    if (input_path == output_path) {
        throw std::runtime_error(
            "--convert-ai-spline-v2 requires different input and output paths");
    }

    const auto input_bytes = read_file(input_path);
    const auto source =
        apex::formats::parseAiSpline(input_bytes, input_path.string());
    const auto converted = apex::app::convertAiSplineV2ToV7File(source);
    if (!converted.ok()) {
        if (converted.diagnostics.empty()) {
            throw std::runtime_error(
                "AI spline version-2 conversion failed");
        }
        const auto& diagnostic = converted.diagnostics.back();
        throw std::runtime_error(diagnostic.code + ": " +
                                 diagnostic.message);
    }

    apex::platform::writeFileExclusive(output_path, converted.bytes);
    std::cout << "AI spline converted: source-version=2, output-version=7"
              << ", points=" << converted.pointCount << ", grid="
              << (converted.gridBuilt ? "rebuilt" : "empty")
              << ", output=";
    write_cli_text(std::cout, output_path.string());
    std::cout << '\n';
    return 0;
}

int invert_ai_spline(int argc, char** argv) {
    if (argc < 6)
        throw std::runtime_error("invalid --invert-ai-spline arguments");

    const std::filesystem::path input_path = argv[2];
    const std::filesystem::path output_path = argv[3];
    std::vector<std::uint32_t> selection;
    std::unordered_set<std::uint32_t> membership;
    std::size_t selection_entry_count = 0U;
    for (int argument = 4; argument < argc; ++argument) {
        const std::string_view option = argv[argument];
        if (option != "--index")
            throw std::runtime_error("unknown --invert-ai-spline option: " +
                                     std::string(option));
        if (argument + 1 >= argc)
            throw std::runtime_error("--index requires a value");
        if (selection_entry_count >=
            apex::authoring::aiSplineMaxSelectionEntries)
            throw std::runtime_error(
                "AI spline selection exceeds its entry limit");
        ++selection_entry_count;
        const auto point_index = parse_unsigned_index(
            argv[++argument], "AI spline point index");
        if (membership.insert(point_index).second)
            selection.push_back(point_index);
    }
    if (selection.empty())
        throw std::runtime_error("--invert-ai-spline requires --index");

    const auto input_bytes = read_file(input_path);
    auto spline = normalize_ai_spline_for_editing(
        apex::formats::parseAiSpline(input_bytes, input_path.string()));
    apex::authoring::AiSplineSession session(std::move(spline));
    const auto result = session.invertSelectedCamber(selection);
    if (!result.ok()) {
        if (result.diagnostics.empty())
            throw std::runtime_error("AI spline camber inversion failed");
        const auto& diagnostic = result.diagnostics.back();
        throw std::runtime_error(diagnostic.code + ": " + diagnostic.message);
    }

    apex::platform::writeFileExclusive(output_path, session.currentBytes());
    std::cout << "AI spline camber inverted: selected=" << selection.size()
              << ", changed=" << (result.changed ? "yes" : "no")
              << ", output=";
    write_cli_text(std::cout, output_path.string());
    std::cout << '\n';
    return 0;
}

int set_ai_spline_point(int argc, char** argv) {
    if (argc < 10)
        throw std::runtime_error("invalid --set-ai-spline-point arguments");

    const std::filesystem::path input_path = argv[2];
    const std::filesystem::path output_path = argv[3];
    std::optional<std::uint32_t> point_index;
    std::optional<std::array<float, 3>> position;
    for (int argument = 4; argument < argc; ++argument) {
        const std::string_view option = argv[argument];
        if (option == "--index") {
            if (point_index.has_value())
                throw std::runtime_error("duplicate --index option");
            if (argument + 1 >= argc)
                throw std::runtime_error("--index requires a value");
            point_index =
                parse_unsigned_index(argv[++argument], "AI spline point index");
        } else if (option == "--position") {
            if (position.has_value())
                throw std::runtime_error("duplicate --position option");
            if (argument + 3 >= argc)
                throw std::runtime_error("--position requires x, y, and z");
            position = std::array<float, 3>{
                parse_finite_float(argv[++argument], "AI spline point x"),
                parse_finite_float(argv[++argument], "AI spline point y"),
                parse_finite_float(argv[++argument], "AI spline point z")};
        } else {
            throw std::runtime_error("unknown --set-ai-spline-point option: " +
                                     std::string(option));
        }
    }
    if (!point_index.has_value())
        throw std::runtime_error("--set-ai-spline-point requires --index");
    if (!position.has_value())
        throw std::runtime_error("--set-ai-spline-point requires --position");

    const auto input_bytes = read_file(input_path);
    auto spline = normalize_ai_spline_for_editing(
        apex::formats::parseAiSpline(input_bytes, input_path.string()));
    apex::authoring::AiSplineSession session(std::move(spline));
    const auto result = session.setPointPosition(*point_index, *position);
    if (!result.ok()) {
        if (result.diagnostics.empty())
            throw std::runtime_error("AI spline point-position edit failed");
        const auto& diagnostic = result.diagnostics.back();
        throw std::runtime_error(diagnostic.code + ": " + diagnostic.message);
    }

    apex::platform::writeFileExclusive(output_path, session.currentBytes());
    std::cout << "AI spline point position set: point=" << *result.pointIndex
              << ", changed=" << (result.changed ? "yes" : "no")
              << ", grid=" << (result.changed ? "rebuilt" : "preserved")
              << ", output=";
    write_cli_text(std::cout, output_path.string());
    std::cout << '\n';
    return 0;
}

int set_ai_spline_points(int argc, char** argv) {
    if (argc < 9)
        throw std::runtime_error("invalid --set-ai-spline-points arguments");

    const std::filesystem::path input_path = argv[2];
    const std::filesystem::path output_path = argv[3];
    std::vector<apex::authoring::AiSplinePointPositionEdit> edits;
    for (int argument = 4; argument < argc; ++argument) {
        const std::string_view option = argv[argument];
        if (option != "--point")
            throw std::runtime_error("unknown --set-ai-spline-points option: " +
                                     std::string(option));
        if (argument + 4 >= argc)
            throw std::runtime_error("--point requires an index, x, y, and z");
        if (edits.size() >= apex::authoring::aiSplineMaxSelectionEntries)
            throw std::runtime_error(
                "AI spline point-position edit count exceeds its entry limit");
        apex::authoring::AiSplinePointPositionEdit edit;
        edit.pointIndex =
            parse_unsigned_index(argv[++argument], "AI spline point index");
        edit.position = {
            parse_finite_float(argv[++argument], "AI spline point x"),
            parse_finite_float(argv[++argument], "AI spline point y"),
            parse_finite_float(argv[++argument], "AI spline point z")};
        edits.push_back(edit);
    }

    const auto input_bytes = read_file(input_path);
    auto spline = normalize_ai_spline_for_editing(
        apex::formats::parseAiSpline(input_bytes, input_path.string()));
    apex::authoring::AiSplineSession session(std::move(spline));
    const auto result = session.setPointPositions(edits);
    if (!result.ok()) {
        if (result.diagnostics.empty())
            throw std::runtime_error("AI spline point-position batch failed");
        const auto& diagnostic = result.diagnostics.back();
        throw std::runtime_error(diagnostic.code + ": " + diagnostic.message);
    }

    apex::platform::writeFileExclusive(output_path, session.currentBytes());
    std::cout << "AI spline point positions set: requested=" << edits.size()
              << ", applied=" << result.applied
              << ", last-point=" << *result.pointIndex
              << ", changed=" << (result.changed ? "yes" : "no")
              << ", grid=" << (result.changed ? "rebuilt" : "preserved")
              << ", output=";
    write_cli_text(std::cout, output_path.string());
    std::cout << '\n';
    return 0;
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
workspace_camera_move_for_key(apex::platform::WindowKey key) noexcept {
    switch (key) {
    case apex::platform::WindowKey::w:
        return apex::app::WorkspaceViewportCameraMove::forward;
    case apex::platform::WindowKey::s:
        return apex::app::WorkspaceViewportCameraMove::backward;
    case apex::platform::WindowKey::a:
        return apex::app::WorkspaceViewportCameraMove::left;
    case apex::platform::WindowKey::d:
        return apex::app::WorkspaceViewportCameraMove::right;
    case apex::platform::WindowKey::q:
        return apex::app::WorkspaceViewportCameraMove::down;
    case apex::platform::WindowKey::e:
        return apex::app::WorkspaceViewportCameraMove::up;
    default:
        return std::nullopt;
    }
}

std::optional<apex::app::WorkspaceAiSplineManualKey>
workspace_ai_spline_manual_key_for_window_key(
    apex::platform::WindowKey key) noexcept {
    switch (key) {
    case apex::platform::WindowKey::keypad_8:
        return apex::app::WorkspaceAiSplineManualKey::forward;
    case apex::platform::WindowKey::keypad_2:
        return apex::app::WorkspaceAiSplineManualKey::backward;
    case apex::platform::WindowKey::keypad_4:
        return apex::app::WorkspaceAiSplineManualKey::left;
    case apex::platform::WindowKey::keypad_6:
        return apex::app::WorkspaceAiSplineManualKey::right;
    case apex::platform::WindowKey::keypad_9:
        return apex::app::WorkspaceAiSplineManualKey::up;
    case apex::platform::WindowKey::keypad_3:
        return apex::app::WorkspaceAiSplineManualKey::down;
    case apex::platform::WindowKey::left_control:
        return apex::app::WorkspaceAiSplineManualKey::left_control;
    case apex::platform::WindowKey::right_control:
        return apex::app::WorkspaceAiSplineManualKey::right_control;
    default:
        return std::nullopt;
    }
}

WindowWorkspaceOptions parse_window_workspace_options(int argc, char** argv,
                                                       int first_option,
                                                       bool& validation,
                                                       std::uint64_t& frame_limit) {
    WindowWorkspaceOptions result;
    std::unordered_set<std::uint32_t> ai_spline_index_membership;
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
        } else if (option == "--fbx") {
            if (result.fbx.has_value())
                throw std::runtime_error("duplicate --fbx option");
            result.fbx = std::filesystem::path(require_value("--fbx"));
        } else if (option == "--fbx-assets") {
            if (result.fbxAssets.has_value())
                throw std::runtime_error("duplicate --fbx-assets option");
            result.fbxAssets =
                std::filesystem::path(require_value("--fbx-assets"));
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
        } else if (option == "--fbx-animation") {
            if (result.fbxAnimation.has_value())
                throw std::runtime_error("duplicate --fbx-animation option");
            result.fbxAnimation = parse_unsigned_index(
                require_value("--fbx-animation"), "FBX animation index");
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
        } else if (option == "--track-camera-set") {
            if (result.trackCameraSet.has_value())
                throw std::runtime_error(
                    "duplicate --track-camera-set option");
            result.trackCameraSet = std::filesystem::path(
                require_value("--track-camera-set"));
        } else if (option == "--track-camera-index") {
            if (result.trackCameraIndex.has_value())
                throw std::runtime_error(
                    "duplicate --track-camera-index option");
            result.trackCameraIndex = parse_unsigned_index(
                require_value("--track-camera-index"),
                "track-camera index");
        } else if (option == "--track-camera-position") {
            if (result.trackCameraPositionSpecified)
                throw std::runtime_error(
                    "duplicate --track-camera-position option");
            result.trackCameraPosition = parse_finite_number(
                require_value("--track-camera-position"),
                "track-camera position");
            result.trackCameraPositionSpecified = true;
        } else if (option == "--track-camera-play") {
            if (result.trackCameraPlay)
                throw std::runtime_error(
                    "duplicate --track-camera-play option");
            result.trackCameraPlay = true;
        } else if (option == "--track-camera-mode") {
            if (result.trackCameraModeSpecified)
                throw std::runtime_error(
                    "duplicate --track-camera-mode option");
            result.trackCameraMode = parse_track_camera_mode(
                require_value("--track-camera-mode"));
            result.trackCameraModeSpecified = true;
        } else if (option == "--ai-spline") {
            if (result.aiSpline.has_value())
                throw std::runtime_error("duplicate --ai-spline option");
            result.aiSpline =
                std::filesystem::path(require_value("--ai-spline"));
        } else if (option == "--ai-spline-mode") {
            if (result.aiSplineModeSpecified)
                throw std::runtime_error(
                    "duplicate --ai-spline-mode option");
            result.aiSplineMode = parse_ai_spline_mode(
                require_value("--ai-spline-mode"));
            result.aiSplineModeSpecified = true;
        } else if (option == "--ai-spline-interval") {
            if (result.aiSplineInterval.has_value())
                throw std::runtime_error(
                    "duplicate --ai-spline-interval option");
            const double begin = parse_finite_number(
                require_value("--ai-spline-interval"),
                "AI spline interval start");
            const double end = parse_finite_number(
                require_value("--ai-spline-interval"),
                "AI spline interval end");
            if (begin < 0.0 || end > 1.0 || begin > end)
                throw std::runtime_error(
                    "AI spline interval must be ordered and from zero to one");
            result.aiSplineInterval = {
                static_cast<float>(begin), static_cast<float>(end)};
        } else if (option == "--ai-spline-show-left") {
            if (result.aiSplineShowLeft)
                throw std::runtime_error(
                    "duplicate --ai-spline-show-left option");
            result.aiSplineShowLeft = true;
        } else if (option == "--ai-spline-show-right") {
            if (result.aiSplineShowRight)
                throw std::runtime_error(
                    "duplicate --ai-spline-show-right option");
            result.aiSplineShowRight = true;
        } else if (option == "--ai-spline-index") {
            const std::uint32_t selected_index = parse_unsigned_index(
                require_value("--ai-spline-index"), "AI spline index");
            if (ai_spline_index_membership.insert(selected_index).second) {
                if (result.aiSplineIndices.size() >=
                    apex::app::workspace_ai_spline_max_selection_points)
                    throw std::runtime_error(
                        "AI spline selection exceeds the bounded marker limit");
                result.aiSplineIndices.push_back(selected_index);
            }
        } else if (option == "--ai-spline-show-camber") {
            if (result.aiSplineShowCamber)
                throw std::runtime_error(
                    "duplicate --ai-spline-show-camber option");
            result.aiSplineShowCamber = true;
        } else if (option == "--ai-spline-edit-point") {
            if (result.aiSplineEdits.size() >=
                apex::authoring::aiSplineMaxSelectionEntries)
                throw std::runtime_error(
                    "AI spline edit count exceeds its bounded limit");
            apex::authoring::AiSplinePointPositionEdit edit;
            edit.pointIndex = parse_unsigned_index(
                require_value("--ai-spline-edit-point"),
                "AI spline edit point index");
            edit.position = {
                parse_finite_float(require_value("--ai-spline-edit-point"),
                                   "AI spline edit X"),
                parse_finite_float(require_value("--ai-spline-edit-point"),
                                   "AI spline edit Y"),
                parse_finite_float(require_value("--ai-spline-edit-point"),
                                   "AI spline edit Z")};
            result.aiSplineEdits.push_back(edit);
        } else if (option == "--ai-spline-unlock-edit") {
            if (result.aiSplineUnlockEdit)
                throw std::runtime_error(
                    "duplicate --ai-spline-unlock-edit option");
            result.aiSplineUnlockEdit = true;
        } else if (option == "--ai-spline-save-on-exit") {
            if (result.aiSplineSaveOnExit.has_value())
                throw std::runtime_error(
                    "duplicate --ai-spline-save-on-exit option");
            result.aiSplineSaveOnExit = std::filesystem::path(
                require_value("--ai-spline-save-on-exit"));
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
        } else if (option == "--grid") {
            if (result.gridVisible)
                throw std::runtime_error("duplicate --grid option");
            result.gridVisible = true;
        } else if (option == "--view-axis") {
            if (result.viewAxisVisible)
                throw std::runtime_error("duplicate --view-axis option");
            result.viewAxisVisible = true;
        } else if (option == "--skeleton") {
            if (result.skeletonVisible)
                throw std::runtime_error("duplicate --skeleton option");
            result.skeletonVisible = true;
        } else if (option == "--weather") {
            if (result.weatherSpecified)
                throw std::runtime_error("duplicate --weather option");
            result.weather = std::string(require_value("--weather"));
            result.weatherSpecified = true;
        } else if (option == "--cloud-assets") {
            if (result.cloudAssets.has_value())
                throw std::runtime_error("duplicate --cloud-assets option");
            result.cloudAssets = std::filesystem::path(
                require_value("--cloud-assets"));
        } else if (option == "--sun-heading") {
            if (result.sunHeadingSpecified)
                throw std::runtime_error("duplicate --sun-heading option");
            result.sunHeading = parse_finite_number(
                require_value("--sun-heading"), "sun heading");
            result.sunHeadingSpecified = true;
        } else if (option == "--sun-height") {
            if (result.sunHeightSpecified)
                throw std::runtime_error("duplicate --sun-height option");
            result.sunHeight = parse_finite_number(
                require_value("--sun-height"), "sun height");
            result.sunHeightSpecified = true;
        } else if (option == "--hdr") {
            if (result.hdr)
                throw std::runtime_error("duplicate --hdr option");
            result.hdr = true;
        } else if (option == "--fxaa") {
            if (result.fxaa)
                throw std::runtime_error("duplicate --fxaa option");
            result.fxaa = true;
        } else if (option == "--exposure") {
            if (result.exposure.has_value())
                throw std::runtime_error("duplicate --exposure option");
            const float exposure = parse_finite_float(
                require_value("--exposure"), "exposure");
            if (exposure < 0.0F)
                throw std::runtime_error(
                    "exposure must be finite and nonnegative");
            result.exposure = exposure;
        } else if (option == "--builtin-vulkan-ks-per-pixel") {
            if (result.builtinVulkanKsPerPixel)
                throw std::runtime_error(
                    "duplicate --builtin-vulkan-ks-per-pixel option");
            result.builtinVulkanKsPerPixel = true;
        } else if (option == "--d3d12-ks-per-pixel-package") {
            if (result.d3d12KsPerPixelPackage.has_value())
                throw std::runtime_error(
                    "duplicate --d3d12-ks-per-pixel-package option");
            result.d3d12KsPerPixelPackage = std::filesystem::path(
                require_value("--d3d12-ks-per-pixel-package"));
        } else if (option == "--d3d12-ks-per-pixel-at-package") {
            if (result.d3d12KsPerPixelAtPackage.has_value())
                throw std::runtime_error(
                    "duplicate --d3d12-ks-per-pixel-at-package option");
            result.d3d12KsPerPixelAtPackage = std::filesystem::path(
                require_value("--d3d12-ks-per-pixel-at-package"));
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
        } else if (option == "--directional-shadow-vertex") {
            if (result.directionalShadowVertex.has_value())
                throw std::runtime_error(
                    "duplicate --directional-shadow-vertex option");
            result.directionalShadowVertex = std::filesystem::path(
                require_value("--directional-shadow-vertex"));
        } else if (option == "--directional-shadow-alpha-vertex") {
            if (result.directionalShadowAlphaVertex.has_value())
                throw std::runtime_error(
                    "duplicate --directional-shadow-alpha-vertex option");
            result.directionalShadowAlphaVertex = std::filesystem::path(
                require_value("--directional-shadow-alpha-vertex"));
        } else if (option == "--directional-shadow-alpha-fragment") {
            if (result.directionalShadowAlphaFragment.has_value())
                throw std::runtime_error(
                    "duplicate --directional-shadow-alpha-fragment option");
            result.directionalShadowAlphaFragment = std::filesystem::path(
                require_value("--directional-shadow-alpha-fragment"));
        } else if (option == "--directional-shadow-skinned-vertex") {
            if (result.directionalShadowSkinnedVertex.has_value())
                throw std::runtime_error(
                    "duplicate --directional-shadow-skinned-vertex option");
            result.directionalShadowSkinnedVertex = std::filesystem::path(
                require_value("--directional-shadow-skinned-vertex"));
        } else if (option == "--selection-axis-vertex" ||
                   option == "--authoring-overlay-vertex") {
            if (result.authoringOverlayVertex.has_value())
                throw std::runtime_error(
                    "duplicate authoring-overlay vertex option");
            result.authoringOverlayVertex = std::filesystem::path(
                require_value(option));
        } else if (option == "--selection-axis-fragment" ||
                   option == "--authoring-overlay-fragment") {
            if (result.authoringOverlayFragment.has_value())
                throw std::runtime_error(
                    "duplicate authoring-overlay fragment option");
            result.authoringOverlayFragment = std::filesystem::path(
                require_value(option));
        } else if (option == "--selected-mesh-vertex") {
            if (result.selectedMeshVertex.has_value())
                throw std::runtime_error(
                    "duplicate selected-mesh vertex option");
            result.selectedMeshVertex = std::filesystem::path(
                require_value(option));
        } else if (option == "--selected-mesh-fragment") {
            if (result.selectedMeshFragment.has_value())
                throw std::runtime_error(
                    "duplicate selected-mesh fragment option");
            result.selectedMeshFragment = std::filesystem::path(
                require_value(option));
        } else {
            throw std::runtime_error("unknown window option");
        }
    }
    const auto model_source_count =
        static_cast<unsigned>(result.model.has_value()) +
        static_cast<unsigned>(result.fbx.has_value()) +
        static_cast<unsigned>(result.workspaceRoot.has_value());
    if (model_source_count > 1U)
        throw std::runtime_error(
            "--model, --fbx, and --workspace-root are mutually exclusive");
    if (result.fbxAssets.has_value() && !result.fbx.has_value())
        throw std::runtime_error("--fbx-assets requires --fbx");
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
    if (result.fbx.has_value() &&
        !has_d3d12_native_package(result) &&
        std::none_of(result.shaders.begin(), result.shaders.end(),
                     [](const auto& shader) {
                         return shader.family == "ksPerPixel";
                     }))
        throw std::runtime_error(
            "--fbx requires a ksPerPixel shader family");
    const bool has_model_source = model_source_count != 0U;
    if (!has_model_source &&
        (result.builtinVulkanKsPerPixel ||
         has_d3d12_native_package(result) ||
         !result.shaders.empty() ||
         has_directional_shadow_modules(result) ||
         result.authoringOverlayVertex.has_value() ||
         result.authoringOverlayFragment.has_value() ||
         result.selectedMeshVertex.has_value() ||
         result.selectedMeshFragment.has_value()))
        throw std::runtime_error("shader modules require a workspace model");
    if (result.authoringOverlayVertex.has_value() !=
        result.authoringOverlayFragment.has_value())
        throw std::runtime_error(
            "authoring-overlay vertex and fragment modules must be supplied together");
    if (result.authoringOverlayVertex.has_value() &&
        !result.selectedNode.has_value() && !result.gridVisible &&
        !result.viewAxisVisible && !result.skeletonVisible &&
        !result.aiSpline.has_value())
        throw std::runtime_error(
            "authoring-overlay shader modules require --selected-node, --grid, --view-axis, --skeleton, or --ai-spline");
    if (result.gridVisible && !result.authoringOverlayVertex.has_value())
        throw std::runtime_error(
            "--grid requires authoring-overlay shader modules");
    if (result.viewAxisVisible && !result.authoringOverlayVertex.has_value())
        throw std::runtime_error(
            "--view-axis requires authoring-overlay shader modules");
    if (result.skeletonVisible && !result.authoringOverlayVertex.has_value())
        throw std::runtime_error(
            "--skeleton requires authoring-overlay shader modules");
    if (result.selectedMeshVertex.has_value() !=
        result.selectedMeshFragment.has_value())
        throw std::runtime_error(
            "selected-mesh vertex and fragment modules must be supplied together");
    if (result.selectedMeshVertex.has_value() &&
        !result.selectedNode.has_value())
        throw std::runtime_error(
            "selected-mesh shader modules require --selected-node");
    if (result.directionalShadowAlphaVertex.has_value() !=
        result.directionalShadowAlphaFragment.has_value())
        throw std::runtime_error(
            "directional-shadow alpha vertex and fragment modules must be supplied together");
    if (has_directional_shadow_modules(result) && result.shaders.empty() &&
        !result.builtinVulkanKsPerPixel &&
        !has_d3d12_native_package(result)) {
        if (result.directionalShadowVertex.has_value() &&
            !result.directionalShadowAlphaVertex.has_value() &&
            !result.directionalShadowSkinnedVertex.has_value())
            throw std::runtime_error(
                "--directional-shadow-vertex requires receiver-capable material shader modules");
        throw std::runtime_error(
            "directional-shadow modules require receiver-capable material shader modules");
    }
    if (result.builtinVulkanKsPerPixel && has_d3d12_native_package(result))
        throw std::runtime_error(
            std::string("--builtin-vulkan-ks-per-pixel and ") +
            std::string(result.d3d12KsPerPixelAtPackage.has_value()
                            ? "--d3d12-ks-per-pixel-at-package"
                            : "--d3d12-ks-per-pixel-package") +
            " are mutually exclusive");
    if (result.rpmSpecified && !result.analogInstruments.has_value())
        throw std::runtime_error("--rpm requires --analog-instruments");
    if (result.analogInstruments.has_value() &&
        !has_model_source)
        throw std::runtime_error("--analog-instruments requires a workspace model");
    if (result.fbx.has_value() && result.analogInstruments.has_value())
        throw std::runtime_error(
            "--analog-instruments is not supported with --fbx");
    if (result.animation.has_value() && result.fbxAnimation.has_value())
        throw std::runtime_error(
            "--animation and --fbx-animation are mutually exclusive");
    if (result.animationPositionSpecified && !result.animation.has_value() &&
        !result.fbxAnimation.has_value())
        throw std::runtime_error(
            "--animation-position requires --animation or --fbx-animation");
    if (result.animation.has_value() &&
        !has_model_source)
        throw std::runtime_error("--animation requires a workspace model");
    if (result.fbx.has_value() && result.animation.has_value())
        throw std::runtime_error("--animation is not supported with --fbx");
    if (result.fbxAnimation.has_value() && !result.fbx.has_value())
        throw std::runtime_error("--fbx-animation requires --fbx");
    if (result.lodIndex.has_value() &&
        (!result.workspaceRoot.has_value() ||
         result.kind != apex::app::WorkspaceSessionKind::carLods)) {
        throw std::runtime_error("--lod-index requires a carLods workspace");
    }
    if (result.trackCameraSet.has_value() !=
        result.trackCameraIndex.has_value())
        throw std::runtime_error(
            "--track-camera-set and --track-camera-index must be supplied together");
    if ((result.trackCameraPositionSpecified || result.trackCameraPlay) &&
        !result.trackCameraSet.has_value())
        throw std::runtime_error(
            "track-camera position and playback require --track-camera-set");
    if (result.trackCameraModeSpecified &&
        !result.trackCameraSet.has_value())
        throw std::runtime_error(
            "--track-camera-mode requires --track-camera-set");
    if (result.trackCameraPosition < 0.0 ||
        result.trackCameraPosition > 1.0)
        throw std::runtime_error(
            "track-camera position must be from zero to one");
    if (result.trackCameraSet.has_value() && !has_model_source)
        throw std::runtime_error(
            "track-camera options require a workspace model");
    if (result.fbx.has_value() && result.trackCameraSet.has_value())
        throw std::runtime_error(
            "track-camera options are not supported with --fbx");
    if (result.aiSpline.has_value() && !has_model_source)
        throw std::runtime_error("--ai-spline requires a workspace model");
    if (result.fbx.has_value() && result.aiSpline.has_value())
        throw std::runtime_error("--ai-spline is not supported with --fbx");
    if (result.aiSpline.has_value() &&
        !result.authoringOverlayVertex.has_value())
        throw std::runtime_error(
            "--ai-spline requires authoring-overlay shader modules");
    if (result.aiSplineModeSpecified && !result.aiSpline.has_value())
        throw std::runtime_error(
            "--ai-spline-mode requires --ai-spline");
    if (result.aiSplineInterval.has_value() && !result.aiSpline.has_value())
        throw std::runtime_error(
            "--ai-spline-interval requires --ai-spline");
    if ((result.aiSplineShowLeft || result.aiSplineShowRight ||
         !result.aiSplineIndices.empty() ||
         result.aiSplineShowCamber || !result.aiSplineEdits.empty() ||
         result.aiSplineUnlockEdit || result.aiSplineSaveOnExit.has_value()) &&
        !result.aiSpline.has_value())
        throw std::runtime_error(
            "AI spline overlays require --ai-spline");
    if (result.isolateSelected && !result.selectedNode.has_value())
        throw std::runtime_error("--isolate-selected requires --selected-node");
    const bool selection_options = result.nodeSearch.has_value() ||
                                   result.selectedNode.has_value() ||
                                   result.showHidden || result.wireframe ||
                                   result.gridVisible || result.viewAxisVisible;
    if (selection_options && !has_model_source)
        throw std::runtime_error("hierarchy options require a workspace model");
    const bool lighting_options = result.weatherSpecified ||
                                  result.sunHeadingSpecified ||
                                  result.sunHeightSpecified ||
                                  result.cloudAssets.has_value();
    if (lighting_options && !has_model_source)
        throw std::runtime_error("lighting options require a workspace model");
    if (result.exposure.has_value() && !result.hdr)
        throw std::runtime_error("--exposure requires --hdr");
    if (result.fxaa && !result.hdr)
        throw std::runtime_error("--fxaa requires --hdr");
    if (result.hdr && !has_model_source)
        throw std::runtime_error(
            "post-processing options require a workspace model");
    return result;
}

void report_workspace_diagnostics(const apex::app::WorkspaceSessionResult& result) {
    for (const auto& item : result.diagnostics)
        std::cerr << item.code << " [" << item.path << "]: " << item.message << '\n';
}

void load_window_workspace(const WindowWorkspaceOptions& options,
                           apex::render::Backend backend,
                           LoadedWindowWorkspace& loaded) {
    if (!options.model.has_value() && !options.fbx.has_value() &&
        !options.workspaceRoot.has_value())
        return;

    if (options.fbx.has_value()) {
        const auto bytes = read_file(*options.fbx);
        apex::assets::AssetSource assets;
        apex::app::FbxPreviewDocumentRequest request;
        request.source = options.fbx->filename().generic_string();
        request.bytes = bytes;
        if (options.fbxAssets.has_value()) {
            assets.addDirectory(*options.fbxAssets);
            request.textures = apex::app::FbxPreviewTextureGrant{
                "window-fbx-assets", &assets};
        }
        auto opened = apex::app::open_fbx_preview_document(request);
        report_fbx_diagnostics(opened);
        if (!opened.gpu_renderable()) {
            throw std::runtime_error(
                std::string("FBX window preview is ") +
                apex::app::fbx_preview_document_status_name(opened.status));
        }
        loaded.fbxPreview = std::move(opened);
    } else {
        apex::app::WorkspaceSessionResult opened;
        if (options.model.has_value()) {
            const auto bytes = read_file(*options.model);
            apex::app::WorkspaceSessionFile file;
            file.name = options.model->filename().generic_string();
            file.bytes = bytes;
            apex::app::WorkspaceSessionOpenRequest request;
            request.kind = apex::app::WorkspaceSessionKind::generic;
            request.name = file.name;
            request.modelFiles =
                std::span<const apex::app::WorkspaceSessionFile>(&file, 1U);
            opened = apex::app::WorkspaceSession{}.open(request);
        } else {
            apex::assets::AssetSource source;
            source.addDirectory(*options.workspaceRoot);
            const auto manifestName = options.manifest->generic_string();
            opened = apex::app::WorkspaceSession{}.openAssetSource(
                options.kind,
                options.workspaceRoot->filename().generic_string(),
                manifestName, source);
        }
        if (!opened.ok()) {
            report_workspace_diagnostics(opened);
            throw std::runtime_error("workspace open failed");
        }
        loaded.document = std::move(opened.document);
    }
    auto& document = *loaded.activeDocument();
    if (options.lodIndex.has_value()) {
        const auto& files = document.assembly.workspace.files;
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
            document.assembly.model, &*config.rpm, options.rpm);
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
    if (options.fbxAnimation.has_value()) {
        if (!loaded.fbxPreview.has_value() ||
            *options.fbxAnimation >= loaded.fbxPreview->animations.size())
            throw std::runtime_error(
                "selected FBX animation index is not present");
        const auto& clip =
            loaded.fbxPreview->animations[*options.fbxAnimation];
        const auto clamped_position = static_cast<float>(
            std::clamp(options.animationPosition, 0.0, 1.0));
        const auto applied = apex::domain::apply_animation_preview(
            document.assembly.model, clip.animation, clamped_position);
        for (const auto& item : applied.diagnostics) {
            std::cerr << item.code << " [" << item.source
                      << "]: " << item.message << '\n';
        }
        model_changed = applied.matched_nodes > 0U || model_changed;
        loaded.animationSkinningRequired = applied.skinning_required;
        std::cout << "FBX animation: index=" << *options.fbxAnimation
                  << ", name=";
        write_cli_text(std::cout, clip.name);
        std::cout << ", tracks=" << applied.tracks
                  << ", animated=" << applied.animated_tracks
                  << ", matched-tracks=" << applied.matched_tracks
                  << ", matched-nodes=" << applied.matched_nodes
                  << ", position=" << applied.position << '\n';
    } else if (options.animation.has_value()) {
        const auto bytes = read_file(*options.animation);
        const auto animation = apex::formats::parseKsAnimation(
            bytes, options.animation->generic_string());
        const auto clamped_position = static_cast<float>(
            std::clamp(options.animationPosition, 0.0, 1.0));
        const auto applied = apex::domain::apply_animation_preview(
            document.assembly.model, animation, clamped_position);
        for (const auto& item : applied.diagnostics) {
            std::cerr << item.code << " [" << item.source
                      << "]: " << item.message << '\n';
        }
        model_changed = applied.matched_nodes > 0U || model_changed;
        loaded.animationSkinningRequired = applied.skinning_required;
        std::cout << "animation: tracks=" << applied.tracks
                  << ", animated=" << applied.animated_tracks
                  << ", matched-tracks=" << applied.matched_tracks
                  << ", matched-nodes=" << applied.matched_nodes
                  << ", position=" << applied.position << '\n';
    }
    if (model_changed) {
        document.scene = apex::scene::convertKn5Scene(
            document.assembly.model);
        document.sceneBinding = apex::workspace::bindWorkspaceScene(
            document.scene.snapshot, document.assembly.workspace);
    }
    if (options.trackCameraSet.has_value()) {
        const auto camera_bytes = read_file(*options.trackCameraSet);
        const auto camera_set = apex::domain::parse_track_cameras(
            bytes_as_text(camera_bytes),
            options.trackCameraSet->generic_string());
        for (const auto& item : camera_set.diagnostics) {
            std::cerr << item.code << " [" << item.source << ':'
                      << item.line << "]: " << item.message << '\n';
        }
        const auto camera = std::find_if(
            camera_set.cameras.begin(), camera_set.cameras.end(),
            [&](const apex::domain::CameraData& candidate) {
                return candidate.index == *options.trackCameraIndex;
            });
        if (camera == camera_set.cameras.end())
            throw std::runtime_error(
                "selected track-camera index is not present");

        LoadedWindowWorkspace::TrackCamera preview;
        preview.camera = *camera;
        if (!preview.camera.spline.empty()) {
            const auto spline_request = apex::domain::request_camera_spline(
                preview.camera, options.trackCameraSet->generic_string());
            if (!spline_request.accepted)
                throw std::runtime_error(
                    spline_request.code + ": " + spline_request.message);
            std::string portable_relative = spline_request.relative_path;
            std::replace(portable_relative.begin(), portable_relative.end(),
                         '\\', '/');
            const auto spline_path = options.trackCameraSet->parent_path() /
                                     std::filesystem::path(portable_relative);
            const auto spline_bytes = read_file(spline_path);
            const auto spline = apex::domain::parse_camera_spline(
                bytes_as_text(spline_bytes), spline_path.generic_string());
            for (const auto& item : spline.diagnostics) {
                std::cerr << item.code << " [" << item.source << ':'
                          << item.line << "]: " << item.message << '\n';
            }
            if (spline.points.empty())
                throw std::runtime_error(
                    "selected track-camera spline has no valid points");
            preview.rawSplinePoints = spline.points;
            if (options.trackCameraMode ==
                apex::app::TrackCameraPreviewMode::installed_editor) {
                auto installed =
                    apex::app::buildInstalledEditorTrackCameraSpline(
                        preview.rawSplinePoints);
                if (!installed.ok())
                    throw std::runtime_error(
                        installed.code + ": " + installed.message);
                preview.installedEditorSpline =
                    std::move(*installed.spline);
            } else {
                preview.splinePoints = apex::domain::rotate_camera_spline(
                    preview.rawSplinePoints,
                    preview.camera.spline_rotation);
            }
        } else if (options.trackCameraPlay) {
            throw std::runtime_error(
                "--track-camera-play requires a camera with a resolved spline");
        } else if (options.trackCameraMode ==
                   apex::app::TrackCameraPreviewMode::installed_editor) {
            throw std::runtime_error("installed-editor track-camera mode "
                                     "requires a resolved spline");
        }
        std::cout << "track camera: index=" << preview.camera.index
                  << ", name=";
        write_cli_text(std::cout, preview.camera.name);
        std::cout << ", spline-points=" << preview.rawSplinePoints.size()
                  << ", position=" << options.trackCameraPosition << ", mode="
                  << apex::app::track_camera_preview_mode_name(
                         options.trackCameraMode)
                  << '\n';
        loaded.trackCamera = std::move(preview);
    }
    if (options.aiSpline.has_value()) {
        const auto bytes = read_file(*options.aiSpline);
        auto spline = apex::formats::parseAiSpline(
            bytes, options.aiSpline->generic_string());
        const auto version = spline.version;
        const apex::app::WorkspaceAiSplineOverlaySet* overlay_set = nullptr;
        const bool authoring_requested =
            !options.aiSplineEdits.empty() || options.aiSplineUnlockEdit ||
            options.aiSplineSaveOnExit.has_value();
        bool read_only = version != 7U &&
                         (version != 2U || !authoring_requested);
        if (!read_only) {
            apex::app::WorkspaceAiSplineControllerConfiguration configuration;
            configuration.mode = options.aiSplineMode;
            configuration.interval = options.aiSplineInterval;
            configuration.showLeft = options.aiSplineShowLeft;
            configuration.showRight = options.aiSplineShowRight;
            configuration.selectedIndices = options.aiSplineIndices;
            configuration.showCamber = options.aiSplineShowCamber;
            auto created = apex::app::WorkspaceAiSplineController::create(
                spline, std::move(configuration));
            if (created.ok()) {
                loaded.aiSplineController = std::move(created.controller);
                overlay_set = &loaded.aiSplineController->overlays();
            } else if (!options.aiSplineEdits.empty() ||
                       options.aiSplineUnlockEdit ||
                       options.aiSplineSaveOnExit.has_value()) {
                throw std::runtime_error(created.diagnostic.code + ": " +
                                         created.diagnostic.message);
            } else {
                std::cerr << "AI spline edit disabled: "
                          << created.diagnostic.code << ": "
                          << created.diagnostic.message << '\n';
                read_only = true;
            }
        }
        if (read_only) {
            if (!options.aiSplineEdits.empty() ||
                options.aiSplineUnlockEdit ||
                options.aiSplineSaveOnExit.has_value())
                throw std::runtime_error(
                    "AI spline live editing and save require native version 2 or 7");
            apex::app::WorkspaceAiSplineOverlayRequest overlay_request;
            overlay_request.mode = options.aiSplineMode;
            overlay_request.interval = options.aiSplineInterval;
            overlay_request.show_left = options.aiSplineShowLeft;
            overlay_request.show_right = options.aiSplineShowRight;
            overlay_request.selected_indices = options.aiSplineIndices;
            overlay_request.show_camber = options.aiSplineShowCamber;
            auto built = apex::app::buildWorkspaceAiSplineOverlays(
                spline, overlay_request);
            if (!built.ok())
                throw std::runtime_error(built.diagnostic.code + ": " +
                                         built.diagnostic.message);
            loaded.readOnlyAiSplineOverlays = std::move(built.overlays);
            overlay_set = &*loaded.readOnlyAiSplineOverlays;
        }
        const auto& overlays = *overlay_set;
        std::cout << "AI spline: version=" << version
                  << ", points=" << overlays.primary.source_point_count
                  << ", samples="
                  << overlays.primary.sample_point_count
                  << ", segments="
                  << overlays.primary.vertices.size() / 2U
                  << ", draws=" << overlays.primary.chunks.size()
                  << ", mode="
                  << apex::app::workspace_ai_spline_display_mode_name(
                         overlays.primary.mode)
                  << '\n';
        if (overlays.interval.has_value()) {
            std::cout << "AI spline interval: in="
                      << options.aiSplineInterval->begin
                      << ", out=" << options.aiSplineInterval->end
                      << ", samples="
                      << overlays.interval->sample_point_count
                      << ", segments="
                      << overlays.interval->vertices.size() / 2U
                      << ", draws=" << overlays.interval->chunks.size()
                      << '\n';
        }
        if (overlays.left.has_value()) {
            std::cout << "AI spline left side: samples="
                      << overlays.left->sample_point_count
                      << ", segments="
                      << overlays.left->vertices.size() / 2U
                      << ", draws=" << overlays.left->chunks.size()
                      << '\n';
        }
        if (overlays.right.has_value()) {
            std::cout << "AI spline right side: samples="
                      << overlays.right->sample_point_count
                      << ", segments="
                      << overlays.right->vertices.size() / 2U
                      << ", draws=" << overlays.right->chunks.size()
                      << '\n';
        }
        if (overlays.selection.has_value()) {
            std::cout << "AI spline last selected index: index="
                      << *overlays.selection->last_selected_index
                      << ", lines="
                      << overlays.selection->sample_point_count
                      << ", draws="
                      << overlays.selection->chunks.size()
                      << ", selected="
                      << overlays.selection->selected_point_count
                      << '\n';
        }
        if (overlays.camber.has_value()) {
            std::cout << "AI spline camber: lines="
                      << overlays.camber->sample_point_count
                      << ", draws=" << overlays.camber->chunks.size()
                      << '\n';
        }
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
        request.selected_node =
            options.selectedNode.value_or(apex::scene::invalid_node_id);
        request.isolate_selected = options.isolateSelected;
        request.show_hidden = options.showHidden;
        request.wireframe = options.wireframe;
        const auto selection = apex::app::resolve_workspace_selection(
            document.scene.snapshot, request);
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
            for (const auto &match : selection.matches) {
                std::cout << "node: id=" << match.node
                          << ", depth=" << match.depth << ", kind="
                          << apex::app::workspace_selection_node_kind_name(
                                 match.kind)
                          << ", name=";
                write_cli_text(std::cout, match.name);
                std::cout << '\n';
            }
        }
    }
    if (has_d3d12_native_package(options)) {
        const auto load_native =
            [&](const std::filesystem::path& package_path,
                apex::render::StockKsPerPixelVariant variant,
                std::string key) {
                auto native = apex::render::
                    load_validated_stock_ks_per_pixel_native_program_file(
                        package_path, variant);
                if (!native.ok()) {
                    const std::string code =
                        native.diagnostic.code.empty()
                            ? "stock_ks_per_pixel_package_invalid"
                            : native.diagnostic.code;
                    const std::string message =
                        native.diagnostic.message.empty()
                            ? "the installed package failed validation"
                            : native.diagnostic.message;
                    throw std::runtime_error(code + ": " + message);
                }
                auto owner = std::make_shared<const
                    apex::render::ValidatedStockKsPerPixelNativeProgram>(
                        std::move(*native.program));
                loaded.d3d12NativePrograms.push_back(
                    {std::move(key), std::move(owner)});
            };
        if (options.d3d12KsPerPixelPackage.has_value())
            load_native(*options.d3d12KsPerPixelPackage,
                        apex::render::StockKsPerPixelVariant::base,
                        "ksPerPixel");
        if (options.d3d12KsPerPixelAtPackage.has_value())
            load_native(
                *options.d3d12KsPerPixelAtPackage,
                apex::render::StockKsPerPixelVariant::alpha_to_coverage,
                "ksPerPixelAT");
    }
    if (options.shaders.empty() && !options.builtinVulkanKsPerPixel &&
        !has_d3d12_native_package(options))
        throw std::runtime_error("workspace rendering requires caller-supplied shader modules");

    const auto shader_format =
        [backend](std::span<const std::uint8_t> bytes) {
            const auto detected =
                apex::render::detect_pipeline_shader_format(bytes);
            const bool matches = backend == apex::render::Backend::Vulkan
                                     ? detected == apex::render::PipelineShaderFormat::spirv
                                     : detected == apex::render::PipelineShaderFormat::dxbc ||
                                           detected == apex::render::PipelineShaderFormat::dxil;
            if (!matches)
                throw std::runtime_error(
                    "a caller-supplied shader module does not match the selected backend");
            return detected;
        };
    constexpr std::uint64_t max_shader_bytes =
        apex::render::StaticSceneResourceLimits{}.max_total_shader_bytes;
    std::uint64_t shader_bytes = 0U;
    loaded.shaderSets.reserve(options.shaders.size());
    for (const auto& shader : options.shaders) {
        LoadedWindowWorkspace::ShaderSet set;
        auto vertex_bytes = read_file(shader.vertex);
        auto fragment_bytes = read_file(shader.fragment);
        set.modules.push_back({apex::render::PipelineShaderStage::vertex,
                               shader_format(vertex_bytes),
                               std::move(vertex_bytes)});
        set.modules.push_back({apex::render::PipelineShaderStage::fragment,
                               shader_format(fragment_bytes),
                               std::move(fragment_bytes)});
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
    if (options.authoringOverlayVertex.has_value()) {
        std::vector<apex::render::PipelineShaderModule> modules;
        auto vertex_bytes = read_file(*options.authoringOverlayVertex);
        auto fragment_bytes = read_file(*options.authoringOverlayFragment);
        modules.push_back({apex::render::PipelineShaderStage::vertex,
                           shader_format(vertex_bytes),
                           std::move(vertex_bytes)});
        modules.push_back({apex::render::PipelineShaderStage::fragment,
                           shader_format(fragment_bytes),
                           std::move(fragment_bytes)});
        for (const auto& module : modules) {
            if (module.bytes.size() > apex::render::max_shader_module_bytes)
                throw std::runtime_error(
                    "an authoring-overlay module exceeds the native module budget");
            if (module.bytes.size() > max_shader_bytes ||
                shader_bytes > max_shader_bytes - module.bytes.size())
                throw std::runtime_error(
                    "caller-supplied shader modules exceed the native shader budget");
            shader_bytes += module.bytes.size();
        }
        loaded.authoringOverlayModules = std::move(modules);
    }
    if (options.selectedMeshVertex.has_value()) {
        std::vector<apex::render::PipelineShaderModule> modules;
        auto vertex_bytes = read_file(*options.selectedMeshVertex);
        auto fragment_bytes = read_file(*options.selectedMeshFragment);
        modules.push_back({apex::render::PipelineShaderStage::vertex,
                           shader_format(vertex_bytes),
                           std::move(vertex_bytes)});
        modules.push_back({apex::render::PipelineShaderStage::fragment,
                           shader_format(fragment_bytes),
                           std::move(fragment_bytes)});
        for (const auto& module : modules) {
            if (module.bytes.size() > apex::render::max_shader_module_bytes)
                throw std::runtime_error(
                    "a selected-mesh module exceeds the native module budget");
            if (module.bytes.size() > max_shader_bytes ||
                shader_bytes > max_shader_bytes - module.bytes.size())
                throw std::runtime_error(
                    "caller-supplied shader modules exceed the native shader budget");
            shader_bytes += module.bytes.size();
        }
        loaded.selectedMeshModules = std::move(modules);
    }
    if (has_directional_shadow_modules(options)) {
        const auto load_shadow_module =
            [&](const std::filesystem::path& path,
                apex::render::PipelineShaderStage stage,
                std::string_view role) {
                auto bytes = read_file(path);
                if (bytes.size() > apex::render::max_shader_module_bytes)
                    throw std::runtime_error(
                        "the directional-shadow " + std::string(role) +
                        " module exceeds the native module budget");
                if (bytes.size() > max_shader_bytes ||
                    shader_bytes > max_shader_bytes - bytes.size())
                    throw std::runtime_error(
                        "caller-supplied shader modules exceed the native shader budget");
                shader_bytes += bytes.size();
                return apex::render::PipelineShaderModule{
                    stage, shader_format(bytes), std::move(bytes)};
            };
        const auto build_shadow_pipeline =
            [](std::string name,
               std::vector<apex::render::PipelineShaderModule> modules,
               apex::render::DepthOnlyIndexedPipelineRole role) {
                auto built = apex::app::buildWorkspaceShadowPipeline(
                    std::move(name), std::move(modules), role);
                if (!built.ok())
                    throw std::runtime_error(
                        "directional-shadow pipeline contract: " +
                        built.diagnostic.code + ": " +
                        built.diagnostic.message);
                return std::move(*built.pipeline);
            };
        apex::app::WorkspaceViewportDirectionalShadowOptions shadows;
        if (options.directionalShadowVertex.has_value()) {
            std::vector<apex::render::PipelineShaderModule> modules;
            modules.push_back(load_shadow_module(
                *options.directionalShadowVertex,
                apex::render::PipelineShaderStage::vertex,
                "opaque vertex"));
            shadows.opaque_pipeline = build_shadow_pipeline(
                "workspace-opaque-directional-shadow", std::move(modules),
                apex::render::DepthOnlyIndexedPipelineRole::opaque_static);
        }
        if (options.directionalShadowAlphaVertex.has_value()) {
            std::vector<apex::render::PipelineShaderModule> modules;
            modules.push_back(load_shadow_module(
                *options.directionalShadowAlphaVertex,
                apex::render::PipelineShaderStage::vertex,
                "alpha-tested vertex"));
            modules.push_back(load_shadow_module(
                *options.directionalShadowAlphaFragment,
                apex::render::PipelineShaderStage::fragment,
                "alpha-tested fragment"));
            shadows.alpha_static_pipeline = build_shadow_pipeline(
                "workspace-alpha-tested-directional-shadow",
                std::move(modules),
                apex::render::DepthOnlyIndexedPipelineRole::
                    stock_alpha_tested_static);
        }
        if (options.directionalShadowSkinnedVertex.has_value()) {
            std::vector<apex::render::PipelineShaderModule> modules;
            modules.push_back(load_shadow_module(
                *options.directionalShadowSkinnedVertex,
                apex::render::PipelineShaderStage::vertex,
                "skinned vertex"));
            shadows.skinned_pipeline = build_shadow_pipeline(
                "workspace-skinned-directional-shadow", std::move(modules),
                apex::render::DepthOnlyIndexedPipelineRole::skinned);
        }
        loaded.directionalShadows = std::move(shadows);
    }
    loaded.descriptors.reserve(loaded.shaderSets.size());
    for (std::size_t index = 0; index < loaded.shaderSets.size(); ++index) {
        loaded.shaderSets[index].descriptor = {
            apex::render::StockMaterialShaderKeyKind::shader_family,
            options.shaders[index].family, loaded.shaderSets[index].modules};
        loaded.shaderSets[index].descriptor.directional_shadow_receiver =
            loaded.directionalShadows.has_value();
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
    if (workspace_options.builtinVulkanKsPerPixel &&
        backend != apex::render::Backend::Vulkan)
        throw std::runtime_error(
            "--builtin-vulkan-ks-per-pixel requires the Vulkan backend");
    if (has_d3d12_native_package(workspace_options) &&
        backend != apex::render::Backend::D3D12)
        throw std::runtime_error(
            std::string(workspace_options.d3d12KsPerPixelAtPackage.has_value()
                            ? "--d3d12-ks-per-pixel-at-package"
                            : "--d3d12-ks-per-pixel-package") +
            " requires the D3D12 backend");
    const auto workspace_lighting =
        apex::app::evaluateWorkspaceViewportLighting({
            workspace_options.weather,
            static_cast<float>(workspace_options.sunHeading),
            static_cast<float>(workspace_options.sunHeight)});
    if (!workspace_lighting.ok()) {
        throw std::runtime_error(workspace_lighting.diagnostic.code + ": " +
                                 workspace_lighting.diagnostic.message);
    }
    LoadedWindowWorkspace loaded_workspace;
    load_window_workspace(workspace_options, backend, loaded_workspace);
    const auto* active_document = loaded_workspace.activeDocument();

    std::array<apex::render::DecodedTexturePlan,
               apex::render::portable_cloud_texture_count>
        cloud_texture_plans;
    std::array<bool, apex::render::portable_cloud_texture_count>
        cloud_texture_ready{};
    if (workspace_options.cloudAssets.has_value()) {
        apex::assets::AssetSource cloud_assets;
        cloud_assets.addDirectory(*workspace_options.cloudAssets);
        for (std::size_t index = 0U; index < cloud_texture_plans.size(); ++index) {
            const std::string relative =
                "content/texture/clouds/cloud" + std::to_string(index + 1U) +
                "C.dds";
            const auto resolved = cloud_assets.resolve(relative);
            if (resolved.status != apex::assets::AssetResolveStatus::resolved ||
                resolved.file == nullptr) {
                throw std::runtime_error(
                    "cloud asset did not resolve uniquely: " + relative);
            }
            const auto bytes = cloud_assets.read(*resolved.file);
            const auto planned = apex::render::plan_decoded_dds_texture(
                bytes, resolved.file->relativePath);
            if (!planned.ok()) {
                throw std::runtime_error(
                    planned.diagnostic.code + ": " +
                    planned.diagnostic.message);
            }
            cloud_texture_plans[index] = std::move(planned.plan);
            cloud_texture_ready[index] = true;
        }
    }

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
    if (active_document != nullptr &&
        active_document->scene.preview_bounds.has_value()) {
        const auto& bounds = *active_document->scene.preview_bounds;
        const double distance = std::max(
            static_cast<double>(bounds.radius) * 2.35, 0.2);
        if (!std::isfinite(distance) || distance > 1.0e7) {
            throw std::runtime_error(
                "workspace preview bounds exceed the native camera range");
        }
        camera_controller.target = bounds.center;
        camera_controller.distance = static_cast<float>(distance);
    }

    bool track_camera_active = loaded_workspace.trackCamera.has_value();
    std::optional<std::chrono::steady_clock::time_point>
        track_camera_playback_started;
    auto current_camera = [&](std::uint32_t width,
                              std::uint32_t height)
        -> apex::render::CameraFrameResult {
        const float aspect = static_cast<float>(width) /
                             static_cast<float>(height);
        if (!track_camera_active)
            return camera_controller.frame(aspect, clip_space);

        const auto& preview = *loaded_workspace.trackCamera;
        if (workspace_options.trackCameraMode ==
                apex::app::TrackCameraPreviewMode::installed_editor &&
            !preview.installedEditorSpline.has_value()) {
            return {std::nullopt,
                    "installed_editor_track_camera_spline_missing",
                    "Installed-editor preview requires a camera spline"};
        }
        double spline_position = workspace_options.trackCameraPosition;
        if (workspace_options.trackCameraPlay) {
            const double elapsed = track_camera_playback_started.has_value()
                ? std::chrono::duration<double>(
                      std::chrono::steady_clock::now() -
                      *track_camera_playback_started).count()
                : 0.0;
            apex::app::WorkspaceTrackCameraPlaybackResult playback;
            if (workspace_options.trackCameraMode ==
                apex::app::TrackCameraPreviewMode::installed_editor) {
                playback =
                    apex::app::evaluateInstalledEditorTrackCameraPlayback({
                        static_cast<float>(
                            workspace_options.trackCameraPosition),
                        elapsed,
                        apex::app::installed_editor_track_camera_default_speed,
                        preview.installedEditorSpline->length});
            } else {
                playback = apex::app::evaluateWorkspaceTrackCameraPlayback({
                    workspace_options.trackCameraPosition,
                    elapsed,
                    preview.camera.spline_animation_length});
            }
            if (!playback.ok()) {
                return {std::nullopt, playback.code, playback.message};
            }
            spline_position = playback.position;
        }
        if (workspace_options.trackCameraMode ==
            apex::app::TrackCameraPreviewMode::installed_editor) {
            return apex::app::buildInstalledEditorTrackCameraFrame({
                &preview.camera,
                &*preview.installedEditorSpline,
                static_cast<float>(spline_position),
                apex::app::installed_editor_track_camera_default_lookahead,
                aspect,
                clip_space});
        }
        return apex::app::buildWorkspaceTrackCameraFrame({
            &preview.camera,
            preview.splinePoints,
            spline_position,
            aspect,
            clip_space});
    };
    std::unique_ptr<apex::app::WorkspaceViewport> viewport;
    auto prepare_viewport = [&]() {
        if (active_document == nullptr) {
            viewport.reset();
            return true;
        }
        const auto description = target_result.target->info().description;
        const auto camera = current_camera(description.width,
                                           description.height);
        if (!camera.ok()) {
            std::cerr << "workspace camera: " << camera.code << ": "
                      << camera.message << '\n';
            return false;
        }
        apex::app::WorkspaceViewportPrepareRequest request;
        request.presentation = target_result.target->info().description;
        request.sky_enabled = true;
        if (workspace_options.cloudAssets.has_value()) {
            const auto& preset = workspace_lighting.evaluated.preset;
            apex::app::WorkspaceViewportPortableCloudOptions clouds;
            clouds.settings.width = preset.cloud_width;
            clouds.settings.height = preset.cloud_height;
            clouds.settings.radius = preset.cloud_radius;
            clouds.settings.count = preset.cloud_number;
            clouds.settings.base_speed = preset.cloud_base_speed;
            clouds.cloud_cover = preset.cloud_cover;
            clouds.cloud_cutoff = preset.cloud_cutoff;
            clouds.cloud_color = preset.cloud_color;
            for (std::size_t index = 0U;
                 index < cloud_texture_plans.size(); ++index) {
                if (cloud_texture_ready[index])
                    clouds.textures[index] = &cloud_texture_plans[index];
            }
            request.portable_clouds = clouds;
        }
        request.fxaa_enabled = workspace_options.fxaa;
        if (workspace_options.hdr) {
            apex::render::HdrToneMapParameters tone_map;
            tone_map.exposure = workspace_options.exposure.value_or(0.28F);
            tone_map.bloom.enabled = true;
            request.hdr_tone_map = tone_map;
            request.hdr_exposure_mode = workspace_options.exposure.has_value()
                ? apex::render::HdrExposureMode::manual
                : apex::render::HdrExposureMode::automatic;
        }
        request.shader_modules = loaded_workspace.descriptors;
        if (workspace_options.builtinVulkanKsPerPixel) {
            request.builtin_vulkan_source =
                apex::render::BuiltinVulkanStockSourceSelector::ks_per_pixel;
        }
        if (has_d3d12_native_package(workspace_options)) {
            if (workspace_options.d3d12KsPerPixelPackage.has_value() &&
                workspace_options.d3d12KsPerPixelAtPackage.has_value())
                request.builtin_d3d12_native =
                    apex::render::BuiltinD3D12StockNativeSelector::
                        ks_per_pixel_base_and_alpha_to_coverage;
            else if (workspace_options.d3d12KsPerPixelAtPackage.has_value())
                request.builtin_d3d12_native =
                    apex::render::BuiltinD3D12StockNativeSelector::
                        ks_per_pixel_alpha_to_coverage;
            else
                request.builtin_d3d12_native =
                    apex::render::BuiltinD3D12StockNativeSelector::
                        ks_per_pixel_base;
            request.builtin_d3d12_native_programs =
                loaded_workspace.d3d12NativePrograms;
            if (workspace_options.d3d12KsPerPixelAtPackage.has_value())
                request.color_samples = 4U;
        }
        request.render.camera_position = camera.frame->position;
        request.camera_mesh_filter = true;
        request.webgl_live_transparent_order = true;
        request.render.isolated = loaded_workspace.selection.isolate_selected;
        request.render.isolated_node = loaded_workspace.selection.selected_node;
        request.render.show_hidden = loaded_workspace.selection.show_hidden;
        request.packets.selected_node = loaded_workspace.selection.selected_node;
        request.packets.wireframe = loaded_workspace.selection.wireframe;
        request.wireframe = loaded_workspace.selection.wireframe;
        request.grid_visible = workspace_options.gridVisible;
        request.view_axis_visible = workspace_options.viewAxisVisible;
        if (workspace_options.skeletonVisible)
            request.skeleton_overlay =
                apex::app::WorkspaceViewportSkeletonOverlayOptions{};
        const bool authoring_overlay_requested =
            workspace_options.gridVisible || workspace_options.viewAxisVisible ||
            workspace_options.skeletonVisible ||
            workspace_options.selectedNode.has_value();
        if (loaded_workspace.authoringOverlayModules.has_value() &&
            authoring_overlay_requested) {
            apex::render::PipelineProgram pipeline;
            pipeline.name = "workspace-authoring-overlay";
            pipeline.shaders = *loaded_workspace.authoringOverlayModules;
            pipeline.vertex_layout.stride =
                sizeof(apex::render::OverlayLineVertex);
            pipeline.vertex_layout.attributes = {
                {apex::render::PipelineVertexSemantic::position,
                 apex::render::PipelineVertexAttributeFormat::float32x3,
                 0U, 0U},
                {apex::render::PipelineVertexSemantic::color,
                 apex::render::PipelineVertexAttributeFormat::float32x3,
                 1U, 12U},
            };
            pipeline.targets.colors = {{
                request.hdr_tone_map.has_value()
                    ? apex::render::PipelineRenderTargetFormat::rgba16_float
                    : pipeline_color_format(request.presentation.format),
                request.color_samples}};
            pipeline.targets.has_depth = true;
            pipeline.targets.depth = {
                apex::render::PipelineRenderTargetFormat::depth32_float,
                request.color_samples};
            pipeline.raster.cull = apex::render::PipelineCullMode::none;
            pipeline.raster.fill = apex::render::PipelineFillMode::wireframe;
            pipeline.depth.test_enabled = false;
            pipeline.depth.write_enabled = false;
            pipeline.transform_contract =
                apex::render::PipelineTransformContract::draw_matrices;
            request.authoring_overlay_pipeline = std::move(pipeline);
        }
        if (const auto* ai_overlays = loaded_workspace.aiSplineOverlays();
            ai_overlays != nullptr) {
            const auto& ai = *ai_overlays;
            if (loaded_workspace.aiSplineController != nullptr)
                request.ai_spline_generation =
                    loaded_workspace.aiSplineController->generation();
            apex::render::PipelineProgram pipeline;
            pipeline.name = "workspace-ai-spline-raw";
            pipeline.shaders = *loaded_workspace.authoringOverlayModules;
            pipeline.vertex_layout.stride =
                sizeof(apex::render::OverlayLineVertex);
            pipeline.vertex_layout.attributes = {
                {apex::render::PipelineVertexSemantic::position,
                 apex::render::PipelineVertexAttributeFormat::float32x3, 0U,
                 0U},
                {apex::render::PipelineVertexSemantic::color,
                 apex::render::PipelineVertexAttributeFormat::float32x3, 1U,
                 12U},
            };
            pipeline.targets.colors = {{
                request.hdr_tone_map.has_value()
                    ? apex::render::PipelineRenderTargetFormat::rgba16_float
                    : pipeline_color_format(request.presentation.format),
                request.color_samples}};
            pipeline.targets.has_depth = true;
            pipeline.targets.depth = {
                apex::render::PipelineRenderTargetFormat::depth32_float,
                request.color_samples};
            pipeline.raster.cull = apex::render::PipelineCullMode::none;
            pipeline.raster.fill = apex::render::PipelineFillMode::wireframe;
            pipeline.depth.test_enabled = true;
            pipeline.depth.write_enabled = true;
            pipeline.depth.compare =
                apex::render::PipelineCompareOperation::less_or_equal;
            pipeline.transform_contract =
                apex::render::PipelineTransformContract::draw_matrices;
            request.ai_spline_geometry = &ai.primary;
            if (ai.interval.has_value()) {
                auto interval_pipeline = pipeline;
                interval_pipeline.name = "workspace-ai-spline-interval";
                interval_pipeline.depth.test_enabled = false;
                interval_pipeline.depth.write_enabled = false;
                request.ai_spline_interval_geometry = &*ai.interval;
                request.ai_spline_interval_pipeline =
                    std::move(interval_pipeline);
            }
            if (loaded_workspace.aiSplineController != nullptr ||
                ai.left.has_value()) {
                auto side_pipeline = pipeline;
                side_pipeline.name = "workspace-ai-spline-left";
                if (ai.left.has_value())
                    request.ai_spline_left_geometry = &*ai.left;
                request.ai_spline_left_pipeline = std::move(side_pipeline);
            }
            if (loaded_workspace.aiSplineController != nullptr ||
                ai.right.has_value()) {
                auto side_pipeline = pipeline;
                side_pipeline.name = "workspace-ai-spline-right";
                if (ai.right.has_value())
                    request.ai_spline_right_geometry = &*ai.right;
                request.ai_spline_right_pipeline = std::move(side_pipeline);
            }
            if (loaded_workspace.aiSplineController != nullptr ||
                ai.selection.has_value()) {
                auto selection_pipeline = pipeline;
                selection_pipeline.name = "workspace-ai-spline-selection";
                if (ai.selection.has_value())
                    request.ai_spline_selection_geometry = &*ai.selection;
                request.ai_spline_selection_pipeline =
                    std::move(selection_pipeline);
            }
            if (loaded_workspace.aiSplineController != nullptr) {
                auto temporary_pipeline = pipeline;
                temporary_pipeline.name =
                    "workspace-ai-spline-temporary-interpolation";
                if (ai.temporaryInterpolation.has_value())
                    request.ai_spline_temporary_interpolation_geometry =
                        &*ai.temporaryInterpolation;
                request.ai_spline_temporary_interpolation_pipeline =
                    temporary_pipeline;
                temporary_pipeline.name =
                    "workspace-ai-spline-temporary-markers";
                if (ai.temporaryMarkers.has_value())
                    request.ai_spline_temporary_marker_geometry =
                        &*ai.temporaryMarkers;
                request.ai_spline_temporary_marker_pipeline =
                    std::move(temporary_pipeline);
            }
            if (ai.camber.has_value()) {
                auto camber_pipeline = pipeline;
                camber_pipeline.name = "workspace-ai-spline-camber";
                request.ai_spline_camber_geometry = &*ai.camber;
                request.ai_spline_camber_pipeline = std::move(camber_pipeline);
            }
            request.ai_spline_pipeline = std::move(pipeline);
        }
        if (loaded_workspace.selectedMeshModules.has_value()) {
            apex::render::PipelineProgram pipeline;
            pipeline.name = "workspace-selected-mesh";
            pipeline.shaders = *loaded_workspace.selectedMeshModules;
            pipeline.vertex_layout.stride = 11U * sizeof(float);
            pipeline.vertex_layout.attributes = {
                {apex::render::PipelineVertexSemantic::position,
                 apex::render::PipelineVertexAttributeFormat::float32x3, 0U,
                 0U},
            };
            pipeline.targets.colors = {{
                request.hdr_tone_map.has_value()
                    ? apex::render::PipelineRenderTargetFormat::rgba16_float
                    : pipeline_color_format(request.presentation.format),
                request.color_samples}};
            pipeline.targets.has_depth = true;
            pipeline.targets.depth = {
                apex::render::PipelineRenderTargetFormat::depth32_float,
                request.color_samples};
            pipeline.raster.fill = apex::render::PipelineFillMode::solid;
            pipeline.raster.cull = apex::render::PipelineCullMode::front;
            pipeline.depth.test_enabled = false;
            pipeline.depth.write_enabled = false;
            pipeline.transform_contract =
                apex::render::PipelineTransformContract::selected_mesh;
            request.selected_mesh_pipeline = std::move(pipeline);
        }
        if (loaded_workspace.directionalShadows.has_value()) {
            request.directional_shadow_receiver =
                !loaded_workspace.descriptors.empty();
            request.directional_shadows = loaded_workspace.directionalShadows;
        } else if (workspace_options.builtinVulkanKsPerPixel ||
                   has_d3d12_native_package(workspace_options)) {
            request.directional_shadows =
                apex::app::WorkspaceViewportDirectionalShadowOptions{};
        }
        if (request.directional_shadows.has_value()) {
            request.directional_shadows->maps.lighting.scene_radius = std::max(
                1.0F, active_document->scene.snapshot.bounds_radius);
            request.directional_shadows->maps.lighting.sun_direction =
                workspace_lighting.evaluated.sun_direction;
            if ((workspace_options.builtinVulkanKsPerPixel &&
                 loaded_workspace.descriptors.empty()) ||
                has_d3d12_native_package(workspace_options)) {
                request.directional_shadows->maps.lighting.splits =
                    apex::render::ks_editor_shadow_splits;
                request.directional_shadows->maps.lighting.far_plane =
                    apex::render::ks_editor_shadow_range;
            }
        }
        if (active_document->assembly.workspace.kind == "carLods") {
            if (!active_document->scene.preview_bounds.has_value()) {
                throw std::runtime_error(
                    "carLods workspace has no preview-visible geometry bounds");
            }
            request.workspace.lod_bounds_center =
                active_document->scene.preview_bounds->center;
            request.workspace.lod_index = workspace_options.lodIndex;
            request.workspace.lod_track_camera = track_camera_active;
        }
        auto prepared = loaded_workspace.fbxPreview.has_value()
                            ? apex::app::prepareWorkspaceViewport(
                                  *device_result.device,
                                  *loaded_workspace.fbxPreview, request)
                            : apex::app::prepareWorkspaceViewport(
                                  *device_result.device, *active_document,
                                  request);
        if (!prepared.ok()) {
            std::cerr << "workspace render: " << prepared.diagnostic.code << ": "
                      << prepared.diagnostic.message << '\n';
            return false;
        }
        viewport = std::move(prepared.viewport);
        return true;
    };
    if (!prepare_viewport()) return 1;
    if (!workspace_options.aiSplineEdits.empty()) {
        if (loaded_workspace.aiSplineController == nullptr ||
            viewport == nullptr) {
            std::cerr << "AI spline live edit: unavailable\n";
            return 1;
        }
        const auto edited =
            loaded_workspace.aiSplineController->setPointPositions(
                *device_result.device, *viewport,
                workspace_options.aiSplineEdits,
                loaded_workspace.aiSplineController->revision());
        if (!edited.ok()) {
            std::cerr << "AI spline live edit: " << edited.diagnostic.code
                      << ": " << edited.diagnostic.message << '\n';
            return 1;
        }
        std::cout << "AI spline live edit: status="
                  << apex::app::workspace_ai_spline_controller_status_name(
                         edited.status)
                  << ", revision=" << edited.revision
                  << ", points=" << edited.applied
                  << ", passes=" << edited.replacedPassCount << '\n';
    }
    if (workspace_options.trackCameraPlay)
        track_camera_playback_started = std::chrono::steady_clock::now();

    std::array<apex::platform::WindowEvent,
               apex::platform::max_window_poll_count>
        events{};
    apex::app::WorkspaceAiSplineManualInputState ai_spline_manual_input;
    std::uint64_t frames = 0U;
    bool reported_shadow_diagnostic = false;
    bool reported_ai_spline_manual_diagnostic = false;
    bool window_has_keyboard_focus = true;
    apex::app::PresentationRecreationController recreation;
    auto pick_ai_spline_point =
        [&](const apex::platform::WindowEvent& event) {
            if (loaded_workspace.aiSplineController == nullptr ||
                active_document == nullptr || viewport == nullptr)
                return;
            const std::uint32_t width = window_result.window->width();
            const std::uint32_t height = window_result.window->height();
            if (width == 0U || height == 0U || !std::isfinite(event.x) ||
                !std::isfinite(event.y))
                return;
            const auto camera = current_camera(width, height);
            if (!camera.ok()) {
                std::cerr << "AI spline pick camera: " << camera.code << ": "
                          << camera.message << '\n';
                return;
            }
            // WinForms supplied integer MouseEventArgs coordinates to the
            // installed RayPicker. Truncate SDL's logical float coordinates.
            const float pixel_x = std::trunc(event.x);
            const float pixel_y = std::trunc(event.y);
            const auto ray = apex::render::build_screen_ray(
                *camera.frame, pixel_x, pixel_y, width, height);
            if (!ray.ok()) {
                std::cerr << "AI spline pick ray: " << ray.diagnostic.code
                          << ": " << ray.diagnostic.message << '\n';
                return;
            }
            const auto mesh_hit = apex::render::pick_kn5_scene(
                *ray.ray, active_document->assembly.model.root);
            if (mesh_hit.status == apex::render::PickStatus::invalid_request) {
                std::cerr << "AI spline mesh pick: "
                          << mesh_hit.diagnostic.code << ": "
                          << mesh_hit.diagnostic.message << '\n';
                return;
            }
            if (!mesh_hit.ok()) return;
            const auto closest =
                apex::app::resolveWorkspaceAiSplineClosestPoint(
                    loaded_workspace.aiSplineController->current(),
                    mesh_hit.hit->callback_position);
            if (!closest.ok()) {
                std::cerr << "AI spline point pick: "
                          << closest.diagnostic.code << ": "
                          << closest.diagnostic.message << '\n';
                return;
            }
            if (loaded_workspace.aiSplineController->current().points.empty())
                return;
            apex::app::WorkspaceAiSplinePointSelectionRequest selection;
            selection.pointIndex = closest.point_index;
            selection.pickedPosition = mesh_hit.hit->callback_position;
            selection.controlPressed =
                apex::platform::window_modifier_active(
                    event.modifiers,
                    apex::platform::WindowModifier::control);
            selection.shiftPressed =
                apex::platform::window_modifier_active(
                    event.modifiers,
                    apex::platform::WindowModifier::shift);
            selection.expected =
                loaded_workspace.aiSplineController->inputSnapshot();
            const auto selected =
                loaded_workspace.aiSplineController->selectPoint(
                    *device_result.device, *viewport, selection);
            if (!selected.ok()) {
                std::cerr << "AI spline point selection: "
                          << selected.diagnostic.code << ": "
                          << selected.diagnostic.message << '\n';
            }
        };
    const auto change_ai_spline_editing =
        [&](apex::platform::WindowKey key) {
            if (loaded_workspace.aiSplineController == nullptr ||
                viewport == nullptr)
                return;
            const auto expected =
                loaded_workspace.aiSplineController->inputSnapshot();
            apex::app::WorkspaceAiSplineControllerResult changed;
            if (key == apex::platform::WindowKey::enter) {
                changed = loaded_workspace.aiSplineController->editing()
                              ? loaded_workspace.aiSplineController
                                    ->finishEditing(*device_result.device,
                                                    *viewport, expected)
                              : loaded_workspace.aiSplineController
                                    ->startEditing(*device_result.device,
                                                   *viewport, expected);
            } else if (key == apex::platform::WindowKey::escape &&
                       loaded_workspace.aiSplineController->editing()) {
                changed = loaded_workspace.aiSplineController->cancelEditing(
                    *device_result.device, *viewport, expected);
            } else {
                return;
            }
            if (!changed.ok()) {
                std::cerr << "AI spline edit mode: "
                          << changed.diagnostic.code << ": "
                          << changed.diagnostic.message << '\n';
            }
        };
    const auto change_ai_spline_side_visibility =
        [&](apex::app::WorkspaceAiSplineSideVisibilityCommand command) {
            if (loaded_workspace.aiSplineController == nullptr ||
                viewport == nullptr)
                return;
            const auto& configuration =
                loaded_workspace.aiSplineController->configuration();
            bool show_left = configuration.showLeft;
            bool show_right = configuration.showRight;
            switch (command) {
            case apex::app::WorkspaceAiSplineSideVisibilityCommand::
                toggle_left:
                show_left = !show_left;
                break;
            case apex::app::WorkspaceAiSplineSideVisibilityCommand::
                toggle_right:
                show_right = !show_right;
                break;
            }
            const auto changed =
                loaded_workspace.aiSplineController->setSideVisibility(
                    *device_result.device, *viewport, show_left, show_right,
                    loaded_workspace.aiSplineController->inputSnapshot());
            if (!changed.ok()) {
                std::cerr << "AI spline side visibility: "
                          << changed.diagnostic.code << ": "
                          << changed.diagnostic.message << '\n';
            }
        };
    while (!window_result.window->close_requested() &&
           (frame_limit == 0U || frames < frame_limit)) {
        bool resized = false;
        bool camera_mode_changed = false;
        const auto event_count = window_result.window->poll_events(events);
        for (std::size_t index = 0U; index < event_count; ++index) {
            if (events[index].type == apex::platform::WindowEventType::pixel_size_changed)
                resized = true;
            switch (events[index].type) {
            case apex::platform::WindowEventType::key_down:
                if (!window_has_keyboard_focus) break;
                if (const auto command =
                        apex::app::workspaceAiSplineSideVisibilityCommand(
                            events[index]);
                    command.has_value())
                    change_ai_spline_side_visibility(*command);
                if (!events[index].repeat &&
                    (events[index].semantic_key ==
                         apex::platform::WindowKey::enter ||
                     events[index].semantic_key ==
                         apex::platform::WindowKey::escape))
                    change_ai_spline_editing(events[index].semantic_key);
                if (workspace_options.aiSplineUnlockEdit) {
                    if (const auto key =
                            workspace_ai_spline_manual_key_for_window_key(
                                events[index].semantic_key);
                        key.has_value())
                        (void)ai_spline_manual_input.setPressed(*key, true);
                }
                if (const auto movement = workspace_camera_move_for_key(
                        events[index].semantic_key); movement.has_value()) {
                    if (track_camera_active) {
                        track_camera_active = false;
                        camera_mode_changed = true;
                    }
                    (void)camera_controller.move(*movement);
                }
                break;
            case apex::platform::WindowEventType::key_up:
                if (const auto key =
                        workspace_ai_spline_manual_key_for_window_key(
                            events[index].semantic_key);
                    key.has_value())
                    (void)ai_spline_manual_input.setPressed(*key, false);
                break;
            case apex::platform::WindowEventType::focus_lost:
                window_has_keyboard_focus = false;
                ai_spline_manual_input.setFocused(false);
                break;
            case apex::platform::WindowEventType::focus_gained:
                window_has_keyboard_focus = true;
                ai_spline_manual_input.setFocused(true);
                break;
            case apex::platform::WindowEventType::mouse_button_down:
                if (events[index].button ==
                    apex::platform::window_mouse_button_code(
                        apex::platform::WindowMouseButton::left)) {
                    if (track_camera_active) {
                        track_camera_active = false;
                        camera_mode_changed = true;
                    }
                    (void)camera_controller.apply({
                        apex::app::WorkspaceViewportCameraGesture::begin_orbit,
                        0.0F, 0.0F});
                } else if (events[index].button ==
                           apex::platform::window_mouse_button_code(
                               apex::platform::WindowMouseButton::middle)) {
                    if (track_camera_active) {
                        track_camera_active = false;
                        camera_mode_changed = true;
                    }
                    (void)camera_controller.apply({
                        apex::app::WorkspaceViewportCameraGesture::begin_pan,
                        0.0F, 0.0F});
                }
                break;
            case apex::platform::WindowEventType::mouse_button_up:
                if (events[index].button ==
                        apex::platform::window_mouse_button_code(
                            apex::platform::WindowMouseButton::right)) {
                    pick_ai_spline_point(events[index]);
                } else if (events[index].button ==
                               apex::platform::window_mouse_button_code(
                                   apex::platform::WindowMouseButton::left) ||
                           events[index].button ==
                               apex::platform::window_mouse_button_code(
                                   apex::platform::WindowMouseButton::middle)) {
                    (void)camera_controller.apply({
                        apex::app::WorkspaceViewportCameraGesture::end_drag,
                        0.0F, 0.0F});
                }
                break;
            case apex::platform::WindowEventType::mouse_motion:
                (void)camera_controller.apply({
                    apex::app::WorkspaceViewportCameraGesture::drag,
                    events[index].x_relative, events[index].y_relative});
                break;
            case apex::platform::WindowEventType::mouse_wheel:
                if (track_camera_active) {
                    track_camera_active = false;
                    camera_mode_changed = true;
                }
                (void)camera_controller.apply({
                    apex::app::WorkspaceViewportCameraGesture::wheel,
                    events[index].x_relative, events[index].y_relative});
                break;
            default:
                break;
            }
        }
        if (window_result.window->close_requested()) break;
        const auto pixel_width = window_result.window->pixel_width();
        const auto pixel_height = window_result.window->pixel_height();
        if (apex::app::presentation_surface_is_zero_sized(pixel_width, pixel_height)) {
            ai_spline_manual_input.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        bool viewport_prepared = false;
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
            viewport_prepared = true;
        }
        if (camera_mode_changed && !viewport_prepared &&
            !prepare_viewport()) return 1;
        apex::render::PresentationFrameResult blank_frame;
        apex::app::WorkspaceViewportFrameStatus viewport_status =
            apex::app::WorkspaceViewportFrameStatus::ready;
        apex::render::Diagnostic viewport_diagnostic;
        if (viewport != nullptr) {
            const auto camera = current_camera(pixel_width, pixel_height);
            if (!camera.ok()) {
                std::cerr << "workspace camera: " << camera.code << ": "
                          << camera.message << '\n';
                return 1;
            }
            apex::app::WorkspaceViewportFrameRequest frame_request;
            frame_request.camera = *camera.frame;
            frame_request.frame_constants = workspace_lighting.frame_constants;
            if (viewport->preparation().resources
                    ->requires_stock_vulkan_source_frame()) {
                const auto native_lighting =
                    apex::app::buildWorkspaceViewportStockVulkanSourceLighting(
                        workspace_lighting.evaluated, pixel_width,
                        pixel_height);
                const auto source_frame = native_lighting.has_value()
                    ? apex::app::buildWorkspaceViewportStockVulkanSourceFrame(
                          *camera.frame, *native_lighting)
                    : std::nullopt;
                if (!source_frame.has_value()) {
                    std::cerr << "workspace source frame: invalid native "
                                 "camera or lighting state\n";
                    return 1;
                }
                frame_request.stock_vulkan_source_frame = *source_frame;
            }
            if (viewport->preparation().resources
                    ->requires_stock_d3d12_native_frame()) {
                const auto native_lighting =
                    apex::app::buildWorkspaceViewportStockVulkanSourceLighting(
                        workspace_lighting.evaluated, pixel_width,
                        pixel_height);
                const auto native_frame = native_lighting.has_value()
                    ? apex::app::buildWorkspaceViewportStockD3D12NativeFrame(
                          *camera.frame, *native_lighting)
                    : std::nullopt;
                if (!native_frame.has_value()) {
                    std::cerr << "workspace D3D12 native frame: invalid "
                                 "camera or lighting state\n";
                    return 1;
                }
                frame_request.stock_d3d12_native_frame = *native_frame;
            }
            frame_request.apply_skinning =
                loaded_workspace.animationSkinningRequired;
            if (loaded_workspace.authoringOverlayModules.has_value() &&
                loaded_workspace.selection.selected_node !=
                    apex::scene::invalid_node_id) {
                frame_request.selection_axis_world =
                    active_document->scene.snapshot.nodes[
                        static_cast<std::size_t>(
                            loaded_workspace.selection.selected_node)]
                        .transform;
            }
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
        if (viewport != nullptr && !reported_shadow_diagnostic &&
            !viewport_diagnostic.code.empty()) {
            std::cerr << "workspace shadow: " << viewport_diagnostic.code
                      << ": " << viewport_diagnostic.message << '\n';
            reported_shadow_diagnostic = true;
        }
        if (workspace_options.aiSplineUnlockEdit && viewport != nullptr) {
            if (loaded_workspace.aiSplineController == nullptr) {
                std::cerr << "AI spline manual edit: unavailable\n";
                return 1;
            }
            const auto moved = loaded_workspace.aiSplineController
                                   ->moveSelectedByManualInput(
                                       *device_result.device, *viewport,
                                       ai_spline_manual_input.movement(),
                                       loaded_workspace.aiSplineController
                                           ->inputSnapshot());
            if (!moved.ok()) {
                if (!reported_ai_spline_manual_diagnostic) {
                    std::cerr << "AI spline manual edit: "
                              << moved.diagnostic.code << ": "
                              << moved.diagnostic.message << '\n';
                    reported_ai_spline_manual_diagnostic = true;
                }
                switch (moved.status) {
                case apex::app::WorkspaceAiSplineControllerStatus::
                    allocation_failed:
                case apex::app::WorkspaceAiSplineControllerStatus::
                    viewport_failed:
                    break;
                case apex::app::WorkspaceAiSplineControllerStatus::
                    stale_revision:
                    if (!prepare_viewport()) return 1;
                    break;
                case apex::app::WorkspaceAiSplineControllerStatus::
                    stale_input:
                    break;
                case apex::app::WorkspaceAiSplineControllerStatus::
                    stale_state:
                    if (!prepare_viewport()) return 1;
                    break;
                default:
                    return 1;
                }
            } else {
                reported_ai_spline_manual_diagnostic = false;
            }
        }
        recreation.record_successful_frame();
        ++frames;
    }
    device_result.device->wait_idle();
    if (workspace_options.aiSplineSaveOnExit.has_value()) {
        if (loaded_workspace.aiSplineController == nullptr) {
            std::cerr << "AI spline save: unavailable\n";
            return 1;
        }
        const auto saved =
            loaded_workspace.aiSplineController->buildSaveBytes(
                loaded_workspace.aiSplineController->revision());
        if (!saved.ok()) {
            std::cerr << "AI spline save: " << saved.diagnostic.code << ": "
                      << saved.diagnostic.message << '\n';
            return 1;
        }
        apex::platform::writeFileAtomicReplace(
            *workspace_options.aiSplineSaveOnExit, saved.bytes);
        std::cout << "AI spline saved: revision=" << saved.revision
                  << ", grid=rebuilt, output=";
        write_cli_text(std::cout,
                       workspace_options.aiSplineSaveOnExit->string());
        std::cout << '\n';
    }
    std::cout << apex::render::backend_name(backend) << ": "
              << device_result.device->info().name << ", window="
              << window_result.window->pixel_width() << 'x'
              << window_result.window->pixel_height() << ", frames=" << frames;
    if (active_document != nullptr) {
        if (loaded_workspace.fbxPreview.has_value())
            std::cout << ", workspace=fbx";
        else
            std::cout << ", workspace=" << workspace_kind_name(
                workspace_options.kind);
    }
    std::cout << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "--edit-ai-spline") {
            if (argc < 6) {
                usage(std::cerr);
                return 2;
            }
            return edit_ai_spline(argc, argv);
        }
        if (argc >= 2 && std::string_view(argv[1]) == "--save-ai-spline") {
            if (argc != 4) {
                usage(std::cerr);
                return 2;
            }
            return save_ai_spline(argc, argv);
        }
        if (argc >= 2 &&
            std::string_view(argv[1]) == "--convert-ai-spline-v2") {
            if (argc != 4) {
                usage(std::cerr);
                return 2;
            }
            return convert_ai_spline_v2(argc, argv);
        }
        if (argc >= 2 && std::string_view(argv[1]) == "--invert-ai-spline") {
            if (argc < 6) {
                usage(std::cerr);
                return 2;
            }
            return invert_ai_spline(argc, argv);
        }
        if (argc >= 2 && std::string_view(argv[1]) == "--set-ai-spline-point") {
            if (argc < 10) {
                usage(std::cerr);
                return 2;
            }
            return set_ai_spline_point(argc, argv);
        }
        if (argc >= 2 &&
            std::string_view(argv[1]) == "--set-ai-spline-points") {
            if (argc < 9) {
                usage(std::cerr);
                return 2;
            }
            return set_ai_spline_points(argc, argv);
        }
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
        if (argc >= 2 && std::string_view(argv[1]) == "--inspect-fbx") {
            if (argc == 3)
                return inspect_fbx(argv[2], std::nullopt);
            if (argc == 5 &&
                std::string_view(argv[3]) == "--fbx-assets")
                return inspect_fbx(argv[2], std::filesystem::path(argv[4]));
            usage(std::cerr);
            return 2;
        }
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
