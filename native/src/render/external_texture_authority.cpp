#include "apex/render/external_texture_authority.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <map>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace apex::render {
namespace {

using Diagnostic = ExternalTextureAuthorityDiagnostic;
using Status = ExternalTextureAuthorityStatus;

[[nodiscard]] ExternalTextureAuthorityResult failure(
    Status status, std::size_t request_index, std::string grant_id,
    std::string path, std::string code, std::string message) {
    ExternalTextureAuthorityResult result;
    result.status = status;
    result.diagnostics.push_back({request_index, std::move(grant_id), std::move(path),
                                  std::move(code), std::move(message)});
    return result;
}

void add_diagnostic(ExternalTextureAuthorityResult& result, Status status,
                    std::size_t request_index, std::string grant_id,
                    std::string path, std::string code, std::string message) {
    if (result.diagnostics.empty()) result.status = status;
    result.diagnostics.push_back({request_index, std::move(grant_id), std::move(path),
                                  std::move(code), std::move(message)});
}

[[nodiscard]] bool valid_limits(const ExternalTextureAuthorityLimits& limits) noexcept {
    return limits.max_requests != 0U && limits.max_grants != 0U &&
           limits.max_grant_id_bytes != 0U && limits.max_path_bytes != 0U &&
           limits.max_file_bytes != 0U && limits.max_mip_levels != 0U &&
           limits.max_total_source_bytes != 0U &&
           limits.max_total_decoded_bytes != 0U &&
           limits.max_total_plan_bytes != 0U && limits.decode.maxInputBytes != 0U &&
           limits.decode.maxOutputBytes != 0U;
}

[[nodiscard]] bool checked_add(std::uint64_t value, std::uint64_t& total,
                               std::uint64_t limit) noexcept {
    if (value > limit || total > limit - value) return false;
    total += value;
    return true;
}

[[nodiscard]] bool checked_size_to_u64(std::size_t value, std::uint64_t& output) noexcept {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
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

}  // namespace

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
            const auto planned = plan_decoded_dds_texture(
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
            std::uint64_t plan_bytes = sizeof(DecodedDdsTexturePlan);
            std::uint64_t level_metadata = 0U;
            if (!checked_size_to_u64(planned.plan.levels.size(), level_metadata) ||
                level_metadata > std::numeric_limits<std::uint64_t>::max() /
                                     sizeof(formats::DdsLevel) ||
                !checked_add(level_metadata * sizeof(formats::DdsLevel), plan_bytes,
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
                                        std::move(planned.plan)});
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

}  // namespace apex::render
