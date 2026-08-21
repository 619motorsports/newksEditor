#include "apex/render/decoded_dds_texture.hpp"

#include "apex/core/parse_error.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <new>
#include <string>
#include <utility>

namespace apex::render {

namespace {

DecodedDdsTexturePlanResult failure(TextureUploadStatus status,
                                    std::string code,
                                    std::string message,
                                    std::size_t offset,
                                    std::string source) {
    return {status,
            {std::move(code), std::move(message), offset, std::move(source)},
            {}};
}

bool unsupported_parse_code(const std::string& code) noexcept {
    return code == "GPU_REQUIRED" || code == "UNSUPPORTED_FORMAT" ||
           code == "UNSUPPORTED_LAYOUT";
}

std::string normalized_parse_code(const std::string& code) {
    std::string normalized;
    normalized.reserve(code.size());
    for (const char value : code)
        normalized.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(value))));
    return normalized;
}

}  // namespace

TextureUploadPlan DecodedDdsTexturePlan::make_upload_plan() const {
    TextureUploadPlan uploads;
    uploads.subresources.reserve(levels.size());
    for (std::size_t index = 0U; index < levels.size(); ++index) {
        const formats::DdsLevel& level = levels[index];
        uploads.subresources.push_back(
            {static_cast<std::uint32_t>(index), 0U, level.width, level.height,
             level.width * 4U,
             std::as_bytes(std::span<const std::uint8_t>(level.pixels))});
    }
    return uploads;
}

DecodedDdsTexturePlanResult plan_decoded_dds_texture(
    std::span<const std::uint8_t> bytes, std::string source,
    apex::core::ParseLimits limits) {
    try {
        const auto descriptor = formats::inspectDds(bytes, source, limits);
        if (!descriptor.has_value())
            return failure(TextureUploadStatus::invalid, "invalid_header",
                           "The DDS header is malformed or truncated", 0U,
                           std::move(source));

        DecodedDdsTexturePlan plan;
        plan.levels = formats::decodeDdsRgba(bytes, *descriptor, source, limits);
        if (plan.levels.empty())
            return failure(TextureUploadStatus::invalid, "empty_mip_chain",
                           "The decoded DDS contains no mip levels", 0U,
                           std::move(source));
        plan.description = {
            descriptor->width,
            descriptor->height,
            descriptor->mipCount,
            1U,
            descriptor->srgb ? TextureFormat::rgba8_srgb
                             : TextureFormat::rgba8_unorm,
            TextureUsage::sampled,
            TextureMemory::device_local,
            TextureMutability::immutable,
        };
        const TextureUploadPlan uploads = plan.make_upload_plan();
        Diagnostic diagnostic;
        const TextureStatus validation =
            validate_texture_description(plan.description, uploads, diagnostic);
        if (validation != TextureStatus::ready)
            return failure(validation == TextureStatus::unsupported
                               ? TextureUploadStatus::unsupported
                               : TextureUploadStatus::invalid,
                           std::move(diagnostic.code),
                           std::move(diagnostic.message), 0U,
                           std::move(source));
        return {TextureUploadStatus::ready, {}, std::move(plan)};
    } catch (const apex::core::ParseError& error) {
        return failure(unsupported_parse_code(error.code())
                           ? TextureUploadStatus::unsupported
                           : TextureUploadStatus::invalid,
                       normalized_parse_code(error.code()), error.what(),
                       error.offset(), error.source());
    } catch (const std::bad_alloc&) {
        return failure(TextureUploadStatus::invalid, "allocation_failed",
                       "DDS decode planning has insufficient memory for bounded storage", 0U,
                       std::move(source));
    } catch (const std::exception& error) {
        return failure(TextureUploadStatus::invalid, "decode_failed",
                       error.what(), 0U, std::move(source));
    }
}

}  // namespace apex::render
