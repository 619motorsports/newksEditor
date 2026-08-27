#pragma once

#include "apex/core/parse_limits.hpp"
#include "apex/formats/kn5.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace apex::formats {

class Kn5WriteError final : public std::runtime_error {
public:
    Kn5WriteError(std::string code, std::string message);

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

// Serialize the public KN5 scene representation. The writer emits the
// v4/v5/v6 public layout only; CSP KN5ENC payloads and metadata-only textures
// are rejected because their protected/original bytes cannot be reconstructed.
[[nodiscard]] std::vector<std::uint8_t> serializeKn5(
    const Kn5File& file, apex::core::ParseLimits limits = {});

[[nodiscard]] inline std::vector<std::uint8_t> serialize_kn5(
    const Kn5File& file, apex::core::ParseLimits limits = {}) {
    return serializeKn5(file, limits);
}

} // namespace apex::formats
