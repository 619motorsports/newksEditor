#pragma once

#include <cstddef>
#include <cstdlib>
#include <string>

namespace apex::tests {

[[nodiscard]] inline std::string environment_value(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0U;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        std::free(value);
        return {};
    }
    std::string result(value, length == 0U ? 0U : length - 1U);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

} // namespace apex::tests
