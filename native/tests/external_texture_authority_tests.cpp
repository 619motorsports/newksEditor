#include "apex/render/external_texture_authority.hpp"

#include "apex/formats/acd.hpp"
#include "png_fixture.hpp"

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
using apex::render::MaterialBindingOverrides;
using apex::render::MaterialTextureOverride;
using apex::render::prepare_effective_stock_scene_input;
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
    return {std::move(grant), std::move(binding),
            apex::render::invalid_external_texture_resource, {}};
}

ExternalTextureRequest nonExternalRequest(std::string grant) {
    MaterialTextureBinding binding;
    binding.slot = "txDiffuse";
    binding.kind = MaterialTextureKind::embedded;
    return {std::move(grant), std::move(binding),
            apex::render::invalid_external_texture_resource, {}};
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

struct EffectiveFixture {
    apex::formats::Kn5File model;
    std::vector<MaterialBindingOverrides> overrides;
    std::vector<ExternalTextureGrant> grants;
    std::vector<ExternalTextureRequest> requests;
};

EffectiveFixture effectiveFixture(const std::filesystem::path& root,
                                  std::string file = "textures/override.dds") {
    const auto base = rgba8Dds({9U, 8U, 7U, 255U});
    const auto replacement = rgba8Dds({1U, 2U, 3U, 255U});
    std::filesystem::create_directories(root / "textures");
    writeBytes(root / file, replacement);

    EffectiveFixture fixture;
    fixture.model.materials.push_back({});
    fixture.model.materials.front().name = "body";
    fixture.model.materials.front().shader = "ksPerPixel";
    fixture.model.materials.front().workspaceFileIndex = 7U;
    fixture.model.materials.front().resources.push_back({"txDiffuse", 17U, "base.dds"});
    fixture.model.textures.push_back({true, "base.dds", static_cast<std::uint32_t>(base.size()),
                                      base, 7U});
    fixture.overrides.resize(1U);
    MaterialTextureOverride override_value;
    override_value.file = file;
    fixture.overrides.front().resources.emplace("txDiffuse", std::move(override_value));

    fixture.requests.push_back(request("car-grant", file));
    auto& external = fixture.requests.back();
    external.material_index = 0U;
    external.workspace_file_index = 7U;
    return fixture;
}

void materializesOwnedEffectiveScene(const std::filesystem::path& root) {
    auto fixture = effectiveFixture(root);
    AssetSource source;
    source.addDirectory(root, "car");
    fixture.grants = {{"car-grant", &source}};

    const auto result = prepare_effective_stock_scene_input(
        fixture.model, fixture.overrides, fixture.grants, fixture.requests);
    require(result.ok() && result.input->model.textures.size() == 2U &&
                result.input->overrides_by_material.size() == 1U &&
                result.input->overrides_by_material.front().resources.empty(),
            "external DDS becomes an owned effective-scene texture");
    const auto& material = result.input->model.materials.front();
    require(material.resources.size() == 1U && material.resources.front().textureId == 17U,
            "effective replacement preserves the serialized bind point");
    const auto& synthetic = result.input->model.textures.back();
    require(synthetic.name.rfind("__apex_external_image_", 0U) == 0U &&
                synthetic.name.find("override.dds") == std::string::npos &&
                synthetic.workspaceFileIndex == 7U && synthetic.data == rgba8Dds({1U, 2U, 3U, 255U}),
            "effective texture has opaque name, source scope, and exact DDS bytes");
    require(result.input->external_bindings.size() == 1U &&
                result.input->external_bindings.front().grant_id == "car-grant" &&
                result.input->external_bindings.front().requested_path == "textures/override.dds",
            "effective binding retains source identity outside renderer input");
}

void materializesOwnedPngEffectiveScene(const std::filesystem::path& root) {
    auto fixture = effectiveFixture(root, "textures/override.png");
    const std::array<std::uint8_t, 4> pixel = {13U, 47U, 149U, 229U};
    const auto png = apex::tests::rgba8PngFixture(pixel);
    writeBytes(root / "textures/override.png", png);
    AssetSource source;
    source.addDirectory(root, "car");
    fixture.grants = {{"car-grant", &source}};

    const auto authority = resolve_external_texture_authority(
        fixture.grants, fixture.requests);
    const auto result = prepare_effective_stock_scene_input(
        fixture.model, fixture.overrides, fixture.grants, fixture.requests);
    require(authority.ok() && authority.resources.size() == 1U &&
                authority.resources.front().source_bytes == png &&
                authority.resources.front().plan.description.format ==
                    apex::render::TextureFormat::rgba8_unorm &&
                authority.resources.front().plan.levels.front().pixels ==
                    std::vector<std::uint8_t>(pixel.begin(), pixel.end()) &&
                result.ok() &&
                result.input->model.textures.back().data == png,
            "external PNG becomes a decoded plan and exact owned effective payload");
}

void rejectsEffectiveMalformedScopeAndCollisions(const std::filesystem::path& root) {
    auto malformed = effectiveFixture(root / "malformed", "textures/bad.dds");
    writeBytes(root / "malformed" / "textures/bad.dds", {1U, 2U, 3U});
    AssetSource malformed_source;
    malformed_source.addDirectory(root / "malformed", "malformed");
    malformed.grants = {{"car-grant", &malformed_source}};
    auto result = prepare_effective_stock_scene_input(
        malformed.model, malformed.overrides, malformed.grants, malformed.requests);
    require(result.status == ExternalTextureAuthorityStatus::invalid_request &&
                result.input == nullptr,
            "truncated effective DDS fails atomically");

    auto malformed_png = effectiveFixture(
        root / "malformed-png", "textures/bad.png");
    auto short_png = apex::tests::rgba8PngFixture({1U, 2U, 3U, 255U});
    short_png.pop_back();
    writeBytes(root / "malformed-png" / "textures/bad.png", short_png);
    AssetSource malformed_png_source;
    malformed_png_source.addDirectory(root / "malformed-png", "malformed-png");
    malformed_png.grants = {{"car-grant", &malformed_png_source}};
    result = prepare_effective_stock_scene_input(
        malformed_png.model, malformed_png.overrides,
        malformed_png.grants, malformed_png.requests);
    require(result.status == ExternalTextureAuthorityStatus::invalid_request &&
                result.input == nullptr,
            "truncated effective PNG fails atomically");

    auto scoped = effectiveFixture(root / "scope");
    AssetSource scoped_source;
    scoped_source.addDirectory(root / "scope", "scope");
    scoped.grants = {{"car-grant", &scoped_source}};
    scoped.requests.front().workspace_file_index = 8U;
    result = prepare_effective_stock_scene_input(
        scoped.model, scoped.overrides, scoped.grants, scoped.requests);
    require(result.status == ExternalTextureAuthorityStatus::rejected &&
                result.input == nullptr,
            "workspace scope mismatch fails atomically");

    auto collision = effectiveFixture(root / "collision");
    AssetSource first_source;
    AssetSource second_source;
    first_source.addDirectory(root / "collision", "first");
    second_source.addDirectory(root / "collision", "second");
    collision.grants = {{"first", &first_source}, {"second", &second_source}};
    collision.requests.front().grant_id = "first";
    auto duplicate = collision.requests.front();
    duplicate.grant_id = "second";
    collision.requests.push_back(std::move(duplicate));
    result = prepare_effective_stock_scene_input(
        collision.model, collision.overrides, collision.grants, collision.requests);
    require(result.status == ExternalTextureAuthorityStatus::ambiguous &&
                result.input == nullptr,
            "same material slot collision fails atomically");

    auto name_collision = effectiveFixture(root / "name-collision");
    name_collision.model.textures.push_back(
        {true, "__apex_external_image_0", 0U, {}, 7U});
    AssetSource name_source;
    name_source.addDirectory(root / "name-collision", "name");
    name_collision.grants = {{"car-grant", &name_source}};
    result = prepare_effective_stock_scene_input(
        name_collision.model, name_collision.overrides,
        name_collision.grants, name_collision.requests);
    require(result.ok() && result.input->model.textures.back().name !=
                "__apex_external_image_0",
            "synthetic texture names avoid scoped table collisions");

    auto budget = effectiveFixture(root / "budget");
    AssetSource budget_source;
    budget_source.addDirectory(root / "budget", "budget");
    budget.grants = {{"car-grant", &budget_source}};
    ExternalTextureAuthorityLimits effective_limits;
    effective_limits.max_total_effective_source_bytes = 1U;
    result = prepare_effective_stock_scene_input(
        budget.model, budget.overrides, budget.grants, budget.requests,
        effective_limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.input == nullptr,
            "effective owned DDS aggregate limit fails atomically");
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
        materializesOwnedEffectiveScene(root / "effective");
        materializesOwnedPngEffectiveScene(root / "effective-png");
        rejectsEffectiveMalformedScopeAndCollisions(root / "effective-cases");
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
