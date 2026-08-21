#pragma once

#include "apex/core/parse_limits.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace apex::formats {

enum class FbxFormat { binary, ascii };

struct FbxHeader {
    FbxFormat format = FbxFormat::ascii;
    std::uint32_t version = 0;
};

enum class FbxStage { header, binary_dom, ascii_tokens, ascii_dom, conversion };

struct FbxConversionCapability {
    bool domSupported = true;
    bool sceneSupported = false;
    FbxStage stage = FbxStage::conversion;
    std::string code;
    std::string diagnostic;
};

struct FbxDiagnostic {
    FbxStage stage = FbxStage::header;
    std::string code;
    std::string message;
    std::size_t offset = 0;
};

class FbxError final : public std::runtime_error {
public:
    FbxError(FbxDiagnostic diagnostic)
        : std::runtime_error(diagnostic.message), diagnostic_(std::move(diagnostic)) {}

    [[nodiscard]] FbxStage stage() const noexcept { return diagnostic_.stage; }
    [[nodiscard]] const std::string& code() const noexcept { return diagnostic_.code; }
    [[nodiscard]] std::size_t offset() const noexcept { return diagnostic_.offset; }

private:
    FbxDiagnostic diagnostic_;
};

using FbxArray = std::variant<std::vector<std::uint8_t>, std::vector<std::int64_t>,
                              std::vector<float>, std::vector<double>>;
using FbxValue = std::variant<std::monostate, bool, std::int64_t, double, std::string,
                              std::vector<std::uint8_t>, FbxArray>;

struct FbxProperty {
    char code = 0;
    std::vector<FbxValue> values;
};

struct FbxNode {
    std::string name;
    std::vector<FbxProperty> properties;
    std::vector<FbxNode> children;
};

struct FbxLimits {
    apex::core::ParseLimits parse{};
    std::size_t maxDepth = 128;
    std::size_t maxNodes = 1'000'000;
    std::size_t maxProperties = 4'000'000;
    std::size_t maxPropertyBytes = 256u * 1024u * 1024u;
    std::size_t maxAggregateBytes = 256u * 1024u * 1024u;
    std::size_t maxArrayElements = 10'000'000;
    std::size_t maxArrayBytes = 256u * 1024u * 1024u;
    std::size_t maxAsciiTokens = 4'000'000;
    std::size_t maxAsciiTokenBytes = 1u * 1024u * 1024u;
    // Binary data after the required root null record is accepted only as an
    // opaque footer of at least 16 bytes, bounded by this limit.
    std::size_t maxFooterBytes = 1u * 1024u * 1024u;
};

struct FbxDocument {
    FbxHeader header;
    std::vector<FbxNode> roots;
    std::size_t bytesRead = 0;
    std::size_t trailingBytes = 0;
    std::vector<FbxDiagnostic> diagnostics;
};

[[nodiscard]] FbxHeader inspectFbxHeader(std::span<const std::uint8_t> bytes,
                                          std::string source = "scene.fbx");

[[nodiscard]] FbxConversionCapability fbxConversionCapability();

inline FbxConversionCapability fbx_conversion_capability() { return fbxConversionCapability(); }

inline FbxHeader inspectFbxHeader(std::span<const std::byte> bytes,
                                  std::string source = "scene.fbx") {
    return inspectFbxHeader({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()}, std::move(source));
}

[[nodiscard]] FbxDocument parseFbx(std::span<const std::uint8_t> bytes,
                                    std::string source = "scene.fbx",
                                    FbxLimits limits = {});

inline FbxDocument parseFbx(std::span<const std::byte> bytes,
                            std::string source = "scene.fbx", FbxLimits limits = {}) {
    return parseFbx({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()},
                    std::move(source), std::move(limits));
}

inline FbxDocument parse_fbx(std::span<const std::uint8_t> bytes,
                              std::string source = "scene.fbx", FbxLimits limits = {}) {
    return parseFbx(bytes, std::move(source), std::move(limits));
}

} // namespace apex::formats
