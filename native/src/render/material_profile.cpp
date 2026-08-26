#include "apex/render/material_profile.hpp"
#include "dxbc_reader.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace apex::render {
namespace {

// This catalog is deliberately kept as data instead of coupled to either
// Vulkan or Direct3D. It is the installed SDK shader-package contract.
constexpr std::array kStockProfiles = {
    StockShaderProfile{"GL", false, "mesh"},
    StockShaderProfile{"GL2D", false, "2d"},
    StockShaderProfile{"GLTextured", false, "mesh"},
    StockShaderProfile{"ksBrakeDisc", false, "mesh"},
    StockShaderProfile{"ksBrokenGlass", false, "mesh"},
    StockShaderProfile{"ksCameraDirt", false, "mesh"},
    StockShaderProfile{"ksCarPaintSimple", false, "mesh"},
    StockShaderProfile{"ksCircularRPM", false, "mesh"},
    StockShaderProfile{"ksClouds", false, "mesh"},
    StockShaderProfile{"ksColourShader", false, "mesh"},
    StockShaderProfile{"ksFXAA_0", false, "mesh"},
    StockShaderProfile{"ksFXAA_1", false, "mesh"},
    StockShaderProfile{"ksFXAA_2", false, "mesh"},
    StockShaderProfile{"ksFXAA_3", false, "mesh"},
    StockShaderProfile{"ksFXAA_4", false, "mesh"},
    StockShaderProfile{"ksFXAA_5", false, "mesh"},
    StockShaderProfile{"ksFakeCarShadows", false, "mesh"},
    StockShaderProfile{"ksFakeCarShadowsGen", false, "mesh"},
    StockShaderProfile{"ksFlags", false, "mesh"},
    StockShaderProfile{"ksFont", false, "mesh"},
    StockShaderProfile{"ksGrass", true, "mesh"},
    StockShaderProfile{"ksHighPass", false, "mesh"},
    StockShaderProfile{"ksIdealLine", false, "mesh"},
    StockShaderProfile{"ksMSDepthResolve", false, "mesh"},
    StockShaderProfile{"ksMegaShader", false, "mesh"},
    StockShaderProfile{"ksMultilayer", false, "mesh"},
    StockShaderProfile{"ksMultilayer_fresnel_nm", false, "mesh"},
    StockShaderProfile{"ksMultilayer_objsp", false, "mesh"},
    StockShaderProfile{"ksOrenNayar", false, "mesh"},
    StockShaderProfile{"ksParticle", false, "particle"},
    StockShaderProfile{"ksPerPixel", false, "mesh"},
    StockShaderProfile{"ksPerPixelAT", true, "mesh"},
    StockShaderProfile{"ksPerPixelAT_NM", true, "mesh"},
    StockShaderProfile{"ksPerPixelAT_NS", true, "mesh"},
    StockShaderProfile{"ksPerPixelAlpha", false, "mesh"},
    StockShaderProfile{"ksPerPixelMultiMap", false, "mesh"},
    StockShaderProfile{"ksPerPixelMultiMapSimpleRefl", false, "mesh"},
    StockShaderProfile{"ksPerPixelMultiMap_AT", true, "mesh"},
    StockShaderProfile{"ksPerPixelMultiMap_AT_NMDetail", true, "mesh"},
    StockShaderProfile{"ksPerPixelMultiMap_NMDetail", false, "mesh"},
    StockShaderProfile{"ksPerPixelMultiMap_damage", false, "mesh"},
    StockShaderProfile{"ksPerPixelMultiMap_damage_dirt", false, "mesh"},
    StockShaderProfile{"ksPerPixelMultiMap_emissive", false, "mesh"},
    StockShaderProfile{"ksPerPixelNM", false, "mesh"},
    StockShaderProfile{"ksPerPixelNM_UV2", false, "mesh"},
    StockShaderProfile{"ksPerPixelNM_UVMult", false, "mesh"},
    StockShaderProfile{"ksPerPixelReflection", false, "mesh"},
    StockShaderProfile{"ksPerPixelSimpleRefl", false, "mesh"},
    StockShaderProfile{"ksPerPixel_dual_layer", false, "mesh"},
    StockShaderProfile{"ksPerPixel_nosdw", false, "mesh"},
    StockShaderProfile{"ksPostAdaptLum", false, "mesh"},
    StockShaderProfile{"ksPostBW", false, "mesh"},
    StockShaderProfile{"ksPostBlur", false, "mesh"},
    StockShaderProfile{"ksPostBlurH", false, "mesh"},
    StockShaderProfile{"ksPostBlurRadial", false, "mesh"},
    StockShaderProfile{"ksPostBlurRadialMS", false, "mesh"},
    StockShaderProfile{"ksPostBlurV", false, "mesh"},
    StockShaderProfile{"ksPostBlur_MS", false, "mesh"},
    StockShaderProfile{"ksPostCopy", false, "mesh"},
    StockShaderProfile{"ksPostCopyLuma", false, "mesh"},
    StockShaderProfile{"ksPostFOG", false, "mesh"},
    StockShaderProfile{"ksPostFOG_MS", false, "mesh"},
    StockShaderProfile{"ksPostToneMap", false, "mesh"},
    StockShaderProfile{"ksSelectedMesh", false, "mesh"},
    StockShaderProfile{"ksShadowGen", false, "mesh"},
    StockShaderProfile{"ksShadowGenAT", false, "mesh"},
    StockShaderProfile{"ksShadowGenSKIN", false, "skinned"},
    StockShaderProfile{"ksShadowGen_debug", false, "mesh"},
    StockShaderProfile{"ksSimpleShader", false, "mesh"},
    StockShaderProfile{"ksSkidMark", false, "mesh"},
    StockShaderProfile{"ksSkinnedMesh", false, "skinned"},
    StockShaderProfile{"ksSkinnedMesh_NMDetaill", false, "skinned"},
    StockShaderProfile{"ksSky", false, "mesh"},
    StockShaderProfile{"ksSkyBox", false, "mesh"},
    StockShaderProfile{"ksSkyCubemap", false, "mesh"},
    StockShaderProfile{"ksTest", false, "mesh"},
    StockShaderProfile{"ksTree", true, "mesh"},
    StockShaderProfile{"ksTyres", false, "mesh"},
    StockShaderProfile{"ksWindscreen", false, "mesh"},
    StockShaderProfile{"newStefano_ksTyres", false, "mesh"},
    StockShaderProfile{"stPerPixelNM_UVflow", false, "mesh"},
};

constexpr std::array<std::string_view, 4> kLayouts = {"mesh", "skinned", "particle", "2d"};

[[nodiscard]] std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
        ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        --end;
    return std::string(value.substr(begin, end - begin));
}

[[nodiscard]] std::string upper(std::string_view value) {
    std::string result(value);
    for (char& character : result)
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return result;
}

[[nodiscard]] std::string mode_name(const std::optional<std::string>& value, int fallback) {
    if (value.has_value() && !value->empty()) return upper(trim(*value));
    return std::to_string(fallback);
}

[[nodiscard]] std::string override_mode_name(const std::optional<std::string>& value) {
    if (!value.has_value()) return {};
    return upper(trim(*value));
}

[[nodiscard]] bool contains(std::string_view value, std::string_view needle) {
    return value.find(needle) != std::string_view::npos;
}

[[nodiscard]] std::optional<MaterialCullMode> explicit_cull(const std::optional<std::string>& value) {
    if (!value.has_value()) return std::nullopt;
    const std::string mode = upper(trim(*value));
    if (mode.empty()) return std::nullopt;
    if (mode == "NONE" || mode == "DOUBLESIDED" || mode == "DOUBLE_SIDED" || mode == "OFF")
        return MaterialCullMode::none;
    if (mode == "FRONT") return MaterialCullMode::front;
    return MaterialCullMode::back;
}

[[nodiscard]] bool positive_number(std::string_view value) {
    const std::string text = trim(value);
    if (text.empty()) return false;
    char* end = nullptr;
    const double number = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(number) || number <= 0.0) return false;
    while (*end != '\0') {
        if (std::isspace(static_cast<unsigned char>(*end)) == 0) return false;
        ++end;
    }
    return true;
}

[[noreturn]] void fail(std::string message, std::size_t offset) {
    throw MaterialProfileError(std::move(message), offset);
}

[[nodiscard]] std::uint32_t u32_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool dxbc_at(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return offset <= bytes.size() && bytes.size() - offset >= 4 && bytes[offset] == 0x44U &&
           bytes[offset + 1] == 0x58U && bytes[offset + 2] == 0x42U && bytes[offset + 3] == 0x43U;
}

struct StockShaderContainerLayout {
    StockShaderContainerHeader header;
    std::size_t vertex_offset = 0U;
    std::size_t pixel_offset = 0U;
    std::size_t geometry_offset = 0U;
    std::size_t end_offset = 0U;
};

[[nodiscard]] StockShaderContainerLayout parse_container_layout(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 14U) fail("Stock shader container is truncated", 0U);
    const std::uint8_t version = bytes[0];
    const std::uint32_t layout_code = u32_le(bytes, 2U);
    if (version != 2U)
        fail("Unsupported stock shader container version " +
                 std::to_string(version),
             0U);
    if (layout_code >= kLayouts.size())
        fail("Unsupported stock shader vertex layout " +
                 std::to_string(layout_code),
             2U);

    const std::uint32_t vertex_bytes = u32_le(bytes, 6U);
    constexpr std::size_t vertex_offset = 10U;
    if (!dxbc_at(bytes, vertex_offset) ||
        static_cast<std::size_t>(vertex_bytes) > bytes.size() - vertex_offset)
        fail("Invalid stock vertex shader payload", vertex_offset);
    const std::size_t pixel_size_offset =
        vertex_offset + static_cast<std::size_t>(vertex_bytes);
    if (pixel_size_offset > bytes.size() ||
        bytes.size() - pixel_size_offset < 8U)
        fail("Invalid stock vertex shader payload", pixel_size_offset);

    const std::uint32_t pixel_bytes = u32_le(bytes, pixel_size_offset);
    const std::size_t pixel_offset = pixel_size_offset + 4U;
    if (!dxbc_at(bytes, pixel_offset) ||
        static_cast<std::size_t>(pixel_bytes) >
            bytes.size() - pixel_offset - 4U)
        fail("Invalid stock pixel shader payload", pixel_offset);
    const std::size_t geometry_size_offset =
        pixel_offset + static_cast<std::size_t>(pixel_bytes);
    const std::uint32_t geometry_bytes = u32_le(bytes, geometry_size_offset);
    const std::size_t geometry_offset = geometry_size_offset + 4U;
    if (static_cast<std::size_t>(geometry_bytes) >
        bytes.size() - geometry_offset)
        fail("Invalid stock geometry shader payload", geometry_offset);
    if (geometry_bytes != 0U && !dxbc_at(bytes, geometry_offset))
        fail("Invalid stock geometry shader payload", geometry_offset);

    return {{version, bytes[1] != 0U, kLayouts[layout_code], vertex_bytes,
             pixel_bytes, geometry_bytes},
            vertex_offset, pixel_offset, geometry_offset,
            geometry_offset + static_cast<std::size_t>(geometry_bytes)};
}

enum class DxbcProgramType : std::uint16_t {
    pixel = 0U,
    vertex = 1U,
    geometry = 2U,
};

[[nodiscard]] StockShaderStageMetadata validate_dxbc_payload(
    std::span<const std::uint8_t> bytes, std::size_t container_offset,
    DxbcProgramType expected, const StockShaderContainerLimits& limits) {
    detail::DxbcContainerInspection inspection;
    std::size_t error_offset = 0U;
    const detail::DxbcReaderStatus status = detail::inspect_dxbc_container(
        detail::as_bytes(bytes), limits.max_dxbc_chunks, inspection,
        error_offset);
    switch (status) {
    case detail::DxbcReaderStatus::ready: break;
    case detail::DxbcReaderStatus::truncated_header:
    case detail::DxbcReaderStatus::invalid_signature:
        fail("Stock shader DXBC payload is truncated",
             container_offset + error_offset);
    case detail::DxbcReaderStatus::declared_size_mismatch:
        fail("Stock shader DXBC declared size does not match its stage payload",
             container_offset + error_offset);
    case detail::DxbcReaderStatus::invalid_chunk_count:
        fail("Stock shader DXBC chunk count is outside its limit",
             container_offset + error_offset);
    case detail::DxbcReaderStatus::chunk_table_out_of_bounds:
        fail("Stock shader DXBC chunk table exceeds its stage payload",
             container_offset + error_offset);
    case detail::DxbcReaderStatus::chunk_header_out_of_bounds:
        fail("Stock shader DXBC chunk offset exceeds its stage payload",
             container_offset + error_offset);
    case detail::DxbcReaderStatus::chunk_payload_out_of_bounds:
        fail("Stock shader DXBC chunk payload exceeds its stage payload",
             container_offset + error_offset);
    case detail::DxbcReaderStatus::chunks_overlap:
        fail("Stock shader DXBC chunks overlap",
             container_offset + error_offset);
    case detail::DxbcReaderStatus::program_chunk_truncated:
        fail("Stock shader DXBC must contain one complete program chunk",
             container_offset + error_offset);
    case detail::DxbcReaderStatus::invalid_header_version:
        fail("Stock shader DXBC container header version must be one",
             container_offset + error_offset);
    }
    if (inspection.program_chunk_count != 1U ||
        inspection.program_format != detail::DxbcProgramFormat::legacy)
        fail("Stock shader DXBC has no executable program chunk",
             container_offset);

    std::uint32_t program_version = 0U;
    for (std::uint32_t index = 0U; index < inspection.chunk_count; ++index) {
        detail::DxbcChunkView chunk;
        if (!detail::read_dxbc_chunk(detail::as_bytes(bytes), index, chunk))
            fail("Stock shader DXBC chunk offset exceeds its stage payload",
                 container_offset);
        if (chunk.tag != detail::dxbc_tag_shdr &&
            chunk.tag != detail::dxbc_tag_shex)
            continue;
        (void)detail::read_u32_le(chunk.payload, 0U, program_version);
        const auto program_type = static_cast<DxbcProgramType>(
            static_cast<std::uint16_t>(program_version >> 16U));
        if (program_type != expected)
            fail("Stock shader DXBC program type does not match its stage",
                 container_offset + chunk.header_offset + 8U);
        break;
    }
    return {static_cast<std::uint8_t>((program_version >> 4U) & 0x0fU),
            static_cast<std::uint8_t>(program_version & 0x0fU),
            inspection.chunk_count};
}

} // namespace

const StockShaderProfile* stock_shader_profile(std::string_view name) noexcept {
    const std::string candidate = upper(trim(name));
    for (const StockShaderProfile& profile : kStockProfiles) {
        if (upper(profile.name) == candidate) return &profile;
    }
    return nullptr;
}

std::span<const StockShaderProfile> stock_shader_profiles() noexcept {
    return std::span<const StockShaderProfile>(kStockProfiles.data(), kStockProfiles.size());
}

MaterialProfileError::MaterialProfileError(std::string message, std::size_t offset)
    : std::runtime_error([&] {
          std::ostringstream output;
          output << message << " at byte " << offset;
          return output.str();
      }()),
      offset_(offset) {}

StockShaderContainerHeader parse_stock_shader_container_header(std::span<const std::uint8_t> bytes) {
    return parse_container_layout(bytes).header;
}

StockShaderContainer parse_stock_shader_container(
    std::span<const std::uint8_t> bytes,
    const StockShaderContainerLimits& limits) {
    if (limits.max_container_bytes == 0U || limits.max_stage_bytes == 0U ||
        limits.max_dxbc_chunks == 0U)
        fail("Stock shader container limits are invalid", 0U);
    if (bytes.size() > limits.max_container_bytes)
        fail("Stock shader container exceeds its byte limit", 0U);
    const StockShaderContainerLayout layout = parse_container_layout(bytes);
    if (bytes[1] > 1U)
        fail("Stock shader alpha-tested flag must be zero or one", 1U);
    if (layout.end_offset != bytes.size())
        fail("Stock shader container has trailing bytes", layout.end_offset);
    if (layout.header.vertex_bytes > limits.max_stage_bytes)
        fail("Stock shader vertex stage exceeds its byte limit", 6U);
    if (layout.header.pixel_bytes > limits.max_stage_bytes)
        fail("Stock shader pixel stage exceeds its byte limit",
             layout.pixel_offset - 4U);
    if (layout.header.geometry_bytes > limits.max_stage_bytes)
        fail("Stock shader geometry stage exceeds its byte limit",
             layout.geometry_offset - 4U);

    const auto vertex = bytes.subspan(layout.vertex_offset,
                                      layout.header.vertex_bytes);
    const auto pixel = bytes.subspan(layout.pixel_offset,
                                     layout.header.pixel_bytes);
    const auto geometry = bytes.subspan(layout.geometry_offset,
                                        layout.header.geometry_bytes);
    const StockShaderStageMetadata vertex_metadata = validate_dxbc_payload(
        vertex, layout.vertex_offset, DxbcProgramType::vertex, limits);
    const StockShaderStageMetadata pixel_metadata = validate_dxbc_payload(
        pixel, layout.pixel_offset, DxbcProgramType::pixel, limits);
    std::optional<StockShaderStageMetadata> geometry_metadata;
    if (!geometry.empty()) {
        geometry_metadata = validate_dxbc_payload(
            geometry, layout.geometry_offset, DxbcProgramType::geometry,
            limits);
    }

    StockShaderContainer result;
    result.header = layout.header;
    result.vertex_metadata = vertex_metadata;
    result.pixel_metadata = pixel_metadata;
    result.geometry_metadata = geometry_metadata;
    result.vertex_shader.assign(vertex.begin(), vertex.end());
    result.pixel_shader.assign(pixel.begin(), pixel.end());
    result.geometry_shader.assign(geometry.begin(), geometry.end());
    return result;
}

StockShaderContainerFileResult load_stock_shader_container_file(
    const std::filesystem::path& path,
    const StockShaderContainerLimits& limits) {
    const auto failure = [&](StockShaderContainerFileStatus status,
                             std::string code, std::string message,
                             std::size_t offset = 0U) {
        return StockShaderContainerFileResult{
            status,
            {path, std::move(code), std::move(message), offset},
            std::nullopt};
    };
    try {
        if (limits.max_container_bytes == 0U ||
            limits.max_stage_bytes == 0U || limits.max_dxbc_chunks == 0U)
            return failure(
                StockShaderContainerFileStatus::invalid_limits,
                "stock_shader_file_limits_invalid",
                "Stock shader file limits must be nonzero");
        if (path.empty())
            return failure(
                StockShaderContainerFileStatus::invalid_path,
                "stock_shader_file_path_invalid",
                "The stock shader file path is empty");

        std::error_code error;
        const std::filesystem::file_status file_status =
            std::filesystem::status(path, error);
        if (error)
            return failure(
                StockShaderContainerFileStatus::not_regular_file,
                "stock_shader_file_not_regular",
                "The stock shader path is not a readable regular file");
        if (!std::filesystem::is_regular_file(file_status))
            return failure(
                StockShaderContainerFileStatus::not_regular_file,
                "stock_shader_file_not_regular",
                "The stock shader path is not a regular file");

        const std::uintmax_t file_bytes =
            std::filesystem::file_size(path, error);
        if (error)
            return failure(
                StockShaderContainerFileStatus::file_size_unavailable,
                "stock_shader_file_size_unavailable",
                "The stock shader file size is unavailable");
        if (file_bytes > limits.max_container_bytes ||
            file_bytes > std::numeric_limits<std::size_t>::max())
            return failure(
                StockShaderContainerFileStatus::file_too_large,
                "stock_shader_file_too_large",
                "The stock shader file exceeds its byte limit");

        std::ifstream input(path, std::ios::binary);
        if (!input)
            return failure(
                StockShaderContainerFileStatus::open_failed,
                "stock_shader_file_open_failed",
                "The stock shader file cannot be opened");
        std::vector<std::uint8_t> bytes(
            static_cast<std::size_t>(file_bytes));
        if (!bytes.empty()) {
            input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(bytes.size()));
            if (input.gcount() !=
                static_cast<std::streamsize>(bytes.size()))
                return failure(
                    input.bad()
                        ? StockShaderContainerFileStatus::read_failed
                        : StockShaderContainerFileStatus::file_size_changed,
                    input.bad() ? "stock_shader_file_read_failed"
                                : "stock_shader_file_size_changed",
                    input.bad()
                        ? "The stock shader file read failed"
                        : "The stock shader file size changed during the read");
        }
        char extra = 0;
        input.read(&extra, 1);
        if (input.gcount() != 0)
            return failure(
                StockShaderContainerFileStatus::file_size_changed,
                "stock_shader_file_size_changed",
                "The stock shader file size changed during the read");
        if (input.bad())
            return failure(
                StockShaderContainerFileStatus::read_failed,
                "stock_shader_file_read_failed",
                "The stock shader file read failed");

        try {
            StockShaderContainer container =
                parse_stock_shader_container(bytes, limits);
            return {StockShaderContainerFileStatus::ready, {path, {}, {}, 0U},
                    std::move(container)};
        } catch (const MaterialProfileError& parse_error) {
            return failure(
                StockShaderContainerFileStatus::malformed_package,
                "stock_shader_file_malformed_package", parse_error.what(),
                parse_error.offset());
        }
    } catch (const std::bad_alloc&) {
        return failure(
            StockShaderContainerFileStatus::allocation_failed,
            "stock_shader_file_allocation_failed",
            "The bounded stock shader file load cannot allocate memory");
    }
}

MaterialRenderProfile resolve_material_render_profile(const MaterialInput& material,
                                                       const NodeMaterialInput& node,
                                                       const MaterialOverride* override_values) {
    const std::optional<std::string> no_shader;
    const std::optional<std::string>* override_shader = override_values ? &override_values->shader : &no_shader;
    std::string shader;
    if (override_shader->has_value() && !override_shader->value().empty())
        shader = override_shader->value();
    else
        shader = material.shader;

    const StockShaderProfile* stock = stock_shader_profile(shader);
    const int serialized_blend_mode = material.serialized_blend_mode;
    const int package_blend_mode = stock != nullptr && stock->alpha_tested ? 2 : 0;
    const int native_blend_mode = serialized_blend_mode != 0 ? serialized_blend_mode : package_blend_mode;
    const std::optional<std::string> no_blend;
    const std::string override_blend = override_values ? override_mode_name(override_values->blend_mode) : "";

    int effective_blend_mode = native_blend_mode;
    std::string blend_source = serialized_blend_mode != 0
                                   ? "kn5"
                                   : package_blend_mode != 0 ? "shader-package" : "default";
    if (!override_blend.empty()) {
        blend_source = "override";
        if (override_blend == "2" || contains(override_blend, "ALPHA_TEST") ||
            contains(override_blend, "ALPHA_TO_COVERAGE") || contains(override_blend, "A2C"))
            effective_blend_mode = 2;
        else if (override_blend == "1" || contains(override_blend, "ALPHA_BLEND") ||
                 contains(override_blend, "TRANSPARENT_AS_BLACK") || contains(override_blend, "MULTIPLY"))
            effective_blend_mode = 1;
        else if (override_blend == "0" || contains(override_blend, "OPAQUE") ||
                 contains(override_blend, "NONE") || contains(override_blend, "OFF"))
            effective_blend_mode = 0;
    }

    MaterialRenderProfile result;
    result.shader = shader;
    result.stock = stock;
    result.serialized_blend_mode = serialized_blend_mode;
    result.native_blend_mode = native_blend_mode;
    result.effective_blend_mode = effective_blend_mode;
    result.blend_source = std::move(blend_source);
    result.alpha_to_coverage = effective_blend_mode == 2;
    result.shadow_alpha_tested = effective_blend_mode != 0;
    result.transparent = override_values && override_values->is_transparent.has_value()
                             ? override_values->is_transparent.value()
                             : node.transparent || effective_blend_mode == 1;
    result.blend_enabled = effective_blend_mode == 1;
    if (result.blend_enabled)
        result.blend = contains(override_blend, "MULTIPLY")
                           ? "multiply"
                           : contains(override_blend, "TRANSPARENT_AS_BLACK") ? "transparent-as-black" : "alpha";
    result.blend_mode = !override_blend.empty() ? override_blend : std::to_string(effective_blend_mode);

    const std::optional<std::string> no_depth;
    const std::optional<std::string>& depth_override = override_values ? override_values->depth_mode : no_depth;
    result.depth_mode = mode_name(depth_override, material.depth_mode);
    result.depth_test = result.depth_mode != "2" && result.depth_mode != "OFF" && result.depth_mode != "NONE";
    result.depth_write = result.depth_test && !result.transparent && result.depth_mode != "1" &&
                         !contains(result.depth_mode, "NOWRITE") && !contains(result.depth_mode, "READ_ONLY");

    const std::optional<std::string> no_cull;
    const std::optional<std::string>& cull_override = override_values ? override_values->cull_mode : no_cull;
    const std::optional<MaterialCullMode> cull = explicit_cull(cull_override);
    if (cull.has_value()) {
        result.cull = *cull;
        result.cull_source = "override";
    }

    const std::string shader_key = upper(trim(shader));
    result.windscreen = shader_key == "KSWINDSCREEN";
    result.broken_glass = shader_key == "KSBROKENGLASS";
    bool configured_refraction = false;
    if (override_values) {
        const auto property = override_values->properties.find("extrefraction");
        configured_refraction = property != override_values->properties.end() && positive_number(property->second);
    }
    result.refractive = result.broken_glass || contains(shader_key, "REFRACT") || configured_refraction;
    result.reflection_alpha = !result.windscreen && result.transparent &&
                              (contains(shader_key, "KSPERPIXELREFLECTION") || contains(shader_key, "SMGLASS") ||
                               contains(shader_key, "KSBROKENGLASS") || contains(shader_key, "REFRACT"));
    if (result.broken_glass)
        result.glass_mode = MaterialGlassMode::broken_glass_csp_refraction;
    else if (result.refractive)
        result.glass_mode = MaterialGlassMode::refractive;
    else if (result.windscreen)
        result.glass_mode = MaterialGlassMode::windscreen;
    else if (result.reflection_alpha)
        result.glass_mode = MaterialGlassMode::reflection;
    return result;
}

MaterialShaderAudit audit_material_shader_profiles(std::span<const MaterialInput> materials) {
    MaterialShaderAudit result;
    result.materials = materials.size();
    std::vector<std::string> unknown;
    for (const MaterialInput& material : materials) {
        const StockShaderProfile* stock = stock_shader_profile(material.shader);
        const MaterialRenderProfile profile = resolve_material_render_profile(material);
        if (stock != nullptr)
            ++result.known_stock;
        else if (!material.shader.empty() &&
                 std::find(unknown.begin(), unknown.end(), material.shader) == unknown.end())
            unknown.push_back(material.shader);
        if (profile.effective_blend_mode == 1) ++result.alpha_blend;
        if (profile.alpha_to_coverage) ++result.alpha_to_coverage;
        if (profile.shadow_alpha_tested) ++result.shadow_cutout;
        if (stock != nullptr && stock->alpha_tested && profile.serialized_blend_mode == 0)
            ++result.package_defaults;
        if (profile.serialized_blend_mode != 0) ++result.serialized_overrides;
        if (profile.windscreen) ++result.windscreens;
        if (profile.reflection_alpha) ++result.reflection_glass;
        if (profile.refractive) ++result.refractive;
    }
    std::sort(unknown.begin(), unknown.end());
    result.unknown_shaders = unknown;
    result.diagnostics.reserve(unknown.size());
    for (const std::string& shader : unknown)
        result.diagnostics.push_back({"unknown_shader", shader,
                                      "Unknown shader package; using conservative material defaults"});
    return result;
}

const char* material_cull_mode_name(MaterialCullMode mode) noexcept {
    switch (mode) {
    case MaterialCullMode::back:
        return "back";
    case MaterialCullMode::front:
        return "front";
    case MaterialCullMode::none:
        return "none";
    }
    return "unknown";
}

const char* material_glass_mode_name(MaterialGlassMode mode) noexcept {
    switch (mode) {
    case MaterialGlassMode::none:
        return "none";
    case MaterialGlassMode::windscreen:
        return "windscreen";
    case MaterialGlassMode::reflection:
        return "reflection";
    case MaterialGlassMode::refractive:
        return "refractive";
    case MaterialGlassMode::broken_glass_csp_refraction:
        return "broken glass · CSP refraction";
    }
    return "unknown";
}

} // namespace apex::render
