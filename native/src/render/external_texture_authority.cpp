#include "apex/render/external_texture_authority.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace apex::render {
namespace {

using Diagnostic = ExternalTextureAuthorityDiagnostic;
using Status = ExternalTextureAuthorityStatus;

[[nodiscard]] ExternalTextureAuthorityResult failure(Status status, std::size_t request_index, std::string grant_id,
                                                     std::string path, std::string code, std::string message) {
    ExternalTextureAuthorityResult result;
    result.status = status;
    result.diagnostics.push_back(
        {request_index, std::move(grant_id), std::move(path), std::move(code), std::move(message)});
    return result;
}

void add_diagnostic(ExternalTextureAuthorityResult& result, Status status, std::size_t request_index,
                    std::string grant_id, std::string path, std::string code, std::string message) {
    if (result.diagnostics.empty())
        result.status = status;
    result.diagnostics.push_back(
        {request_index, std::move(grant_id), std::move(path), std::move(code), std::move(message)});
}

[[nodiscard]] bool valid_limits(const ExternalTextureAuthorityLimits& limits) noexcept {
    return limits.max_requests != 0U && limits.max_grants != 0U && limits.max_grant_id_bytes != 0U &&
           limits.max_path_bytes != 0U && limits.max_file_bytes != 0U && limits.max_mip_levels != 0U &&
           limits.max_materials != 0U && limits.max_solid_colors != 0U &&
           limits.max_effective_nodes != 0U &&
           limits.max_effective_node_depth != 0U &&
           limits.max_effective_textures != 0U &&
           limits.max_effective_texture_name_bytes != 0U && limits.max_total_source_bytes != 0U &&
           limits.max_total_decoded_bytes != 0U && limits.max_total_plan_bytes != 0U &&
           limits.max_total_effective_source_bytes != 0U &&
           limits.max_total_effective_copy_bytes != 0U &&
           limits.decode.maxInputBytes != 0U &&
           limits.decode.maxOutputBytes != 0U;
}

[[nodiscard]] bool checked_add(std::uint64_t value, std::uint64_t& total, std::uint64_t limit) noexcept {
    if (value > limit || total > limit - value)
        return false;
    total += value;
    return true;
}

[[nodiscard]] bool checked_size_to_u64(std::size_t value,
                                       std::uint64_t &output) noexcept {
  if (value >
      static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
    return false;
  output = static_cast<std::uint64_t>(value);
  return true;
}

[[nodiscard]] std::string diagnostic_source(std::string_view grant_id,
                                            std::string_view path) {
    std::string source;
    source.reserve(grant_id.size() + 1U + path.size());
    source.append(grant_id);
    source.push_back(':');
    source.append(path);
    return source;
}

[[nodiscard]] bool decode_limit_diagnostic(std::string_view code) noexcept {
    return code == "input_too_large" || code == "output_too_large" ||
           code == "allocation_failed";
}

struct ResolvedRequest {
    std::size_t grant_index = 0U;
    const apex::assets::AssetFile* file = nullptr;
    std::size_t resource_index = invalid_external_texture_resource;
};

struct UniqueResource {
    std::size_t grant_index = 0U;
    const apex::assets::AssetFile* file = nullptr;
    std::string key;
    std::uint64_t source_bytes = 0U;
    std::size_t first_request_index = invalid_external_texture_resource;
};

} // namespace

const char* external_texture_authority_status_name(
    ExternalTextureAuthorityStatus status) noexcept {
    switch (status) {
    case ExternalTextureAuthorityStatus::ready: return "ready";
    case ExternalTextureAuthorityStatus::invalid_request: return "invalid_request";
    case ExternalTextureAuthorityStatus::rejected: return "rejected";
    case ExternalTextureAuthorityStatus::missing: return "missing";
    case ExternalTextureAuthorityStatus::ambiguous: return "ambiguous";
    case ExternalTextureAuthorityStatus::unsupported: return "unsupported";
    case ExternalTextureAuthorityStatus::resource_limit: return "resource_limit";
    case ExternalTextureAuthorityStatus::read_failed: return "read_failed";
    }
    return "invalid_request";
}

ExternalTextureAuthorityResult resolve_external_texture_authority(
    std::span<const ExternalTextureGrant> grants,
    std::span<const ExternalTextureRequest> requests,
    ExternalTextureAuthorityLimits limits) try {
    ExternalTextureAuthorityResult result;
    result.status = Status::ready;

    if (!valid_limits(limits))
        return failure(Status::invalid_request, invalid_external_texture_resource, {}, {},
                       "external_texture_limits_invalid",
                       "External texture authority limits must be nonzero");
    if (grants.size() > limits.max_grants)
        return failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                       "external_texture_grant_limit",
                       "External texture grant count exceeds the configured limit");
    if (requests.size() > limits.max_requests)
        return failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                       "external_texture_request_limit",
                       "External texture request count exceeds the configured limit");

    std::map<std::string, std::size_t> grant_indices;
    for (std::size_t index = 0U; index < grants.size(); ++index) {
        const auto& grant = grants[index];
        if (grant.id.empty() || grant.id.size() > limits.max_grant_id_bytes ||
            grant.id.find('\0') != std::string::npos) {
            add_diagnostic(result, Status::invalid_request, invalid_external_texture_resource,
                           grant.id, {}, "external_texture_grant_id_invalid",
                           "External texture grant identity is empty, too large, or contains NUL");
            continue;
        }
        if (grant.source == nullptr) {
            add_diagnostic(result, Status::invalid_request, invalid_external_texture_resource,
                           grant.id, {}, "external_texture_grant_source_missing",
                           "External texture grant has no AssetSource");
            continue;
        }
        if (!grant_indices.emplace(grant.id, index).second)
            add_diagnostic(result, Status::invalid_request, invalid_external_texture_resource,
                           grant.id, {}, "external_texture_grant_duplicate",
                           "External texture grant identities must be unique");
    }
    if (!result.diagnostics.empty()) return result;

    std::vector<ResolvedRequest> resolved(requests.size());
    std::vector<UniqueResource> unique;
    std::map<std::pair<std::size_t, std::string>, std::size_t> unique_by_key;
    std::uint64_t total_source_bytes = 0U;

    for (std::size_t request_index = 0U; request_index < requests.size(); ++request_index) {
        const auto& request = requests[request_index];
        const auto grant_found = grant_indices.find(request.grant_id);
        if (grant_found == grant_indices.end()) {
            add_diagnostic(result, Status::rejected, request_index, request.grant_id,
                           request.binding.file, "external_texture_grant_unknown",
                           "External texture request names an unknown grant");
            continue;
        }
        if (request.binding.kind != MaterialTextureKind::external_file) {
            add_diagnostic(result, Status::invalid_request, request_index, request.grant_id,
                           request.binding.file, "external_texture_binding_kind_invalid",
                           "External texture authority accepts only external_file bindings");
            continue;
        }
        if (request.binding.file.empty()) {
            add_diagnostic(result, Status::invalid_request, request_index, request.grant_id,
                           {}, "external_texture_path_empty",
                           "External texture binding has no file path");
            continue;
        }
        if (request.binding.file.size() > limits.max_path_bytes) {
            add_diagnostic(result, Status::resource_limit, request_index, request.grant_id,
                           request.binding.file, "external_texture_path_limit",
                           "External texture path exceeds the configured limit");
            continue;
        }
        // AssetSource's logical resolver rejects drive roots, but a URI such
        // as http://host/file.dds is otherwise a syntactically relative
        // sequence of components. A grant can expose only path-like names;
        // reject every colon so URI schemes and NTFS alternate data streams
        // cannot cross this boundary.
        if (request.binding.file.find(':') != std::string::npos) {
            add_diagnostic(result, Status::rejected, request_index, request.grant_id,
                           request.binding.file, "external_texture_path_scheme",
                           "External texture paths cannot contain URI or drive syntax");
            continue;
        }

        const auto& grant = grants[grant_found->second];
        const auto resolution = grant.source->resolve(request.binding.file);
        if (resolution.status != apex::assets::AssetResolveStatus::resolved ||
            resolution.file == nullptr) {
            const auto status = resolution.status == apex::assets::AssetResolveStatus::rejected
                                    ? Status::rejected
                                    : resolution.status == apex::assets::AssetResolveStatus::ambiguous
                                        ? Status::ambiguous
                                        : Status::missing;
            const auto code = resolution.status == apex::assets::AssetResolveStatus::rejected
                                  ? "external_texture_path_rejected"
                                  : resolution.status == apex::assets::AssetResolveStatus::ambiguous
                                      ? "external_texture_path_ambiguous"
                                      : "external_texture_path_missing";
            add_diagnostic(result, status, request_index, request.grant_id,
                           request.binding.file, code,
                           resolution.diagnostic.empty() ? "External texture path did not resolve"
                                                         : resolution.diagnostic);
            continue;
        }

        const auto* file = resolution.file;
        if (file->path.size() > limits.max_path_bytes ||
            file->relativePath.size() > limits.max_path_bytes) {
            add_diagnostic(result, Status::resource_limit, request_index, request.grant_id,
                           request.binding.file, "external_texture_resolved_path_limit",
                           "Resolved external texture path exceeds the configured limit");
            continue;
        }
        std::uint64_t file_bytes = 0U;
        if (!checked_size_to_u64(file->size, file_bytes) ||
            file->size > limits.max_file_bytes) {
            add_diagnostic(result, Status::resource_limit, request_index, request.grant_id,
                           request.binding.file, "external_texture_file_limit",
                           "External texture file exceeds the configured size limit");
            continue;
        }

        resolved[request_index] = {grant_found->second, file,
                                   invalid_external_texture_resource};
        const std::pair<std::size_t, std::string> key{grant_found->second, file->path};
        const auto found = unique_by_key.find(key);
        if (found != unique_by_key.end()) {
            resolved[request_index].resource_index = found->second;
            continue;
        }
        if (!checked_add(file_bytes, total_source_bytes, limits.max_total_source_bytes)) {
            add_diagnostic(result, Status::resource_limit, request_index, request.grant_id,
                           request.binding.file, "external_texture_source_aggregate_limit",
                           "External texture source bytes exceed the aggregate limit");
            continue;
        }
        const std::size_t resource_index = unique.size();
        unique_by_key.emplace(key, resource_index);
        unique.push_back({grant_found->second, file, std::move(key.second), file_bytes,
                          request_index});
        resolved[request_index].resource_index = resource_index;
    }
    if (!result.diagnostics.empty()) {
        result.resources.clear();
        result.request_resource_indices.clear();
        return result;
    }

    try {
        result.resources.reserve(unique.size());
        result.request_resource_indices.resize(requests.size(),
                                                invalid_external_texture_resource);
        std::uint64_t total_decoded_bytes = 0U;
        std::uint64_t total_plan_bytes = 0U;
        for (std::size_t resource_index = 0U; resource_index < unique.size();
             ++resource_index) {
            const auto& item = unique[resource_index];
            const auto& grant = grants[item.grant_index];
            std::vector<std::uint8_t> bytes;
            try {
                bytes = grant.source->read(*item.file);
            } catch (const apex::core::ParseError& error) {
                add_diagnostic(result, Status::read_failed, item.first_request_index,
                               grant.id, item.file->path,
                               "external_texture_read_" + error.code(), error.what());
                break;
            } catch (const std::bad_alloc&) {
                add_diagnostic(result, Status::resource_limit, item.first_request_index,
                               grant.id, item.file->path, "external_texture_read_allocation_failed",
                               "External texture read exceeded bounded host storage");
                break;
            } catch (const std::exception& error) {
                add_diagnostic(result, Status::read_failed, item.first_request_index,
                               grant.id, item.file->path, "external_texture_read_failed",
                               error.what());
                break;
            }
            if (bytes.size() != item.file->size || bytes.size() > limits.max_file_bytes) {
                add_diagnostic(result, Status::read_failed, item.first_request_index,
                               grant.id, item.file->path, "external_texture_read_size_changed",
                               "External texture size changed after grant resolution");
                break;
            }

            apex::core::ParseLimits decode_limits = limits.decode;
            decode_limits.maxInputBytes = std::min(decode_limits.maxInputBytes,
                                                   limits.max_file_bytes);
            const std::uint64_t remaining_decoded =
                limits.max_total_decoded_bytes - std::min(total_decoded_bytes,
                                                          limits.max_total_decoded_bytes);
            if (remaining_decoded == 0U) {
                add_diagnostic(result, Status::resource_limit, item.first_request_index,
                               grant.id, item.file->path, "external_texture_decoded_aggregate_limit",
                               "Decoded external texture bytes exceed the aggregate limit");
                break;
            }
            decode_limits.maxOutputBytes = std::min(
                decode_limits.maxOutputBytes,
                static_cast<std::size_t>(std::min<std::uint64_t>(
                    remaining_decoded, std::numeric_limits<std::size_t>::max())));
            const auto planned = plan_decoded_texture_payload(
                bytes, diagnostic_source(grant.id, item.file->path), decode_limits);
            if (!planned.ok()) {
                const auto status =
                    planned.status == TextureUploadStatus::unsupported
                        ? Status::unsupported
                        : decode_limit_diagnostic(planned.diagnostic.code)
                              ? Status::resource_limit
                              : Status::invalid_request;
                add_diagnostic(result, status, item.first_request_index, grant.id,
                               item.file->path, "external_texture_decode_" + planned.diagnostic.code,
                               planned.diagnostic.message);
                break;
            }
            if (planned.plan.levels.size() > limits.max_mip_levels) {
                add_diagnostic(result, Status::resource_limit, item.first_request_index,
                               grant.id, item.file->path, "external_texture_mip_limit",
                               "External texture mip count exceeds the configured limit");
                break;
            }
            std::uint64_t decoded_bytes = 0U;
            for (const auto& level : planned.plan.levels) {
                std::uint64_t level_bytes = 0U;
                if (!checked_size_to_u64(level.pixels.size(), level_bytes) ||
                    !checked_add(level_bytes, decoded_bytes,
                                 std::numeric_limits<std::uint64_t>::max())) {
                    add_diagnostic(result, Status::resource_limit,
                                   item.first_request_index, grant.id, item.file->path,
                                   "external_texture_decoded_size_overflow",
                                   "Decoded external texture size overflows the authority counter");
                    break;
                }
            }
            if (!result.diagnostics.empty()) break;
            if (!checked_add(decoded_bytes, total_decoded_bytes,
                             limits.max_total_decoded_bytes)) {
                add_diagnostic(result, Status::resource_limit, item.first_request_index,
                               grant.id, item.file->path, "external_texture_decoded_aggregate_limit",
                               "Decoded external texture bytes exceed the aggregate limit");
                break;
            }
            std::uint64_t plan_bytes = sizeof(DecodedTexturePlan);
            std::uint64_t level_metadata = 0U;
            std::uint64_t source_payload_bytes = 0U;
            if (!checked_size_to_u64(planned.plan.levels.size(), level_metadata) ||
                !checked_size_to_u64(bytes.size(), source_payload_bytes) ||
                level_metadata > std::numeric_limits<std::uint64_t>::max() /
                                     sizeof(DecodedTextureLevel) ||
                !checked_add(level_metadata * sizeof(DecodedTextureLevel), plan_bytes,
                             std::numeric_limits<std::uint64_t>::max()) ||
                !checked_add(source_payload_bytes, plan_bytes,
                             std::numeric_limits<std::uint64_t>::max()) ||
                !checked_add(decoded_bytes, plan_bytes,
                             std::numeric_limits<std::uint64_t>::max())) {
                add_diagnostic(result, Status::resource_limit,
                               item.first_request_index, grant.id, item.file->path,
                               "external_texture_plan_size_overflow",
                               "External texture plan size overflows the authority counter");
                break;
            }
            if (!checked_add(plan_bytes, total_plan_bytes, limits.max_total_plan_bytes)) {
                add_diagnostic(result, Status::resource_limit, item.first_request_index,
                               grant.id, item.file->path, "external_texture_plan_aggregate_limit",
                               "Retained external texture plans exceed the aggregate limit");
                break;
            }
            result.resources.push_back({grant.id, {}, item.file->path, item.file->kind,
                                        std::move(bytes), std::move(planned.plan)});
            for (std::size_t request_index = 0U; request_index < resolved.size();
                 ++request_index) {
                if (resolved[request_index].resource_index == resource_index) {
                    result.request_resource_indices[request_index] = resource_index;
                    if (result.resources.back().requested_path.empty())
                        result.resources.back().requested_path = requests[request_index].binding.file;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        result.resources.clear();
        result.request_resource_indices.clear();
        add_diagnostic(result, Status::resource_limit, invalid_external_texture_resource, {}, {},
                       "external_texture_authority_allocation_failed",
                       "External texture authority exceeded bounded host storage");
    }
    if (!result.diagnostics.empty()) {
        result.resources.clear();
        result.request_resource_indices.clear();
        return result;
    }
    result.status = Status::ready;
    return result;
} catch (const std::bad_alloc&) {
    return failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                   "external_texture_authority_allocation_failed",
                   "External texture authority exceeded bounded host storage");
} catch (const std::length_error&) {
        return failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                       "external_texture_authority_allocation_failed",
                       "External texture authority exceeded bounded host storage");
}

namespace {

[[nodiscard]] std::string canonical_effective_slot(std::string_view value) {
    std::size_t first = 0U;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0)
        ++first;
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0)
        --last;
    std::string result;
    result.reserve(last - first);
    for (std::size_t index = first; index < last; ++index)
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value[index]))));
    return result;
}

[[nodiscard]] ExternalTextureEffectiveStockSceneResult effective_failure(Status status, std::size_t request_index,
                                                                         std::string grant_id, std::string path,
                                                                         std::string code, std::string message) {
    ExternalTextureEffectiveStockSceneResult result;
    result.status = status;
    result.diagnostics.push_back(
        {request_index, std::move(grant_id), std::move(path), std::move(code), std::move(message)});
    return result;
}

[[nodiscard]] bool finite_solid_color(const std::array<float, 4>& color) noexcept {
    return std::all_of(color.begin(), color.end(), [](float value) { return std::isfinite(value); });
}

[[nodiscard]] std::array<float, 4> clamp_solid_color(const std::array<float, 4>& color) noexcept {
    std::array<float, 4> result{};
    std::transform(color.begin(), color.end(), result.begin(),
                   [](float value) { return std::clamp(value, 0.0F, 1.0F); });
    return result;
}

[[nodiscard]] std::uint8_t solid_color_byte(float value) noexcept {
    const double scaled = static_cast<double>(value) * 255.0 + 0.5;
    return static_cast<std::uint8_t>(std::floor(scaled));
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) noexcept {
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

inline constexpr std::uint64_t solid_color_dds_bytes = 132U;

// Match src/fbx-import.js createColorDds(): one legacy 1x1 BGRA8 DDS with no
// generated mip chain. public/app.js applies the same clamping and channel
// rounding before it uploads a direct RGBA8 WebGL texture.
[[nodiscard]] std::vector<std::uint8_t> make_solid_color_bgra8_dds(const std::array<float, 4>& color) {
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(solid_color_dds_bytes), 0U);
    put_u32(bytes, 0U, 0x20534444U);
    put_u32(bytes, 4U, 124U);
    put_u32(bytes, 8U, 0x100fU);
    put_u32(bytes, 12U, 1U);
    put_u32(bytes, 16U, 1U);
    put_u32(bytes, 20U, 4U);
    put_u32(bytes, 76U, 32U);
    put_u32(bytes, 80U, 0x41U);
    put_u32(bytes, 88U, 32U);
    put_u32(bytes, 92U, 0x00ff0000U);
    put_u32(bytes, 96U, 0x0000ff00U);
    put_u32(bytes, 100U, 0x000000ffU);
    put_u32(bytes, 104U, 0xff000000U);
    put_u32(bytes, 108U, 0x1000U);
    bytes[128U] = solid_color_byte(color[2U]);
    bytes[129U] = solid_color_byte(color[1U]);
    bytes[130U] = solid_color_byte(color[0U]);
    bytes[131U] = solid_color_byte(color[3U]);
    return bytes;
}

[[nodiscard]] bool charge_effective_size(std::size_t value,
                                         std::uint64_t& total,
                                         std::uint64_t limit) noexcept {
    std::uint64_t converted = 0U;
    return checked_size_to_u64(value, converted) &&
           checked_add(converted, total, limit);
}

[[nodiscard]] bool charge_effective_count(std::size_t count,
                                          std::size_t element_size,
                                          std::uint64_t& total,
                                          std::uint64_t limit) noexcept {
    std::uint64_t converted_count = 0U;
    std::uint64_t converted_size = 0U;
    if (!checked_size_to_u64(count, converted_count) ||
        !checked_size_to_u64(element_size, converted_size) ||
        (converted_size != 0U &&
         converted_count >
             std::numeric_limits<std::uint64_t>::max() / converted_size))
        return false;
    return checked_add(converted_count * converted_size, total, limit);
}

[[nodiscard]] bool estimate_effective_copy(
    const apex::formats::Kn5File& model,
    std::span<const MaterialBindingOverrides> overrides_by_material,
    const ExternalTextureAuthorityLimits& limits) {
    const std::uint64_t limit = limits.max_total_effective_copy_bytes;
    std::uint64_t total = 0U;
    const auto charge_string = [&](std::string_view value) {
        return charge_effective_size(value.size(), total, limit);
    };
    if (!checked_add(sizeof(apex::formats::Kn5File), total, limit) ||
        !charge_string(model.source) || !charge_string(model.magic) ||
        !charge_effective_count(model.textures.size(),
                                sizeof(apex::formats::Kn5Texture), total,
                                limit) ||
        !charge_effective_count(model.materials.size(),
                                sizeof(apex::formats::Kn5Material), total,
                                limit))
        return false;
    for (const auto& texture : model.textures) {
        if (!charge_string(texture.name) ||
            !charge_effective_size(texture.data.size(), total, limit))
            return false;
    }
    for (const auto& material : model.materials) {
        if (!charge_string(material.name) || !charge_string(material.shader) ||
            !charge_effective_count(
                material.properties.size(),
                sizeof(apex::formats::Kn5MaterialProperty), total, limit) ||
            !charge_effective_count(
                material.resources.size(),
                sizeof(apex::formats::Kn5MaterialResource), total, limit))
            return false;
        for (const auto& property : material.properties)
            if (!charge_string(property.name)) return false;
        for (const auto& resource : material.resources)
            if (!charge_string(resource.slot) ||
                !charge_string(resource.texture))
                return false;
    }

    using NodeEntry = std::pair<const apex::formats::Kn5Node*, std::size_t>;
    std::vector<NodeEntry> nodes;
    nodes.emplace_back(&model.root, 1U);
    std::size_t node_count = 0U;
    while (!nodes.empty()) {
        const auto [node, depth] = nodes.back();
        nodes.pop_back();
        if (depth > limits.max_effective_node_depth ||
            node_count >= limits.max_effective_nodes)
            return false;
        ++node_count;
        if (!checked_add(sizeof(apex::formats::Kn5Node), total, limit) ||
            !charge_string(node->kind) || !charge_string(node->name) ||
            !charge_effective_count(node->bones.size(),
                                    sizeof(apex::formats::Kn5Bone), total,
                                    limit) ||
            !charge_effective_count(node->vertices.size(), sizeof(float),
                                    total, limit) ||
            !charge_effective_count(node->indices.size(),
                                    sizeof(std::uint16_t), total, limit))
            return false;
        for (const auto& bone : node->bones)
            if (!charge_string(bone.name)) return false;
        if (nodes.size() > limits.max_effective_nodes - node_count ||
            node->children.size() >
                limits.max_effective_nodes - node_count - nodes.size())
            return false;
        for (const auto& child : node->children)
            nodes.emplace_back(&child, depth + 1U);
    }

    if (model.encryption.has_value()) {
        const auto& encryption = *model.encryption;
        if (!checked_add(sizeof(encryption), total, limit) ||
            !charge_string(encryption.format) ||
            !charge_string(encryption.error) ||
            !charge_effective_count(encryption.protectedTextures.size(),
                                    sizeof(std::string), total, limit) ||
            !charge_effective_count(encryption.protectedMeshes.size(),
                                    sizeof(std::string), total, limit))
            return false;
        for (const auto& value : encryption.protectedTextures)
            if (!charge_string(value)) return false;
        for (const auto& value : encryption.protectedMeshes)
            if (!charge_string(value)) return false;
    }

    constexpr std::size_t map_node_overhead = 3U * sizeof(void*);
    if (!charge_effective_count(overrides_by_material.size(),
                                sizeof(MaterialBindingOverrides), total,
                                limit))
        return false;
    for (const auto& overrides : overrides_by_material) {
        for (const auto* value : {
                 &overrides.shader, &overrides.blend_mode,
                 &overrides.depth_mode, &overrides.cull_mode})
            if (value->has_value() && !charge_string(**value)) return false;
        if (!charge_effective_count(
                overrides.properties.size(),
                sizeof(decltype(overrides.properties)::value_type) +
                    map_node_overhead,
                total, limit) ||
            !charge_effective_count(
                overrides.resources.size(),
                sizeof(decltype(overrides.resources)::value_type) +
                    map_node_overhead,
                total, limit))
            return false;
        for (const auto& [name, value] : overrides.properties) {
            static_cast<void>(value);
            if (!charge_string(name)) return false;
        }
        for (const auto& [name, value] : overrides.resources)
            if (!charge_string(name) || !charge_string(value.texture) ||
                !charge_string(value.file))
                return false;
    }
    return true;
}

} // namespace

ExternalTextureEffectiveStockSceneResult prepare_effective_stock_scene_input(
    const apex::formats::Kn5File& model, std::span<const MaterialBindingOverrides> overrides_by_material,
    std::span<const ExternalTextureGrant> grants, std::span<const ExternalTextureRequest> requests,
    ExternalTextureAuthorityLimits limits) try {
    if (!valid_limits(limits))
        return effective_failure(Status::invalid_request, invalid_external_texture_resource, {}, {},
                                 "external_texture_limits_invalid",
                                 "External texture authority limits must be nonzero");
    if (model.materials.size() > limits.max_materials)
        return effective_failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                                 "external_texture_material_limit",
                                 "Effective scene material count exceeds the configured limit");
    if (model.textures.size() > limits.max_effective_textures)
        return effective_failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                                 "external_texture_effective_texture_limit",
                                 "Effective scene texture count exceeds the configured limit");
    if (!overrides_by_material.empty() && overrides_by_material.size() != model.materials.size())
        return effective_failure(Status::invalid_request, invalid_external_texture_resource, {}, {},
                                 "external_texture_override_table_invalid",
                                 "Effective scene overrides must match the material table");
    if (!estimate_effective_copy(model, overrides_by_material, limits))
        return effective_failure(
            Status::resource_limit, invalid_external_texture_resource, {}, {},
            "external_texture_effective_copy_limit",
            "Effective scene copy exceeds the node, depth, or storage limit");

    // Validate every request and collect the exact map key to erase. A
    // canonical-slot collision is rejected before any model is copied or
    // modified, so two sources cannot silently compete for one material slot.
    struct ConsumedOverride {
        std::size_t material_index = 0U;
        std::string raw_key;
        std::size_t request_index = 0U;
        std::string canonical_slot;
    };
    std::vector<ConsumedOverride> consumed;
    consumed.reserve(requests.size());
    std::map<std::pair<std::size_t, std::string>, std::size_t> request_slots;
    for (std::size_t request_index = 0U; request_index < requests.size(); ++request_index) {
        const auto& request = requests[request_index];
        if (request.material_index == invalid_external_texture_resource ||
            request.material_index >= model.materials.size())
            return effective_failure(Status::invalid_request, request_index, request.grant_id, request.binding.file,
                                     "external_texture_material_index_invalid",
                                     "External texture request names no material in the effective scene");
        const auto& material = model.materials[request.material_index];
        if (request.workspace_file_index != material.workspaceFileIndex)
            return effective_failure(Status::rejected, request_index, request.grant_id, request.binding.file,
                                     "external_texture_workspace_scope_mismatch",
                                     "External texture request workspace scope does "
                                     "not match its material");
        if (request.binding.slot.empty() || request.binding.slot.size() > limits.max_path_bytes ||
            request.binding.slot.find('\0') != std::string::npos ||
            canonical_effective_slot(request.binding.slot).empty())
            return effective_failure(Status::invalid_request, request_index, request.grant_id, request.binding.file,
                                     "external_texture_slot_invalid",
                                     "External texture request slot is empty or "
                                     "exceeds the configured limit");
        const std::string canonical_slot = canonical_effective_slot(request.binding.slot);
        if (!request_slots.emplace(std::make_pair(request.material_index, canonical_slot), request_index).second)
            return effective_failure(Status::ambiguous, request_index, request.grant_id, request.binding.file,
                                     "external_texture_slot_collision",
                                     "Multiple external texture requests address one material slot");

        if (overrides_by_material.empty())
            return effective_failure(Status::invalid_request, request_index, request.grant_id, request.binding.file,
                                     "external_texture_override_missing",
                                     "External texture request has no material override table");
        const auto& overrides = overrides_by_material[request.material_index];
        const MaterialTextureOverride* matched = nullptr;
        std::string matched_key;
        std::set<std::string> override_slots;
        for (const auto& [raw_key, value] : overrides.resources) {
            const std::string key = canonical_effective_slot(raw_key);
            if (!override_slots.insert(key).second)
                return effective_failure(Status::ambiguous, request_index, request.grant_id, request.binding.file,
                                         "external_texture_override_collision",
                                         "Material resource overrides contain colliding slot names");
            if (key == canonical_slot) {
                matched = &value;
                matched_key = raw_key;
            }
        }
        if (matched == nullptr || matched->color.has_value() || matched->file.empty() ||
            matched->file != request.binding.file)
            return effective_failure(Status::rejected, request_index, request.grant_id, request.binding.file,
                                     "external_texture_override_mismatch",
                                     "External texture request does not match one material file override");
        consumed.push_back({request.material_index, std::move(matched_key), request_index, canonical_slot});
    }

    struct SolidColorOverride {
        std::size_t material_index = 0U;
        std::string raw_key;
        std::string canonical_slot;
        std::array<float, 4> color{};
        std::optional<std::uint32_t> bind_point;
        std::size_t matching_resource = invalid_external_texture_resource;
    };
    std::vector<SolidColorOverride> solid_colors;
    for (std::size_t material_index = 0U; material_index < overrides_by_material.size(); ++material_index) {
        const auto& overrides = overrides_by_material[material_index];
        const bool has_solid_color = std::any_of(overrides.resources.begin(), overrides.resources.end(),
                                                 [](const auto& entry) { return entry.second.color.has_value(); });
        if (!has_solid_color)
            continue;
        std::set<std::string> override_slots;
        for (const auto& [raw_key, value] : overrides.resources) {
            const std::string canonical_slot = canonical_effective_slot(raw_key);
            if (raw_key.empty() || raw_key.size() > limits.max_path_bytes || raw_key.find('\0') != std::string::npos ||
                canonical_slot.empty())
                return effective_failure(Status::invalid_request, invalid_external_texture_resource, {}, raw_key,
                                         "solid_color_slot_invalid",
                                         "Solid-color material slot is empty or "
                                         "exceeds the configured limit");
            if (!override_slots.insert(canonical_slot).second)
                return effective_failure(Status::ambiguous, invalid_external_texture_resource, {}, raw_key,
                                         "solid_color_override_collision",
                                         "Material resource overrides contain colliding slot names");
            if (!value.color.has_value())
                continue;
            if (solid_colors.size() >= limits.max_solid_colors)
                return effective_failure(Status::resource_limit, invalid_external_texture_resource, {}, raw_key,
                                         "solid_color_count_limit",
                                         "Solid-color override count exceeds the configured limit");
            if (!finite_solid_color(*value.color))
                return effective_failure(Status::invalid_request, invalid_external_texture_resource, {}, raw_key,
                                         "solid_color_non_finite",
                                         "Solid-color material resources must contain finite channels");

            std::size_t matching_resource = invalid_external_texture_resource;
            const auto& material = model.materials[material_index];
            for (std::size_t resource_index = 0U; resource_index < material.resources.size(); ++resource_index) {
                if (canonical_effective_slot(material.resources[resource_index].slot) != canonical_slot)
                    continue;
                if (matching_resource != invalid_external_texture_resource)
                    return effective_failure(Status::ambiguous, invalid_external_texture_resource, {}, raw_key,
                                             "solid_color_material_resource_collision",
                                             "Material contains colliding resource slots");
                matching_resource = resource_index;
            }
            if (matching_resource == invalid_external_texture_resource && !value.bind_point.has_value())
                return effective_failure(Status::invalid_request, invalid_external_texture_resource, {}, raw_key,
                                         "solid_color_bind_point_missing",
                                         "A new solid-color material resource requires "
                                         "an explicit bind point");
            solid_colors.push_back({material_index, raw_key, canonical_slot, clamp_solid_color(*value.color),
                                    value.bind_point, matching_resource});
        }
    }

    // Validate all model and override references before the authority reads an
    // external file. A malformed solid color cannot trigger file I/O or model
    // publication when both override kinds are present.
    const ExternalTextureAuthorityResult authority =
        resolve_external_texture_authority(grants, requests, limits);
    if (!authority.ok()) {
        ExternalTextureEffectiveStockSceneResult result;
        result.status = authority.status;
        result.diagnostics = authority.diagnostics;
        return result;
    }
    if (authority.request_resource_indices.size() != requests.size())
        return effective_failure(Status::invalid_request,
                                 invalid_external_texture_resource, {}, {},
                                 "external_texture_authority_mapping_invalid",
                                 "External texture authority returned an invalid resource map");
    for (std::size_t request_index = 0U; request_index < requests.size();
         ++request_index) {
        if (authority.request_resource_indices[request_index] ==
            invalid_external_texture_resource)
            return effective_failure(
                Status::invalid_request, request_index,
                requests[request_index].grant_id,
                requests[request_index].binding.file,
                "external_texture_authority_mapping_invalid",
                "External texture authority returned no resource for a valid request");
    }

    try {
        auto input = std::make_unique<ExternalTextureEffectiveStockSceneInput>();
        input->model = model;
        input->overrides_by_material.assign(overrides_by_material.begin(), overrides_by_material.end());
        input->external_bindings.reserve(requests.size());
        input->solid_color_bindings.reserve(solid_colors.size());

        using ScopeKey = std::pair<std::size_t, std::optional<std::size_t>>;
        std::map<std::pair<std::size_t, ScopeKey>, std::size_t> external_synthetic_indices;
        std::set<std::pair<ScopeKey, std::string>> texture_names;
        for (const auto& texture : input->model.textures) {
            texture_names.emplace(
                ScopeKey{texture.workspaceFileIndex.has_value() ? 1U : 0U, texture.workspaceFileIndex},
                canonical_effective_slot(texture.name));
        }
        std::uint64_t effective_source_bytes = 0U;
        std::size_t next_name = 0U;
        for (const ConsumedOverride& item : consumed) {
            const auto& request = requests[item.request_index];
            const std::size_t authority_index = authority.request_resource_indices[item.request_index];
            const auto& resource = authority.resources[authority_index];
            const auto& material = input->model.materials[item.material_index];
            const ScopeKey scope{material.workspaceFileIndex.has_value() ? 1U : 0U, material.workspaceFileIndex};
            const std::pair<std::size_t, ScopeKey> resource_key{authority_index, scope};
            auto found_synthetic = external_synthetic_indices.find(resource_key);
            std::size_t texture_index = invalid_external_texture_resource;
            if (found_synthetic != external_synthetic_indices.end()) {
                texture_index = found_synthetic->second;
            } else {
                if (resource.source_bytes.empty() || resource.plan.levels.empty())
                    return effective_failure(Status::read_failed, item.request_index, request.grant_id,
                                             request.binding.file, "external_texture_owned_payload_invalid",
                                             "Authority returned an invalid owned DDS payload");
                if (!checked_add(static_cast<std::uint64_t>(resource.source_bytes.size()), effective_source_bytes,
                                 limits.max_total_effective_source_bytes))
                    return effective_failure(Status::resource_limit, item.request_index, request.grant_id,
                                             request.binding.file, "external_texture_effective_source_limit",
                                             "Owned effective DDS bytes exceed the aggregate limit");
                if (input->model.textures.size() >= limits.max_effective_textures ||
                    resource.source_bytes.size() > std::numeric_limits<std::uint32_t>::max())
                    return effective_failure(Status::resource_limit, item.request_index, request.grant_id,
                                             request.binding.file, "external_texture_effective_texture_limit",
                                             "Synthetic effective texture exceeds the configured limit");
                std::string name;
                do {
                    name = "__apex_external_image_" + std::to_string(next_name++);
                    if (name.size() > limits.max_effective_texture_name_bytes)
                        return effective_failure(Status::resource_limit, item.request_index, request.grant_id,
                                                 request.binding.file, "external_texture_effective_name_limit",
                                                 "Synthetic effective texture name exceeds "
                                                 "the configured limit");
                } while (!texture_names.emplace(scope, canonical_effective_slot(name)).second);
                apex::formats::Kn5Texture texture;
                texture.active = true;
                texture.name = name;
                texture.size = static_cast<std::uint32_t>(resource.source_bytes.size());
                texture.data = resource.source_bytes;
                texture.workspaceFileIndex = material.workspaceFileIndex;
                input->model.textures.push_back(std::move(texture));
                texture_index = input->model.textures.size() - 1U;
                external_synthetic_indices.emplace(resource_key, texture_index);
            }

            auto& effective_material = input->model.materials[item.material_index];
            std::size_t matching_resource = invalid_external_texture_resource;
            for (std::size_t resource_index = 0U; resource_index < effective_material.resources.size();
                 ++resource_index) {
                if (canonical_effective_slot(effective_material.resources[resource_index].slot) ==
                    item.canonical_slot) {
                    if (matching_resource != invalid_external_texture_resource)
                        return effective_failure(Status::ambiguous, item.request_index, request.grant_id,
                                                 request.binding.file, "external_texture_material_resource_collision",
                                                 "Material contains colliding resource slots");
                    matching_resource = resource_index;
                }
            }
            if (matching_resource == invalid_external_texture_resource) {
                if (!request.binding.bind_point.has_value())
                    return effective_failure(Status::invalid_request, item.request_index, request.grant_id,
                                             request.binding.file, "external_texture_bind_point_missing",
                                             "A new material resource requires an explicit bind point");
                effective_material.resources.push_back(
                    {request.binding.slot, *request.binding.bind_point, input->model.textures[texture_index].name});
            } else {
                // The serialized KN5 bind point remains authoritative for an
                // existing slot; only its owned texture payload is replaced.
                effective_material.resources[matching_resource].texture = input->model.textures[texture_index].name;
            }

            auto& effective_overrides = input->overrides_by_material[item.material_index];
            effective_overrides.resources.erase(item.raw_key);
            input->external_bindings.push_back({item.material_index, material.workspaceFileIndex, request.binding.slot,
                                                resource.grant_id, resource.requested_path, resource.resolved_path,
                                                input->model.textures[texture_index].name, authority_index});
        }

        std::map<std::pair<ScopeKey, std::array<float, 4>>, std::size_t> solid_synthetic_indices;
        std::size_t next_solid_name = 0U;
        for (const SolidColorOverride& item : solid_colors) {
            const auto& material = input->model.materials[item.material_index];
            const ScopeKey scope{material.workspaceFileIndex.has_value() ? 1U : 0U, material.workspaceFileIndex};
            const auto color_key = std::make_pair(scope, item.color);
            const auto found_synthetic = solid_synthetic_indices.find(color_key);
            std::size_t texture_index = invalid_external_texture_resource;
            if (found_synthetic != solid_synthetic_indices.end()) {
                texture_index = found_synthetic->second;
            } else {
                if (!checked_add(solid_color_dds_bytes,
                                 effective_source_bytes,
                                 limits.max_total_effective_source_bytes))
                    return effective_failure(Status::resource_limit, invalid_external_texture_resource, {},
                                             item.raw_key, "solid_color_effective_source_limit",
                                             "Owned solid-color DDS bytes exceed the aggregate limit");
                std::vector<std::uint8_t> bytes =
                    make_solid_color_bgra8_dds(item.color);
                if (input->model.textures.size() >= limits.max_effective_textures ||
                    bytes.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
                    return effective_failure(Status::resource_limit, invalid_external_texture_resource, {},
                                             item.raw_key, "solid_color_effective_texture_limit",
                                             "Synthetic solid-color texture exceeds the configured limit");
                std::string name;
                do {
                    name = "__apex_solid_color_" + std::to_string(next_solid_name++);
                    if (name.size() > limits.max_effective_texture_name_bytes)
                        return effective_failure(Status::resource_limit, invalid_external_texture_resource, {},
                                                 item.raw_key, "solid_color_effective_name_limit",
                                                 "Synthetic solid-color texture name exceeds the configured "
                                                 "limit");
                } while (!texture_names.emplace(scope, canonical_effective_slot(name)).second);
                apex::formats::Kn5Texture texture;
                texture.active = true;
                texture.name = name;
                texture.size = static_cast<std::uint32_t>(bytes.size());
                texture.data = std::move(bytes);
                texture.workspaceFileIndex = material.workspaceFileIndex;
                input->model.textures.push_back(std::move(texture));
                texture_index = input->model.textures.size() - 1U;
                solid_synthetic_indices.emplace(color_key, texture_index);
            }

            auto& effective_material = input->model.materials[item.material_index];
            if (item.matching_resource == invalid_external_texture_resource) {
                effective_material.resources.push_back(
                    {item.raw_key, *item.bind_point, input->model.textures[texture_index].name});
            } else {
                effective_material.resources[item.matching_resource].texture =
                    input->model.textures[texture_index].name;
            }
            auto& effective_overrides = input->overrides_by_material[item.material_index];
            effective_overrides.resources.erase(item.raw_key);
            input->solid_color_bindings.push_back({item.material_index, material.workspaceFileIndex, item.raw_key,
                                                   item.color, input->model.textures[texture_index].name});
        }

        ExternalTextureEffectiveStockSceneResult result;
        result.status = Status::ready;
        result.input = std::move(input);
        return result;
    } catch (const std::bad_alloc&) {
        return effective_failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                                 "external_texture_effective_allocation_failed",
                                 "Effective stock-scene input exceeded bounded host storage");
    } catch (const std::length_error&) {
        return effective_failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                                 "external_texture_effective_allocation_failed",
                                 "Effective stock-scene input exceeded bounded host storage");
    }
} catch (const std::bad_alloc&) {
    return effective_failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                             "external_texture_effective_allocation_failed",
                             "Effective stock-scene input exceeded bounded host storage");
} catch (const std::length_error&) {
    return effective_failure(Status::resource_limit, invalid_external_texture_resource, {}, {},
                             "external_texture_effective_allocation_failed",
                             "Effective stock-scene input exceeded bounded host storage");
}

} // namespace apex::render
