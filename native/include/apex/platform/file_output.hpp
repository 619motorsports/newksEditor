#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

namespace apex::platform {

// Write through an exclusive sibling temporary file. Fail if the destination
// already exists. The temporary file is removed after an ordinary failure.
void writeFileExclusive(const std::filesystem::path& path,
                        std::span<const std::uint8_t> bytes);

void writeFileExclusive(const std::filesystem::path& path,
                        std::string_view text);

// Write through an exclusive sibling temporary file, then atomically replace
// the destination. Replacement can change file permissions or ACL metadata.
void writeFileAtomicReplace(const std::filesystem::path& path,
                            std::span<const std::uint8_t> bytes);

}  // namespace apex::platform
