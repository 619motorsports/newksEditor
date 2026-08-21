#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace apex::domain {

struct SkinMetadataLimits {
    std::size_t maxBytes = 1U * 1024U * 1024U;
    std::size_t maxDepth = 32;
    std::size_t maxMembers = 100'000;
    std::size_t maxArrayItems = 100'000;
    std::size_t maxValues = 200'000;
    std::size_t maxStringBytes = 1U * 1024U * 1024U;
    std::size_t maxWarnings = 100'000;
    std::size_t maxOutputBytes = 1U * 1024U * 1024U;
};

class SkinMetadataError final : public std::runtime_error {
public:
    SkinMetadataError(std::string message, std::size_t offset = 0,
                      std::string code = "INVALID_JSON");
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::size_t offset_ = 0;
    std::string code_;
};

struct SkinJsonValue {
    using Pointer = std::shared_ptr<SkinJsonValue>;
    enum class Kind { null_value, boolean, number, string, array, object };
    Kind kind = Kind::null_value;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Pointer> array;
    std::vector<std::pair<std::string, Pointer>> object;
};

struct SkinMetadataFields {
    std::string skinname;
    std::string drivername;
    std::string country;
    std::string team;
    std::string number;
    std::optional<std::uint64_t> priority;
};

struct SkinMetadataEdit {
    std::optional<std::string> skinname;
    std::optional<std::string> drivername;
    std::optional<std::string> country;
    std::optional<std::string> team;
    std::optional<std::string> number;
    std::optional<std::uint64_t> priority;
};

struct SkinMetadata {
    std::string source;
    std::size_t byte_length = 0;
    SkinMetadataFields metadata;
    // Unknown members retain insertion order; duplicate JSON names follow
    // JavaScript assignment semantics (replace in the original position).
    // Number spelling is normalized through binary64 when serialized.
    SkinJsonValue original;
    std::vector<std::string> warnings;
};

[[nodiscard]] SkinMetadata parse_skin_metadata(
    std::string_view text, std::string source = "ui_skin.json",
    SkinMetadataLimits limits = {});
[[nodiscard]] SkinMetadata parse_skin_metadata(
    std::span<const std::uint8_t> bytes, std::string source = "ui_skin.json",
    SkinMetadataLimits limits = {});
[[nodiscard]] SkinMetadata parse_skin_metadata(
    std::span<const std::byte> bytes, std::string source = "ui_skin.json",
    SkinMetadataLimits limits = {});

[[nodiscard]] SkinMetadata create_skin_metadata(
    std::string source = "ui_skin.json");
[[nodiscard]] std::optional<SkinMetadataEdit> normalize_skin_metadata_edit(
    const SkinMetadataEdit& edit, SkinMetadataLimits limits = {});
[[nodiscard]] SkinMetadataFields effective_skin_metadata(
    const SkinMetadata& parsed, const SkinMetadataEdit& edit = {},
    SkinMetadataLimits limits = {});
[[nodiscard]] std::string serialize_skin_metadata(
    const SkinMetadata& parsed, const SkinMetadataEdit& edit = {},
    SkinMetadataLimits limits = {});

}  // namespace apex::domain
