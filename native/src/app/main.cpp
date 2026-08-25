#include "apex/app/authoring_service.hpp"
#include "apex/core/parse_limits.hpp"
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
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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

int run_window(int argc, char** argv) {
    if (argc < 3) throw std::runtime_error("invalid --window arguments");
    const auto backend = parse_backend(argv[2]);
    bool validation = false;
    std::uint64_t frame_limit = 0U;
    for (int index = 3; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--validation") {
            if (validation) throw std::runtime_error("duplicate --validation option");
            validation = true;
        } else if (option == "--frames") {
            if (index + 1 >= argc) throw std::runtime_error("--frames requires a count");
            frame_limit = parse_frame_count(argv[++index]);
        } else {
            throw std::runtime_error("unknown window option");
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

    std::array<apex::platform::WindowEvent, 64U> events{};
    std::uint64_t frames = 0U;
    std::uint32_t recreate_attempts = 0U;
    while (!window_result.window->close_requested() &&
           (frame_limit == 0U || frames < frame_limit)) {
        bool resized = false;
        const auto event_count = window_result.window->poll_events(events);
        for (std::size_t index = 0U; index < event_count; ++index) {
            if (events[index].type == apex::platform::WindowEventType::pixel_size_changed)
                resized = true;
        }
        if (window_result.window->close_requested()) break;
        if (window_result.window->pixel_width() == 0U ||
            window_result.window->pixel_height() == 0U) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        if (resized) {
            target_result.target.reset();
            target_result = create_target();
            if (!target_result.ok()) {
                std::cerr << "presentation resize: " << target_result.diagnostic.code << ": "
                          << target_result.diagnostic.message << '\n';
                return 1;
            }
        }
        const auto frame = device_result.device->clear_and_present(
            *target_result.target, {0.035F, 0.055F, 0.085F, 1.0F});
        if (!frame.ok() && frame.diagnostic.code == "vulkan_presentation_target_out_of_date" &&
            recreate_attempts < 8U) {
            ++recreate_attempts;
            target_result.target.reset();
            target_result = create_target();
            if (target_result.ok()) continue;
        }
        if (!frame.ok()) {
            std::cerr << "presentation frame: " << frame.diagnostic.code << ": "
                      << frame.diagnostic.message << '\n';
            return 1;
        }
        recreate_attempts = 0U;
        ++frames;
    }
    device_result.device->wait_idle();
    std::cout << apex::render::backend_name(backend) << ": "
              << device_result.device->info().name << ", window="
              << window_result.window->pixel_width() << 'x'
              << window_result.window->pixel_height() << ", frames=" << frames << '\n';
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
    } catch (const std::exception& error) {
        std::cerr << "apex-native: " << error.what() << '\n';
        return 1;
    }
}
