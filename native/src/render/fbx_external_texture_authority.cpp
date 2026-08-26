#include "apex/render/fbx_external_texture_authority.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace apex::render {
namespace {

using Status = ExternalTextureAuthorityStatus;

struct DiagnosticBudget {
    std::size_t count = 0U;
    std::size_t bytes = 0U;
};

[[nodiscard]] bool checked_add(std::size_t left, std::size_t right,
                               std::size_t& output) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    output = left + right;
    return true;
}

[[nodiscard]] bool checked_multiply(std::size_t left, std::size_t right,
                                    std::size_t& output) noexcept {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)
        return false;
    output = left * right;
    return true;
}

[[nodiscard]] bool add_diagnostic(
    FbxExternalTextureAuthorityResult& result, DiagnosticBudget& budget,
    const FbxExternalTextureAuthorityLimits& limits, std::size_t material,
    std::size_t candidate, std::string_view basename, std::string_view code,
    std::string_view message) {
    std::size_t bytes = 0U;
    if (!checked_add(basename.size(), code.size(), bytes) ||
        !checked_add(bytes, message.size(), bytes) ||
        budget.count >= limits.max_diagnostics ||
        bytes > limits.max_diagnostic_bytes ||
        budget.bytes > limits.max_diagnostic_bytes - bytes)
        return false;
    result.diagnostics.push_back(
        {material, candidate, std::string(basename), std::string(code),
         std::string(message)});
    ++budget.count;
    budget.bytes += bytes;
    return true;
}

[[nodiscard]] FbxExternalTextureAuthorityResult failure(
    Status status, std::string_view code, std::string_view message) {
    FbxExternalTextureAuthorityResult result;
    result.status = status;
    result.authority.status = status;
    result.diagnostics.push_back(
        {invalid_external_texture_resource, invalid_fbx_texture_selection, {},
         std::string(code), std::string(message)});
    return result;
}

[[nodiscard]] bool valid_basename(std::string_view value,
                                  std::size_t max_bytes) noexcept {
    if (value.empty() || value.size() > max_bytes || value == "." ||
        value == "..")
        return false;
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '/' || character == '\\' || character == ':' ||
            byte == 0U || byte < 0x20U || byte == 0x7fU)
            return false;
    }
    return true;
}

[[nodiscard]] MaterialTextureBinding diffuse_binding(std::string basename) {
    MaterialTextureBinding binding;
    binding.slot = "txDiffuse";
    binding.kind = MaterialTextureKind::external_file;
    binding.bind_point = 0U;
    binding.file = std::move(basename);
    return binding;
}

} // namespace

FbxExternalTextureAuthorityResult resolve_fbx_external_texture_authority(
    const apex::formats::FbxSceneConversion& conversion, std::string grant_id,
    const apex::assets::AssetSource& source,
    FbxExternalTextureAuthorityLimits limits) try {
    if (limits.max_candidates == 0U ||
        limits.max_candidates_per_material == 0U ||
        limits.max_basename_bytes == 0U || limits.max_metadata_bytes == 0U ||
        limits.max_diagnostics == 0U || limits.max_diagnostic_bytes < 256U)
        return failure(Status::invalid_request, "fbx_texture_limits_invalid",
                       "FBX texture authority limits must be nonzero");
    if (grant_id.empty() ||
        grant_id.size() > limits.authority.max_grant_id_bytes ||
        grant_id.find('\0') != std::string::npos)
        return failure(Status::invalid_request, "fbx_texture_grant_id_invalid",
                       "FBX texture grant identity is invalid");

    const auto material_count = conversion.snapshot.materials.size();
    const auto candidate_count = conversion.file_texture_candidates.size();
    if (material_count > limits.authority.max_materials)
        return failure(Status::resource_limit, "fbx_texture_material_limit",
                       "FBX material count exceeds the configured limit");
    if (candidate_count > limits.max_candidates)
        return failure(Status::resource_limit, "fbx_texture_candidate_limit",
                       "FBX texture candidate count exceeds the configured limit");

    std::size_t metadata_bytes = 0U;
    std::size_t amount = 0U;
    if (!checked_multiply(material_count, sizeof(std::size_t) * 2U, amount) ||
        !checked_add(metadata_bytes, amount, metadata_bytes) ||
        !checked_multiply(candidate_count,
                          sizeof(std::size_t) +
                              sizeof(FbxExternalTextureSelection) +
                              sizeof(ExternalTextureRequest),
                          amount) ||
        !checked_add(metadata_bytes, amount, metadata_bytes))
        return failure(Status::resource_limit,
                       "fbx_texture_metadata_overflow",
                       "FBX texture metadata size overflows");

    std::vector<std::size_t> order(candidate_count);
    std::iota(order.begin(), order.end(), 0U);
    for (std::size_t index = 0U; index < candidate_count; ++index) {
        const auto& candidate = conversion.file_texture_candidates[index];
        if (candidate.material >= material_count || candidate.texture_object_id <= 0)
            return failure(Status::invalid_request,
                           "fbx_texture_candidate_identity_invalid",
                           "FBX texture candidate identity is invalid");
        if (!apex::formats::fbxNativeFileTextureChannelRank(candidate.channel)
                 .has_value())
            return failure(Status::invalid_request,
                           "fbx_texture_channel_invalid",
                           "FBX texture candidate channel is not native");
        if (!valid_basename(candidate.basename, limits.max_basename_bytes))
            return failure(Status::rejected, "fbx_texture_basename_rejected",
                           "FBX texture candidate basename is unsafe");
        if (!checked_add(candidate.basename.size(), candidate.channel.size(),
                         amount) ||
            !checked_add(metadata_bytes, amount, metadata_bytes) ||
            metadata_bytes > limits.max_metadata_bytes)
            return failure(Status::resource_limit,
                           "fbx_texture_metadata_limit",
                           "FBX texture metadata exceeds the configured limit");
    }
    if (metadata_bytes > limits.max_metadata_bytes)
        return failure(Status::resource_limit, "fbx_texture_metadata_limit",
                       "FBX texture metadata exceeds the configured limit");

    std::sort(order.begin(), order.end(), [&](std::size_t left_index,
                                               std::size_t right_index) {
        const auto& left = conversion.file_texture_candidates[left_index];
        const auto& right = conversion.file_texture_candidates[right_index];
        return std::tuple{
                   left.material,
                   *apex::formats::fbxNativeFileTextureChannelRank(left.channel),
                   left.connection_order, left.texture_object_id, left_index} <
               std::tuple{
                   right.material,
                   *apex::formats::fbxNativeFileTextureChannelRank(right.channel),
                   right.connection_order, right.texture_object_id, right_index};
    });

    FbxExternalTextureAuthorityResult result;
    result.status = Status::ready;
    result.authority.status = Status::ready;
    result.material_selection_indices.assign(material_count,
                                             invalid_fbx_texture_selection);
    std::vector<ExternalTextureRequest> requests;
    requests.reserve(std::min(material_count, candidate_count));
    result.selections.reserve(std::min(material_count, candidate_count));
    DiagnosticBudget diagnostic_budget;

    std::size_t begin = 0U;
    while (begin < order.size()) {
        const auto material =
            conversion.file_texture_candidates[order[begin]].material;
        std::size_t end = begin;
        while (end < order.size() &&
               conversion.file_texture_candidates[order[end]].material == material)
            ++end;
        if (end - begin > limits.max_candidates_per_material)
            return failure(Status::resource_limit,
                           "fbx_texture_material_candidate_limit",
                           "FBX material texture candidate count exceeds its limit");

        bool selected = false;
        std::string_view last_missing;
        std::size_t last_missing_index = invalid_fbx_texture_selection;
        std::optional<std::pair<std::size_t, std::size_t>> previous_order;
        for (std::size_t offset = begin; offset < end; ++offset) {
            const auto candidate_index = order[offset];
            const auto& candidate =
                conversion.file_texture_candidates[candidate_index];
            const auto channel_rank =
                *apex::formats::fbxNativeFileTextureChannelRank(candidate.channel);
            const auto source_order =
                std::pair{channel_rank, candidate.connection_order};
            if (previous_order.has_value() && *previous_order == source_order)
                return failure(Status::invalid_request,
                               "fbx_texture_candidate_order_duplicate",
                               "FBX texture candidates duplicate one source order");
            previous_order = source_order;
            if (selected) continue;

            const auto resolved = source.resolve(candidate.basename);
            if (resolved.status == apex::assets::AssetResolveStatus::missing) {
                last_missing = candidate.basename;
                last_missing_index = candidate_index;
                continue;
            }
            if (resolved.status == apex::assets::AssetResolveStatus::ambiguous) {
                FbxExternalTextureAuthorityResult failed = failure(
                    Status::ambiguous, "fbx_texture_candidate_ambiguous",
                    "FBX texture basename matches more than one authorized file");
                failed.diagnostics.front().material_index = material;
                failed.diagnostics.front().candidate_index = candidate_index;
                failed.diagnostics.front().basename = candidate.basename;
                return failed;
            }
            if (resolved.status != apex::assets::AssetResolveStatus::resolved ||
                resolved.file == nullptr) {
                FbxExternalTextureAuthorityResult failed = failure(
                    Status::rejected, "fbx_texture_candidate_rejected",
                    "FBX texture basename was rejected by the asset source");
                failed.diagnostics.front().material_index = material;
                failed.diagnostics.front().candidate_index = candidate_index;
                failed.diagnostics.front().basename = candidate.basename;
                return failed;
            }

            if (requests.size() >= limits.authority.max_requests)
                return failure(Status::resource_limit,
                               "fbx_texture_request_limit",
                               "FBX texture request count exceeds the configured limit");
            result.material_selection_indices[material] = result.selections.size();
            result.selections.push_back(
                {material, candidate_index, candidate.texture_object_id,
                 candidate.channel, candidate.basename,
                 invalid_external_texture_resource});
            requests.push_back(
                {grant_id, diffuse_binding(candidate.basename), material, {}});
            selected = true;
        }
        if (!selected && last_missing_index != invalid_fbx_texture_selection) {
            (void)add_diagnostic(
                result, diagnostic_budget, limits, material,
                last_missing_index, last_missing, "fbx_texture_material_missing",
                "No authorized file exists for this FBX material texture");
        }
        begin = end;
    }

    const std::array<ExternalTextureGrant, 1U> grants = {
        ExternalTextureGrant{std::move(grant_id), &source}};
    result.authority = resolve_external_texture_authority(
        grants, std::span<const ExternalTextureRequest>(requests),
        limits.authority);
    result.status = result.authority.status;
    if (!result.authority.ok()) {
        result.selections.clear();
        result.material_selection_indices.clear();
        return result;
    }
    if (result.authority.request_resource_indices.size() !=
        result.selections.size())
        return failure(Status::invalid_request,
                       "fbx_texture_authority_mapping_invalid",
                       "FBX texture authority returned an invalid mapping");
    for (std::size_t index = 0U; index < result.selections.size(); ++index)
        result.selections[index].authority_resource_index =
            result.authority.request_resource_indices[index];
    return result;
} catch (const std::bad_alloc&) {
    return failure(Status::resource_limit,
                   "fbx_texture_authority_allocation_failed",
                   "FBX texture authority exceeded bounded host storage");
} catch (const std::length_error&) {
    return failure(Status::resource_limit,
                   "fbx_texture_authority_allocation_failed",
                   "FBX texture authority exceeded bounded host storage");
}

} // namespace apex::render
