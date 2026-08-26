#include "apex/platform/file_output.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read test output");
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

bool hasTemporaryFile(const std::filesystem::path& root,
                      std::string_view outputName) {
    const auto prefix = std::string(outputName) + ".apex-tmp-";
    for (const auto& item : std::filesystem::directory_iterator(root)) {
        const auto name = item.path().filename().string();
        if (name.starts_with(prefix)) return true;
    }
    return false;
}

void coversExclusiveAndAtomicOutput(const std::filesystem::path& root) {
    const auto exclusive = root / "exclusive.bin";
    apex::platform::writeFileExclusive(exclusive, "first");
    bool rejected = false;
    try {
        apex::platform::writeFileExclusive(exclusive, "second");
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected && readFile(exclusive) == "first",
            "exclusive output preserves an existing destination");
    require(!hasTemporaryFile(root, "exclusive.bin"),
            "exclusive output removes temporary files");

    const auto replaced = root / "replaced.bin";
    apex::platform::writeFileExclusive(replaced, "old");
    const auto collision = root / "replaced.bin.apex-tmp-0";
    apex::platform::writeFileExclusive(collision, "collision");
    constexpr std::array<std::uint8_t, 4U> replacement = {
        0x00U, 0x01U, 0xfeU, 0xffU};
    apex::platform::writeFileAtomicReplace(replaced, replacement);
    require(readFile(replaced) == std::string("\0\1\xfe\xff", 4U),
            "atomic output replaces the destination with binary bytes");
    require(readFile(collision) == "collision",
            "atomic output skips a colliding temporary file");
    std::filesystem::remove(collision);
    require(!hasTemporaryFile(root, "replaced.bin"),
            "atomic output removes temporary files");
}

void keepsPromotionFailureAtomic(const std::filesystem::path& root) {
    const auto blocked = root / "blocked";
    std::filesystem::create_directory(blocked);
    constexpr std::array<std::uint8_t, 1U> byte = {0x42U};
    bool rejected = false;
    try {
        apex::platform::writeFileAtomicReplace(blocked, byte);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected && std::filesystem::is_directory(blocked),
            "promotion failure preserves the destination directory");
    require(!hasTemporaryFile(root, "blocked"),
            "promotion failure removes its temporary file");
}

}  // namespace

int main() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("apex-file-output-tests-" + std::to_string(suffix));
    try {
        std::filesystem::create_directories(root);
        coversExclusiveAndAtomicOutput(root);
        keepsPromotionFailureAtomic(root);
        std::filesystem::remove_all(root);
        std::cout << "file_output_tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << "file_output_tests: " << error.what() << '\n';
        return 1;
    }
}
