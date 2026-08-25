#include "apex/formats/jpeg.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#if defined(APEX_HAS_JPEG)
#include <jpeglib.h>
#endif

namespace apex::formats {
namespace {

using apex::core::ParseError;

[[nodiscard]] ParseError jpegError(std::string_view source, std::size_t offset,
                                   std::string_view code,
                                   std::string_view message) {
    return ParseError("JPEG", std::string(source), offset, std::string(code),
                      std::string(message));
}

#if defined(APEX_HAS_JPEG)
struct JpegErrorManager {
    jpeg_error_mgr base{};
    std::jmp_buf jump{};
    char message[JMSG_LENGTH_MAX]{};
};

void jpegErrorExit(j_common_ptr info) {
    auto* error = reinterpret_cast<JpegErrorManager*>(info->err);
    (*info->err->format_message)(info, error->message);
    longjmp(error->jump, 1);
}
#endif

}  // namespace

JpegImage decodeJpegRgba8(std::span<const std::uint8_t> bytes,
                          std::string source, JpegLimits limits) {
#if !defined(APEX_HAS_JPEG)
    (void)bytes;
    (void)limits;
    throw jpegError(source, 0u, "UNSUPPORTED_FORMAT",
                    "JPEG decoding is unavailable in this native build");
#else
    if (limits.parse.maxInputBytes == 0u || limits.parse.maxOutputBytes == 0u ||
        limits.maxDimension == 0u)
        throw jpegError(source, 0u, "LIMIT", "JPEG limits must be nonzero");
    if (bytes.size() > limits.parse.maxInputBytes)
        throw jpegError(source, 0u, "INPUT_TOO_LARGE",
                        "JPEG input exceeds configured limit");
    if (bytes.size() < 3u)
        throw jpegError(source, 0u, "TRUNCATED", "JPEG signature is truncated");
    if (bytes[0] != 0xffu || bytes[1] != 0xd8u || bytes[2] != 0xffu)
        throw jpegError(source, 0u, "SIGNATURE", "JPEG signature is invalid");
    const auto hasEndMarker = [&] {
        for (std::size_t index = 2u; index < bytes.size(); ++index)
            if (bytes[index - 1u] == 0xffu && bytes[index] == 0xd9u) return true;
        return false;
    };
    if (!hasEndMarker())
        throw jpegError(source, bytes.size(), "TRUNCATED",
                        "JPEG end marker is truncated");

    jpeg_decompress_struct info{};
    JpegErrorManager error{};
    info.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpegErrorExit;
    bool created = false;
    std::uint8_t* rawPixels = nullptr;
    std::uint8_t* row = nullptr;
    if (setjmp(error.jump) != 0) {
        const std::string message = error.message[0] == '\0'
                                        ? "JPEG decoder rejected the input"
                                        : error.message;
        if (rawPixels != nullptr) std::free(rawPixels);
        if (row != nullptr) std::free(row);
        if (created) jpeg_destroy_decompress(&info);
        throw jpegError(source, 0u, "DECODE", message);
    }

    jpeg_create_decompress(&info);
    created = true;
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
        jpeg_destroy_decompress(&info);
        created = false;
        throw jpegError(source, 0u, "INPUT_TOO_LARGE",
                        "JPEG input exceeds decoder address limits");
    }
    jpeg_mem_src(&info, const_cast<unsigned char*>(bytes.data()),
                 static_cast<unsigned long>(bytes.size()));
    const int header = jpeg_read_header(&info, TRUE);
    if (header != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&info);
        created = false;
        throw jpegError(source, 0u, "HEADER", "JPEG header is invalid");
    }
    if (info.data_precision != 8 ||
        (info.num_components != 1 && info.num_components != 3)) {
        jpeg_destroy_decompress(&info);
        created = false;
        throw jpegError(source, 0u, "UNSUPPORTED_FORMAT",
                        "JPEG requires 8-bit grayscale or RGB components");
    }
    if (info.progressive_mode) {
        jpeg_destroy_decompress(&info);
        created = false;
        throw jpegError(source, 0u, "UNSUPPORTED_FORMAT",
                        "Progressive JPEG images are unsupported");
    }
    if (info.image_width == 0u || info.image_height == 0u ||
        info.image_width > limits.maxDimension ||
        info.image_height > limits.maxDimension) {
        jpeg_destroy_decompress(&info);
        created = false;
        throw jpegError(source, 0u, "DIMENSION_LIMIT",
                        "JPEG dimensions are invalid or too large");
    }

    info.out_color_space = JCS_RGB;
    const auto width = static_cast<std::size_t>(info.image_width);
    const auto height = static_cast<std::size_t>(info.image_height);
    if ((width > std::numeric_limits<std::size_t>::max() / 3u) ||
        (width != 0u &&
         height > std::numeric_limits<std::size_t>::max() / width) ||
        (width != 0u &&
         height * width > std::numeric_limits<std::size_t>::max() / 4u)) {
        jpeg_destroy_decompress(&info);
        created = false;
        throw jpegError(source, 0u, "SIZE_OVERFLOW",
                        "JPEG pixel storage size overflows");
    }
    jpeg_start_decompress(&info);
    const auto rowBytes = static_cast<std::size_t>(info.output_width) * 3u;
    const auto pixelBytes = static_cast<std::size_t>(info.output_width) *
                            static_cast<std::size_t>(info.output_height) * 4u;
    if (pixelBytes > limits.parse.maxOutputBytes) {
        jpeg_destroy_decompress(&info);
        created = false;
        throw jpegError(source, 0u, "OUTPUT_TOO_LARGE",
                        "JPEG decoded pixels exceed configured limit");
    }
    rawPixels = static_cast<std::uint8_t*>(std::malloc(pixelBytes));
    if (rawPixels == nullptr && pixelBytes != 0u) {
        jpeg_destroy_decompress(&info);
        created = false;
        throw jpegError(source, 0u, "ALLOCATION_FAILED",
                        "JPEG decoded pixels exceed available memory");
    }
    row = static_cast<std::uint8_t*>(std::malloc(rowBytes));
    if (row == nullptr && rowBytes != 0u) {
        std::free(rawPixels);
        rawPixels = nullptr;
        jpeg_destroy_decompress(&info);
        created = false;
        throw jpegError(source, 0u, "ALLOCATION_FAILED",
                        "JPEG scanline exceeds available memory");
    }
    while (info.output_scanline < info.output_height) {
        JSAMPROW scanline = row;
        if (jpeg_read_scanlines(&info, &scanline, 1u) != 1u) {
            std::free(row);
            std::free(rawPixels);
            rawPixels = nullptr;
            jpeg_destroy_decompress(&info);
            throw jpegError(source, 0u, "DECODE", "JPEG scanline is truncated");
        }
        const auto y = static_cast<std::size_t>(info.output_scanline - 1u);
        for (std::size_t x = 0u; x < static_cast<std::size_t>(info.output_width); ++x) {
            const auto sourceOffset = x * 3u;
            const auto outputOffset = (y * static_cast<std::size_t>(info.output_width) + x) * 4u;
            rawPixels[outputOffset] = row[sourceOffset];
            rawPixels[outputOffset + 1u] = row[sourceOffset + 1u];
            rawPixels[outputOffset + 2u] = row[sourceOffset + 2u];
            rawPixels[outputOffset + 3u] = 255u;
        }
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    created = false;
    std::free(row);

    JpegImage result;
    result.width = info.output_width;
    result.height = info.output_height;
    result.bytesRead = bytes.size();
    result.pixels.assign(rawPixels, rawPixels + pixelBytes);
    std::free(rawPixels);
    return result;
#endif
}

}  // namespace apex::formats
