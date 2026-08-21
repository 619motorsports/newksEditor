#include "apex/render/external_texture_authority.hpp"

#include "apex/formats/acd.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::assets::AssetSource;
using apex::formats::AcdArchive;
using apex::formats::AcdEntry;
using apex::render::ExternalTextureAuthorityLimits;
using apex::render::ExternalTextureAuthorityStatus;
using apex::render::ExternalTextureGrant;
using apex::render::ExternalTextureRequest;
using apex::render::MaterialTextureBinding;
using apex::render::MaterialTextureKind;
using apex::render::resolve_external_texture_authority;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    require(offset + 4U <= bytes.size(), "DDS fixture write is in bounds");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

std::vector<std::uint8_t> rgba8Dds(std::array<std::uint8_t, 4> pixel) {
    std::vector<std::uint8_t> bytes(152U, 0U);
    put32(bytes, 0U, 0x20534444U);  // DDS magic
    put32(bytes, 4U, 124U);         // header size
    put32(bytes, 12U, 1U);          // height
    put32(bytes, 16U, 1U);          // width
    put32(bytes, 20U, 4U);          // pitch
    put32(bytes, 28U, 1U);          // mip count
    put32(bytes, 76U, 32U);         // pixel format size
    put32(bytes, 80U, 0x40U);       // RGB flag
    put32(bytes, 88U, 32U);         // bits per pixel
    put32(bytes, 92U, 0x000000ffU);
    put32(bytes, 96U, 0x0000ff00U);
    put32(bytes, 100U, 0x00ff0000U);
    put32(bytes, 104U, 0xff000000U);
    for (std::size_t index = 0U; index < pixel.size(); ++index)
        bytes[128U + index] = pixel[index];
    return bytes;
}

std::filesystem::path testRoot(std::string_view suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("apex-external-texture-" + std::string(suffix) + "-" +
            std::to_string(static_cast<long long>(stamp)));
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create texture fixture");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot write texture fixture");
}

ExternalTextureRequest request(std::string grant, std::string file) {
    MaterialTextureBinding binding;
    binding.slot = "txDiffuse";
    binding.kind = MaterialTextureKind::external_file;
    binding.file = std::move(file);
    return {std::move(grant), std::move(binding)};
}

ExternalTextureRequest nonExternalRequest(std::string grant) {
    MaterialTextureBinding binding;
    binding.slot = "txDiffuse";
    binding.kind = MaterialTextureKind::embedded;
    return {std::move(grant), std::move(binding)};
}

AcdArchive archiveWith(std::string path, std::vector<std::uint8_t> bytes) {
    AcdArchive archive;
    archive.source = "fixture/data.acd";
    archive.assetName = "fixture";
    AcdEntry entry;
    entry.name = path;
    entry.path = path;
    entry.safe = true;
    entry.size = bytes.size();
    entry.data = std::move(bytes);
    archive.entries.push_back(std::move(entry));
    return archive;
}

void resolvesPrecedenceAndRetainsSourceIdentity(const std::filesystem::path& root) {
    std::filesystem::create_directories(root / "textures");
    const auto red = rgba8Dds({255U, 0U, 0U, 255U});
    writeBytes(root / "textures/body.dds", red);

    AssetSource source;
    source.addDirectory(root, "car");
    source.addAcdArchive(archiveWith("textures/body.dds",
                                     rgba8Dds({0U, 0U, 255U, 255U})));
    const std::array<ExternalTextureGrant, 1U> grants = {{{"car-root", &source}}};
    const std::array<ExternalTextureRequest, 1U> requests = {
        request("car-root", "textures/body.dds")};
    const auto result = resolve_external_texture_authority(grants, requests);
    require(result.ok() && result.resources.size() == 1U &&
                result.request_resource_indices == std::vector<std::size_t>{0U},
            "directory and ACD precedence result");
    require(result.resources.front().source_kind == apex::assets::AssetSourceKind::directory &&
                result.resources.front().grant_id == "car-root" &&
                result.resources.front().plan.levels.front().pixels[0] == 255U,
            "directory grant wins and retains identity");

    AssetSource ambiguous;
    std::filesystem::create_directories(root / "ambiguous-a");
    std::filesystem::create_directories(root / "ambiguous-b");
    writeBytes(root / "ambiguous-a/body.dds", red);
    writeBytes(root / "ambiguous-b/body.dds", red);
    ambiguous.addDirectory(root / "ambiguous-a", "a");
    ambiguous.addDirectory(root / "ambiguous-b", "b");
    const std::array<ExternalTextureGrant, 1U> ambiguous_grants = {{{"ambiguous", &ambiguous}}};
    const std::array<ExternalTextureRequest, 1U> ambiguous_requests = {
        request("ambiguous", "body.dds")};
    const auto ambiguous_result =
        resolve_external_texture_authority(ambiguous_grants, ambiguous_requests);
    require(ambiguous_result.status == ExternalTextureAuthorityStatus::ambiguous &&
                ambiguous_result.resources.empty() && !ambiguous_result.diagnostics.empty(),
            "basename ambiguity fails closed without decoding");
}

void preservesGrantIdentityAndDeduplicates(const std::filesystem::path& root) {
    const auto red = rgba8Dds({255U, 0U, 0U, 255U});
    const auto green = rgba8Dds({0U, 255U, 0U, 255U});
    const auto root_a = root / "a";
    const auto root_b = root / "b";
    std::filesystem::create_directories(root_a / "textures");
    std::filesystem::create_directories(root_b / "textures");
    writeBytes(root_a / "textures/body.dds", red);
    writeBytes(root_b / "textures/body.dds", green);
    AssetSource source_a;
    AssetSource source_b;
    source_a.addDirectory(root_a, "a");
    source_b.addDirectory(root_b, "b");
    const std::array<ExternalTextureGrant, 2U> grants = {
        ExternalTextureGrant{"grant-a", &source_a},
        ExternalTextureGrant{"grant-b", &source_b}};
    const std::array<ExternalTextureRequest, 3U> requests = {
        request("grant-a", "textures/body.dds"),
        request("grant-a", "textures/BODY.dds"),
        request("grant-b", "textures/body.dds")};
    const auto result = resolve_external_texture_authority(grants, requests);
    require(result.ok() && result.resources.size() == 2U &&
                result.request_resource_indices == std::vector<std::size_t>{0U, 0U, 1U},
            "same-grant requests deduplicate while grants stay distinct");
    require(result.resources[0].grant_id == "grant-a" &&
                result.resources[0].plan.levels.front().pixels[0] == 255U &&
                result.resources[1].grant_id == "grant-b" &&
                result.resources[1].plan.levels.front().pixels[1] == 255U,
            "source identity prevents cross-grant confusion");
}

void rejectsUnsafeAndInvalidRequests(const std::filesystem::path& root) {
    const auto valid = rgba8Dds({1U, 2U, 3U, 255U});
    std::filesystem::create_directories(root);
    writeBytes(root / "ok.dds", valid);
    AssetSource source;
    source.addDirectory(root, "car");
    const std::array<ExternalTextureGrant, 1U> grants = {{{"car", &source}}};
    for (const auto& bad : {std::string("../ok.dds"), std::string("/ok.dds"),
                            std::string("C:\\ok.dds"), std::string("http://host/x.dds"),
                            std::string("file://ok.dds"),
                            std::string("\\\\server\\share\\ok.dds")}) {
        const std::array<ExternalTextureRequest, 1U> requests = {request("car", bad)};
        const auto result = resolve_external_texture_authority(grants, requests);
        if (result.status != ExternalTextureAuthorityStatus::rejected ||
            !result.resources.empty()) {
            throw std::runtime_error(
                "unsafe external path rejected: " + bad + " status=" +
                apex::render::external_texture_authority_status_name(result.status));
        }
    }
    {
        std::string nul_path = "ok.dds";
        nul_path.push_back('\0');
        nul_path += "hidden";
        const std::array<ExternalTextureRequest, 1U> requests = {
            request("car", std::move(nul_path))};
        const auto result = resolve_external_texture_authority(grants, requests);
        require(result.status == ExternalTextureAuthorityStatus::rejected &&
                    result.resources.empty(),
                "external path with NUL rejected");
    }
    {
        const std::array<ExternalTextureRequest, 1U> requests = {nonExternalRequest("car")};
        const auto result = resolve_external_texture_authority(grants, requests);
        require(result.status == ExternalTextureAuthorityStatus::invalid_request,
                "non-external material binding rejected");
    }
    {
        const std::array<ExternalTextureRequest, 1U> requests = {
            request("unknown", "ok.dds")};
        const auto result = resolve_external_texture_authority(grants, requests);
        require(result.status == ExternalTextureAuthorityStatus::rejected,
                "unknown grant rejected");
    }
    {
        const std::array<ExternalTextureGrant, 2U> duplicate_grants = {
            ExternalTextureGrant{"car", &source},
            ExternalTextureGrant{"car", &source}};
        const std::array<ExternalTextureRequest, 1U> requests = {
            request("car", "ok.dds")};
        const auto result =
            resolve_external_texture_authority(duplicate_grants, requests);
        require(result.status == ExternalTextureAuthorityStatus::invalid_request &&
                    result.resources.empty(),
                "duplicate grant identities rejected");
    }
    {
        writeBytes(root / "bad.dds", {1U, 2U, 3U});
        AssetSource malformed_source;
        malformed_source.addDirectory(root, "car");
        const std::array<ExternalTextureGrant, 1U> malformed_grants = {
            {{"malformed", &malformed_source}}};
        const std::array<ExternalTextureRequest, 1U> requests = {
            request("malformed", "bad.dds")};
        const auto result =
            resolve_external_texture_authority(malformed_grants, requests);
        require(result.status == ExternalTextureAuthorityStatus::invalid_request &&
                    result.resources.empty(),
                "malformed DDS fails closed without a plan");
    }
}

void enforcesSourceAndDecodedLimits(const std::filesystem::path& root) {
    const auto pixel = rgba8Dds({1U, 2U, 3U, 255U});
    std::filesystem::create_directories(root / "textures");
    writeBytes(root / "textures/a.dds", pixel);
    writeBytes(root / "textures/b.dds", pixel);
    AssetSource source;
    source.addDirectory(root, "car");
    const std::array<ExternalTextureGrant, 1U> grants = {{{"car", &source}}};
    const std::array<ExternalTextureRequest, 2U> requests = {
        request("car", "textures/a.dds"), request("car", "textures/b.dds")};

    ExternalTextureAuthorityLimits source_limits;
    source_limits.max_total_source_bytes = pixel.size();
    auto result = resolve_external_texture_authority(grants, requests, source_limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.resources.empty(),
            "source aggregate limit fails before decode");

    ExternalTextureAuthorityLimits decoded_limits;
    decoded_limits.max_total_source_bytes = pixel.size() * 2U;
    decoded_limits.max_total_decoded_bytes = 4U;
    result = resolve_external_texture_authority(grants, requests, decoded_limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.resources.empty(),
            "decoded aggregate limit removes partial plans");

    ExternalTextureAuthorityLimits single_decoded_limit;
    single_decoded_limit.max_total_decoded_bytes = 3U;
    result = resolve_external_texture_authority(
        grants, std::span<const ExternalTextureRequest>(requests).subspan(0U, 1U),
        single_decoded_limit);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.resources.empty(),
            "single decoded texture reports the authority resource limit");

    ExternalTextureAuthorityLimits plan_limits;
    plan_limits.max_total_plan_bytes = 1U;
    result = resolve_external_texture_authority(
        grants, std::span<const ExternalTextureRequest>(requests).subspan(0U, 1U),
        plan_limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.resources.empty(),
            "retained plan limit removes the decoded plan");

    ExternalTextureAuthorityLimits file_limits;
    file_limits.max_file_bytes = pixel.size() - 1U;
    result = resolve_external_texture_authority(
        grants, std::span<const ExternalTextureRequest>(requests).subspan(0U, 1U), file_limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.resources.empty(),
            "per-file limit fails before source read");
}

}  // namespace

int main() {
    const auto root = testRoot("all");
    try {
        std::filesystem::create_directories(root);
        resolvesPrecedenceAndRetainsSourceIdentity(root / "precedence");
        preservesGrantIdentityAndDeduplicates(root / "identity");
        rejectsUnsafeAndInvalidRequests(root / "invalid");
        enforcesSourceAndDecodedLimits(root / "limits");
        std::error_code cleanup;
        std::filesystem::remove_all(root, cleanup);
        std::cout << "external texture authority tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code cleanup;
        std::filesystem::remove_all(root, cleanup);
        std::cerr << "external texture authority tests failed: " << error.what() << '\n';
        return 1;
    }
}
