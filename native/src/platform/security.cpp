#include "apex/platform/security.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace apex::platform {
namespace {

[[nodiscard]] bool containsNul(const std::string_view value) noexcept {
  return value.find('\0') != std::string_view::npos;
}

[[nodiscard]] bool asciiEqualsIgnoreCase(const std::string_view value,
                                          const std::string_view expected) noexcept {
  if (value.size() != expected.size()) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char actual = value[index] >= 'A' && value[index] <= 'Z'
                            ? static_cast<char>(value[index] - 'A' + 'a')
                            : value[index];
    const char wanted = expected[index] >= 'A' && expected[index] <= 'Z'
                            ? static_cast<char>(expected[index] - 'A' + 'a')
                            : expected[index];
    if (actual != wanted) return false;
  }
  return true;
}

[[nodiscard]] bool isWindowsDeviceName(const std::string_view component) noexcept {
  const std::size_t dot = component.find('.');
  std::size_t stemLength = dot == std::string_view::npos ? component.size() : dot;
  while (stemLength > 0U &&
         (component[stemLength - 1U] == ' ' || component[stemLength - 1U] == '.'))
    --stemLength;
  const std::string_view stem = component.substr(0, stemLength);
  if (asciiEqualsIgnoreCase(stem, "CON") || asciiEqualsIgnoreCase(stem, "PRN") ||
      asciiEqualsIgnoreCase(stem, "AUX") || asciiEqualsIgnoreCase(stem, "NUL") ||
      asciiEqualsIgnoreCase(stem, "CONIN$") || asciiEqualsIgnoreCase(stem, "CONOUT$"))
    return true;
  if (stem.size() != 4U ||
      !(asciiEqualsIgnoreCase(stem.substr(0, 3U), "COM") ||
        asciiEqualsIgnoreCase(stem.substr(0, 3U), "LPT")))
    return false;
  return stem.back() >= '1' && stem.back() <= '9';
}

[[nodiscard]] bool parseIpv4Octet(const std::string_view value, std::uint8_t& output) noexcept {
  if (value.empty() || value.size() > 3 || (value.size() > 1 && value.front() == '0')) return false;
  unsigned number = 0;
  for (const char character : value) {
    if (character < '0' || character > '9') return false;
    number = number * 10U + static_cast<unsigned>(character - '0');
    if (number > 255U) return false;
  }
  output = static_cast<std::uint8_t>(number);
  return true;
}

[[nodiscard]] bool isLoopbackIpv4(const std::string_view host) noexcept {
  std::array<std::uint8_t, 4> octets{};
  std::size_t component = 0;
  std::size_t start = 0;
  while (start <= host.size()) {
    if (component == octets.size()) return false;
    const std::size_t end = host.find('.', start);
    const std::size_t length = end == std::string_view::npos ? host.size() - start : end - start;
    if (!parseIpv4Octet(host.substr(start, length), octets[component])) return false;
    ++component;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return component == octets.size() && octets[0] == 127;
}

[[nodiscard]] int hexValue(const char character) noexcept {
  if (character >= '0' && character <= '9') return character - '0';
  if (character >= 'a' && character <= 'f') return character - 'a' + 10;
  if (character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

[[nodiscard]] bool parseIpv6Part(const std::string_view part,
                                 std::array<std::uint16_t, 8>& groups,
                                 std::size_t& count) noexcept {
  if (part.empty()) return true;
  std::size_t start = 0;
  while (start < part.size()) {
    if (count >= groups.size()) return false;
    const std::size_t end = part.find(':', start);
    const std::size_t length = end == std::string_view::npos ? part.size() - start : end - start;
    if (length == 0 || length > 4) return false;
    std::uint16_t value = 0;
    for (const char character : part.substr(start, length)) {
      const int digit = hexValue(character);
      if (digit < 0) return false;
      value = static_cast<std::uint16_t>(
          (static_cast<std::uint32_t>(value) << 4U) | static_cast<std::uint32_t>(digit));
    }
    groups[count++] = value;
    if (end == std::string_view::npos) break;
    start = end + 1;
    if (start == part.size()) return false;
  }
  return true;
}

[[nodiscard]] bool isLoopbackIpv6(const std::string_view host) noexcept {
  const std::size_t compression = host.find("::");
  std::array<std::uint16_t, 8> groups{};

  if (compression == std::string_view::npos) {
    std::size_t count = 0;
    if (!parseIpv6Part(host, groups, count) || count != groups.size()) return false;
  } else {
    const std::string_view right = host.substr(compression + 2);
    if (right.find("::") != std::string_view::npos) return false;
    std::array<std::uint16_t, 8> leftGroups{};
    std::array<std::uint16_t, 8> rightGroups{};
    std::size_t leftCount = 0;
    std::size_t rightCount = 0;
    if (!parseIpv6Part(host.substr(0, compression), leftGroups, leftCount) ||
        !parseIpv6Part(right, rightGroups, rightCount) ||
        leftCount + rightCount >= groups.size()) return false;
    for (std::size_t index = 0; index < leftCount; ++index) groups[index] = leftGroups[index];
    for (std::size_t index = 0; index < rightCount; ++index) {
      groups[groups.size() - rightCount + index] = rightGroups[index];
    }
  }

  for (std::size_t index = 0; index + 1 < groups.size(); ++index) {
    if (groups[index] != 0) return false;
  }
  return groups.back() == 1;
}

} // namespace

bool isLoopbackAddress(const std::string_view host) noexcept {
  if (host.empty() || containsNul(host)) return false;
  if (host.find(':') == std::string_view::npos) return isLoopbackIpv4(host);
  return isLoopbackIpv6(host);
}

bool isCapabilityRelativePath(const std::string_view path) noexcept {
  if (path.empty() || containsNul(path) || path.front() == '/' || path.front() == '\\') return false;

  std::size_t start = 0;
  while (start < path.size()) {
    const std::size_t end = path.find_first_of("/\\", start);
    const std::size_t length = end == std::string_view::npos ? path.size() - start : end - start;
    const std::string_view component = path.substr(start, length);
    if (component.empty() || component == "." || component == ".." ||
        component.find(':') != std::string_view::npos || isWindowsDeviceName(component)) return false;
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
  return false;
}

} // namespace apex::platform
