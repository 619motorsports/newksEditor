#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace apex::core {

class ParseError final : public std::runtime_error {
public:
    ParseError(std::string format, std::string source, std::size_t offset,
               std::string code, std::string message);

    [[nodiscard]] const std::string& format() const noexcept { return format_; }
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string format_;
    std::string source_;
    std::size_t offset_;
    std::string code_;
};

}
