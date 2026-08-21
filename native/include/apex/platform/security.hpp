#pragma once

#include <string_view>

namespace apex::platform {

// Returns true only for a textual IPv4 or IPv6 loopback address.
// Host names, URL authorities (including bracketed IPv6), ports, and
// IPv4-compatible/mapped IPv6 addresses are intentionally not accepted.
[[nodiscard]] bool isLoopbackAddress(std::string_view host) noexcept;

// Returns true when path is safe to resolve beneath a capability-selected
// directory. Both slash styles are treated as separators on every platform,
// so a path cannot become absolute when it crosses operating systems.
// Drive/UNC roots, NUL bytes, empty components, and . or .. components are
// rejected. A colon is rejected anywhere to avoid drive and NTFS ADS syntax.
[[nodiscard]] bool isCapabilityRelativePath(std::string_view path) noexcept;

} // namespace apex::platform
