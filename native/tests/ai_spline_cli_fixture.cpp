#include "apex/formats/ai_spline.hpp"
#include "apex/formats/ai_spline_write.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void appendF32(std::vector<std::uint8_t>& output, float value) {
    appendU32(output, std::bit_cast<std::uint32_t>(value));
}

void writeFile(const std::filesystem::path& path,
               std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create AI spline fixture");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot write AI spline fixture");
}

apex::formats::AiSpline version7Fixture() {
    apex::formats::AiSpline spline;
    spline.source = "cli-save-fixture.ai";
    spline.version = 7U;
    spline.lapTime = 123U;
    spline.points = {
        apex::formats::AiSplinePoint{{0.0F, 0.0F, 0.0F}, 0.0F, 0},
        apex::formats::AiSplinePoint{{10.0F, 0.0F, 0.0F}, 10.0F, 1},
        apex::formats::AiSplinePoint{{10.0F, 0.0F, 10.0F}, 20.0F, 2},
        apex::formats::AiSplinePoint{{0.0F, 0.0F, 10.0F}, 30.0F, 3},
    };
    for (std::uint32_t index = 0U; index < spline.points.size(); ++index) {
        apex::formats::AiSplinePayload payload;
        payload.speed = 50.0F + static_cast<float>(index);
        payload.radius = 5.0F;
        payload.side0 = 2.0F;
        payload.side1 = 2.0F;
        payload.normal = {0.0F, 1.0F, 0.0F};
        payload.forward = index == 0U
                              ? std::array<float, 3U>{1.0F, 0.0F, 0.0F}
                              : std::array<float, 3U>{0.0F, 0.0F, 1.0F};
        spline.payloads.push_back(payload);
    }
    return spline;
}

std::vector<std::uint8_t> version2Fixture() {
    std::vector<std::uint8_t> bytes;
    appendU32(bytes, 2U);
    appendU32(bytes, 1U);
    appendU32(bytes, 456U);
    appendF32(bytes, 1.0F);
    appendF32(bytes, 2.0F);
    appendF32(bytes, 3.0F);
    appendU32(bytes, 0U);
    appendF32(bytes, 4.0F);
    appendF32(bytes, 5.0F);
    appendF32(bytes, 6.0F);
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    try {
        writeFile(argv[1],
                  apex::formats::serializeAiSpline(version7Fixture()));
        writeFile(argv[2], version2Fixture());
        auto truncated = version2Fixture();
        truncated.pop_back();
        writeFile(argv[3], truncated);
        return 0;
    } catch (const std::exception&) {
        return 1;
    }
}
