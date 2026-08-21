#include "apex/platform/security.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

void testLoopbackAddresses() {
  using apex::platform::isLoopbackAddress;
  for (const std::string_view address : {
           "127.0.0.1", "127.1.2.3", "127.255.255.255", "::1",
           "0:0:0:0:0:0:0:1", "0000:0000:0000:0000:0000:0000:0000:0001"}) {
    require(isLoopbackAddress(address), "valid loopback address rejected");
  }

  for (const std::string_view address : {
           "", "localhost", "127.0.0.1:4173", "127.0.0.01", "126.0.0.1",
           "128.0.0.1", "0.0.0.0", "::", "::2", "0:0:0:0:0:0:1:1",
           "::ffff:127.0.0.1", "[::1]", "::1%lo", "1::1", ":::1", "1:::1",
           "gggg::1", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9"}) {
    require(!isLoopbackAddress(address), "non-loopback address accepted");
  }

  const std::string embeddedNul("127.0.0.1\0evil", 14);
  require(!isLoopbackAddress(embeddedNul), "NUL-containing address accepted");
}

void testCapabilityRelativePaths() {
  using apex::platform::isCapabilityRelativePath;
  for (const std::string_view path : {
           "car/body.kn5", "data\\lods.ini", "textures/skin.dds", "model.kn5"}) {
    require(isCapabilityRelativePath(path), "valid capability path rejected");
  }

  for (const std::string_view path : {
           "", "/etc/passwd", "\\Windows\\system32", "//server/share/file",
           "\\\\server\\share\\file", "C:/asset.kn5", "C:\\asset.kn5", "C:asset.kn5",
           "car/../outside.kn5", "car/./body.kn5", "car//body.kn5", "car\\\\body.kn5",
           "car/", "car\\", "/", ".", "..", "car:stream", "./car.kn5"}) {
    require(!isCapabilityRelativePath(path), "unsafe capability path accepted");
  }

  const std::string embeddedNul("car/body\0evil.kn5", 17);
  require(!isCapabilityRelativePath(embeddedNul), "NUL-containing path accepted");
}

} // namespace

int main() {
  try {
    testLoopbackAddresses();
    testCapabilityRelativePaths();
    std::cout << "platform security tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "platform security tests failed: " << error.what() << '\n';
    return 1;
  }
}
