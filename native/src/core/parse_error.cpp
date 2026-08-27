#include "apex/core/parse_error.hpp"

#include <iomanip>
#include <sstream>

namespace apex::core {

ParseError::ParseError(std::string format, std::string source, std::size_t offset,
                       std::string code, std::string message)
    : std::runtime_error([&] {
          std::ostringstream stream;
          stream << format << ": " << message << " (at 0x" << std::hex << offset << ")";
          if (!source.empty()) stream << " in " << source;
          return stream.str();
      }()),
      format_(std::move(format)), source_(std::move(source)), offset_(offset),
      code_(std::move(code)) {}

}
