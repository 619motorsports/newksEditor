#include "apex/render/external_texture_authority.hpp"

#include "apex/formats/acd.hpp"
#include "apex/render/decoded_dds_texture.hpp"
#include "png_fixture.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
using apex::render::MaterialBindingOverrides;
using apex::render::MaterialTextureBinding;
using apex::render::MaterialTextureKind;
using apex::render::MaterialTextureOverride;
using apex::render::prepare_effective_stock_scene_input;
using apex::render::resolve_external_texture_authority;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    require(offset + 4U <= bytes.size(), "DDS fixture write is in bounds");
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    require(offset + 4U <= bytes.size(), "DDS fixture read is in bounds");
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::vector<std::uint8_t> rgba8Dds(std::array<std::uint8_t, 4> pixel) {
    std::vector<std::uint8_t> bytes(152U, 0U);
    put32(bytes, 0U, 0x20534444U); // DDS magic
    put32(bytes, 4U, 124U);        // header size
    put32(bytes, 12U, 1U);         // height
    put32(bytes, 16U, 1U);         // width
    put32(bytes, 20U, 4U);         // pitch
    put32(bytes, 28U, 1U);         // mip count
    put32(bytes, 76U, 32U);        // pixel format size
    put32(bytes, 80U, 0x40U);      // RGB flag
    put32(bytes, 88U, 32U);        // bits per pixel
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
    const auto result =
        prepare_effective_stock_scene_input(fixture.model, fixture.overrides, fixture.grants, fixture.requests);
    require(authority.ok() && authority.resources.size() == 1U && authority.resources.front().source_bytes == png &&
                authority.resources.front().plan.description.format == apex::render::TextureFormat::rgba8_unorm &&
                authority.resources.front().plan.levels.front().pixels ==
                    std::vector<std::uint8_t>(pixel.begin(), pixel.end()) &&
                result.ok() && result.input->model.textures.back().data == png,
            "external PNG becomes a decoded plan and exact owned effective "
            "payload");
}

void materializesMixedExternalAndSolidScene(
    const std::filesystem::path& root) {
    auto fixture = effectiveFixture(root);
    const auto normal = rgba8Dds({128U, 128U, 255U, 255U});
    fixture.model.materials.front().resources.push_back(
        {"txNormal", 22U, "normal.dds"});
    fixture.model.textures.push_back(
        {true, "normal.dds", static_cast<std::uint32_t>(normal.size()),
         normal, 7U});
    MaterialTextureOverride solid;
    solid.color = std::array<float, 4>{0.25F, 0.5F, 0.75F, 1.0F};
    fixture.overrides.front().resources.emplace("txNormal", solid);
    AssetSource source;
    source.addDirectory(root, "car");
    fixture.grants = {{"car-grant", &source}};

    const auto result = prepare_effective_stock_scene_input(
        fixture.model, fixture.overrides, fixture.grants, fixture.requests);
    require(result.ok() && result.input->external_bindings.size() == 1U &&
                result.input->solid_color_bindings.size() == 1U &&
                result.input->model.textures.size() == 4U &&
                result.input->overrides_by_material.front().resources.empty(),
            "mixed external and solid resources share one effective scene");
    const auto& resources = result.input->model.materials.front().resources;
    require(resources.size() == 2U && resources[0U].textureId == 17U &&
                resources[1U].textureId == 22U &&
                resources[0U].texture ==
                    result.input->external_bindings.front()
                        .synthetic_texture_name &&
                resources[1U].texture ==
                    result.input->solid_color_bindings.front()
                        .synthetic_texture_name,
            "mixed effective resources preserve both serialized bind points");
    require(fixture.model.textures.size() == 2U &&
                fixture.model.materials.front().resources[0U].texture ==
                    "base.dds" &&
                fixture.model.materials.front().resources[1U].texture ==
                    "normal.dds" &&
                fixture.overrides.front().resources.size() == 2U,
            "mixed preparation preserves the complete caller input");
}

void materializesExactSolidColorEffectiveScene(const std::filesystem::path& root) {
    auto fixture = effectiveFixture(root);
    fixture.grants.clear();
    fixture.requests.clear();
    auto& override_value = fixture.overrides.front().resources.at("txDiffuse");
    override_value.texture = "ignored.dds";
    override_value.file = "missing/ignored.dds";
    override_value.color = std::array<float, 4>{0.125F, 0.5F, 0.75F, 1.0F};

    const auto original_texture = fixture.model.materials.front().resources.front().texture;
    const auto result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {});
    require(result.ok() && result.input->model.textures.size() == 2U && result.input->external_bindings.empty() &&
                result.input->solid_color_bindings.size() == 1U &&
                result.input->overrides_by_material.front().resources.empty(),
            "solid color becomes an owned effective-scene texture without a grant");
    const auto& material = result.input->model.materials.front();
    require(material.resources.size() == 1U && material.resources.front().textureId == 17U &&
                material.resources.front().texture != original_texture,
            "solid color preserves the serialized bind point and replaces only "
            "texture name");
    const auto& synthetic = result.input->model.textures.back();
    require(synthetic.name.rfind("__apex_solid_color_", 0U) == 0U && synthetic.workspaceFileIndex == 7U &&
                synthetic.size == 132U && synthetic.data.size() == 132U,
            "solid color uses one scoped opaque 132-byte DDS");
    require(get32(synthetic.data, 0U) == 0x20534444U && get32(synthetic.data, 4U) == 124U &&
                get32(synthetic.data, 8U) == 0x100fU && get32(synthetic.data, 12U) == 1U &&
                get32(synthetic.data, 16U) == 1U && get32(synthetic.data, 20U) == 4U &&
                get32(synthetic.data, 28U) == 0U && get32(synthetic.data, 76U) == 32U &&
                get32(synthetic.data, 80U) == 0x41U && get32(synthetic.data, 88U) == 32U &&
                get32(synthetic.data, 92U) == 0x00ff0000U && get32(synthetic.data, 96U) == 0x0000ff00U &&
                get32(synthetic.data, 100U) == 0x000000ffU && get32(synthetic.data, 104U) == 0xff000000U &&
                get32(synthetic.data, 108U) == 0x1000U,
            "solid color DDS header matches the recovered FBX helper");
    require(synthetic.data[128U] == 191U && synthetic.data[129U] == 128U && synthetic.data[130U] == 32U &&
                synthetic.data[131U] == 255U,
            "solid color uses JavaScript rounding and exact BGRA payload order");
    const auto planned = apex::render::plan_decoded_texture_payload(
        synthetic.data, synthetic.name);
    require(planned.ok() && planned.plan.description.width == 1U &&
                planned.plan.description.height == 1U &&
                planned.plan.description.mip_levels == 1U &&
                planned.plan.description.format ==
                    apex::render::TextureFormat::rgba8_unorm &&
                planned.plan.levels.size() == 1U &&
                planned.plan.levels.front().pixels ==
                    std::vector<std::uint8_t>{32U, 128U, 191U, 255U},
            "solid-color DDS round-trips to one exact RGBA8 texel");
    auto truncated = synthetic.data;
    truncated.pop_back();
    const auto rejected = apex::render::plan_decoded_texture_payload(
        truncated, "truncated-solid-color.dds");
    require(!rejected.ok(),
            "truncated solid-color DDS is rejected by the texture planner");
    require(fixture.model.materials.front().resources.front().texture == original_texture &&
                fixture.overrides.front().resources.at("txDiffuse").color == override_value.color &&
                fixture.overrides.front().resources.at("txDiffuse").file == "missing/ignored.dds",
            "solid color preparation does not mutate caller model or overrides");

    ExternalTextureRequest conflicting =
        request("unused-grant", "missing/ignored.dds");
    conflicting.material_index = 0U;
    conflicting.workspace_file_index = 7U;
    const std::array<ExternalTextureRequest, 1U> conflicting_requests = {
        std::move(conflicting)};
    const auto conflict_result = prepare_effective_stock_scene_input(
        fixture.model, fixture.overrides, {}, conflicting_requests);
    require(conflict_result.status == ExternalTextureAuthorityStatus::rejected &&
                conflict_result.input == nullptr &&
                conflict_result.diagnostics.front().code ==
                    "external_texture_override_mismatch",
            "solid-color precedence rejects a conflicting external request");
}

void deduplicatesAndClampsSolidColors(const std::filesystem::path& root) {
    auto fixture = effectiveFixture(root);
    fixture.grants.clear();
    fixture.requests.clear();
    fixture.model.materials.push_back(fixture.model.materials.front());
    fixture.model.materials.back().name = "glass";
    fixture.overrides.resize(2U);
    MaterialTextureOverride first;
    first.color = std::array<float, 4>{-1.0F, 1.25F, 0.5F, 0.0F};
    fixture.overrides[0U].resources.clear();
    fixture.overrides[0U].resources.emplace("txDiffuse", first);
    fixture.overrides[1U].resources.emplace("txDiffuse", first);

    auto result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {});
    require(result.ok() && result.input->model.textures.size() == 2U &&
                result.input->solid_color_bindings.size() == 2U &&
                result.input->model.materials[0U].resources[0U].texture ==
                    result.input->model.materials[1U].resources[0U].texture,
            "equal colors in one workspace scope share one synthetic texture");
    const auto& data = result.input->model.textures.back().data;
    require(data[128U] == 128U && data[129U] == 255U && data[130U] == 0U && data[131U] == 0U,
            "finite solid colors clamp before deterministic tie rounding");

    fixture.model.textures.push_back({true, " __APEX_SOLID_COLOR_0 ", 0U, {}, 7U});
    result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {});
    require(result.ok() && result.input->model.textures.back().name != "__apex_solid_color_0",
            "solid-color names avoid existing scoped texture collisions");

    fixture.model.materials[1U].workspaceFileIndex = 8U;
    result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {});
    require(result.ok() && result.input->model.textures.size() == 4U &&
                result.input->model.materials[0U].resources[0U].texture !=
                    result.input->model.materials[1U].resources[0U].texture,
            "equal colors in different workspace scopes retain separate textures");
}

void rejectsInvalidSolidColorsAtomically(const std::filesystem::path& root) {
    auto fixture = effectiveFixture(root);
    fixture.grants.clear();
    fixture.requests.clear();
    fixture.overrides.front().resources.at("txDiffuse").file.clear();

    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity()}) {
        fixture.overrides.front().resources.at("txDiffuse").color = std::array<float, 4>{invalid, 0.0F, 0.0F, 1.0F};
        const auto result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {});
        require(result.status == ExternalTextureAuthorityStatus::invalid_request && result.input == nullptr &&
                    result.diagnostics.front().code == "solid_color_non_finite",
                "non-finite solid color fails atomically");
    }

    fixture.model.materials.front().resources.push_back(
        {"txNormal", 22U, "normal.dds"});
    MaterialTextureOverride external_override;
    external_override.file = "missing.dds";
    fixture.overrides.front().resources.emplace("txNormal",
                                                external_override);
    fixture.overrides.front().resources.at("txDiffuse").color =
        std::array<float, 4>{
            std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F, 1.0F};
    ExternalTextureRequest external = request("missing-grant", "missing.dds");
    external.material_index = 0U;
    external.workspace_file_index = 7U;
    external.binding.slot = "txNormal";
    const std::array<ExternalTextureRequest, 1U> external_requests = {
        std::move(external)};
    auto result = prepare_effective_stock_scene_input(
        fixture.model, fixture.overrides, {}, external_requests);
    require(result.status == ExternalTextureAuthorityStatus::invalid_request &&
                result.input == nullptr &&
                result.diagnostics.front().code ==
                    "solid_color_non_finite",
            "invalid solid color fails before an external grant is resolved");

    fixture.overrides.front().resources.clear();
    MaterialTextureOverride missing_bind;
    missing_bind.color = std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F};
    fixture.overrides.front().resources.emplace("txMaps", missing_bind);
    result = prepare_effective_stock_scene_input(fixture.model,
                                                 fixture.overrides, {}, {});
    require(result.status == ExternalTextureAuthorityStatus::invalid_request && result.input == nullptr &&
                result.diagnostics.front().code == "solid_color_bind_point_missing",
            "new solid-color slot requires an explicit bind point");

    fixture.overrides.front().resources.at("txMaps").bind_point = 4U;
    result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {});
    require(result.ok() && result.input->model.materials.front().resources.back().slot == "txMaps" &&
                result.input->model.materials.front().resources.back().textureId == 4U,
            "new solid-color slot retains its explicit bind point");

    fixture.overrides.front().resources.emplace(" TXMAPS ", fixture.overrides.front().resources.at("txMaps"));
    result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {});
    require(result.status == ExternalTextureAuthorityStatus::ambiguous && result.input == nullptr &&
                result.diagnostics.front().code == "solid_color_override_collision",
            "case-insensitive solid-color slot collision fails atomically");
}

void enforcesSolidColorEffectiveLimits(const std::filesystem::path& root) {
    auto fixture = effectiveFixture(root);
    fixture.grants.clear();
    fixture.requests.clear();
    auto& override_value = fixture.overrides.front().resources.at("txDiffuse");
    override_value.file.clear();
    override_value.color = std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F};

    ExternalTextureAuthorityLimits limits;
    limits.max_total_effective_source_bytes = 131U;
    auto result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {}, limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit && result.input == nullptr &&
                result.diagnostics.front().code == "solid_color_effective_source_limit",
            "solid-color DDS aggregate limit fails atomically");

    limits = {};
    limits.max_effective_textures = fixture.model.textures.size();
    result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {}, limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit && result.input == nullptr &&
                result.diagnostics.front().code == "solid_color_effective_texture_limit",
            "solid-color texture count limit fails atomically");

    limits = {};
    limits.max_effective_texture_name_bytes = 1U;
    result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides, {}, {}, limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit && result.input == nullptr &&
                result.diagnostics.front().code == "solid_color_effective_name_limit",
            "solid-color name limit fails atomically");

    fixture.model.materials.push_back(fixture.model.materials.front());
    fixture.model.materials.back().name = "second";
    fixture.overrides.resize(2U);
    MaterialTextureOverride second;
    second.color = std::array<float, 4>{0.9F, 0.8F, 0.7F, 1.0F};
    fixture.overrides[1U].resources.emplace("txDiffuse", second);
    limits = {};
    limits.max_solid_colors = 1U;
    result = prepare_effective_stock_scene_input(fixture.model, fixture.overrides,
                                                 {}, {}, limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.input == nullptr &&
                result.diagnostics.front().code ==
                    "solid_color_count_limit",
            "solid-color override count limit fails atomically");

    limits = {};
    limits.max_total_effective_copy_bytes = 4096U;
    result = prepare_effective_stock_scene_input(fixture.model,
                                                 fixture.overrides, {}, {},
                                                 limits);
    require(result.ok(),
            "bounded effective copy fixture fits before hostile growth");
    fixture.overrides[1U].resources.emplace(
        "txNormal",
        MaterialTextureOverride{std::nullopt, std::string(8192U, 'x'), {},
                                std::nullopt});
    result = prepare_effective_stock_scene_input(fixture.model,
                                                 fixture.overrides, {}, {},
                                                 limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.input == nullptr &&
                result.diagnostics.front().code ==
                    "external_texture_effective_copy_limit",
            "unrelated override storage is bounded before the model copy");

    fixture.overrides[1U].resources.erase("txNormal");
    fixture.model.root.children.push_back({});
    limits = {};
    limits.max_effective_nodes = 1U;
    result = prepare_effective_stock_scene_input(fixture.model,
                                                 fixture.overrides, {}, {},
                                                 limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit &&
                result.input == nullptr &&
                result.diagnostics.front().code ==
                    "external_texture_effective_copy_limit",
            "effective model node count is bounded before the model copy");
}

void rejectsEffectiveMalformedScopeAndCollisions(const std::filesystem::path& root) {
    auto malformed = effectiveFixture(root / "malformed", "textures/bad.dds");
    writeBytes(root / "malformed" / "textures/bad.dds", {1U, 2U, 3U});
    AssetSource malformed_source;
    malformed_source.addDirectory(root / "malformed", "malformed");
    malformed.grants = {{"car-grant", &malformed_source}};
    auto result =
        prepare_effective_stock_scene_input(malformed.model, malformed.overrides, malformed.grants, malformed.requests);
    require(result.status == ExternalTextureAuthorityStatus::invalid_request && result.input == nullptr,
            "truncated effective DDS fails atomically");

    auto malformed_png = effectiveFixture(root / "malformed-png", "textures/bad.png");
    auto short_png = apex::tests::rgba8PngFixture({1U, 2U, 3U, 255U});
    short_png.pop_back();
    writeBytes(root / "malformed-png" / "textures/bad.png", short_png);
    AssetSource malformed_png_source;
    malformed_png_source.addDirectory(root / "malformed-png", "malformed-png");
    malformed_png.grants = {{"car-grant", &malformed_png_source}};
    result = prepare_effective_stock_scene_input(malformed_png.model, malformed_png.overrides, malformed_png.grants,
                                                 malformed_png.requests);
    require(result.status == ExternalTextureAuthorityStatus::invalid_request && result.input == nullptr,
            "truncated effective PNG fails atomically");

    auto scoped = effectiveFixture(root / "scope");
    AssetSource scoped_source;
    scoped_source.addDirectory(root / "scope", "scope");
    scoped.grants = {{"car-grant", &scoped_source}};
    scoped.requests.front().workspace_file_index = 8U;
    result = prepare_effective_stock_scene_input(scoped.model, scoped.overrides, scoped.grants, scoped.requests);
    require(result.status == ExternalTextureAuthorityStatus::rejected && result.input == nullptr,
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
    result =
        prepare_effective_stock_scene_input(collision.model, collision.overrides, collision.grants, collision.requests);
    require(result.status == ExternalTextureAuthorityStatus::ambiguous && result.input == nullptr,
            "same material slot collision fails atomically");

    auto name_collision = effectiveFixture(root / "name-collision");
    name_collision.model.textures.push_back({true, "__apex_external_image_0", 0U, {}, 7U});
    AssetSource name_source;
    name_source.addDirectory(root / "name-collision", "name");
    name_collision.grants = {{"car-grant", &name_source}};
    result = prepare_effective_stock_scene_input(name_collision.model, name_collision.overrides, name_collision.grants,
                                                 name_collision.requests);
    require(result.ok() && result.input->model.textures.back().name != "__apex_external_image_0",
            "synthetic texture names avoid scoped table collisions");

    auto budget = effectiveFixture(root / "budget");
    AssetSource budget_source;
    budget_source.addDirectory(root / "budget", "budget");
    budget.grants = {{"car-grant", &budget_source}};
    ExternalTextureAuthorityLimits effective_limits;
    effective_limits.max_total_effective_source_bytes = 1U;
    result = prepare_effective_stock_scene_input(budget.model, budget.overrides, budget.grants, budget.requests,
                                                 effective_limits);
    require(result.status == ExternalTextureAuthorityStatus::resource_limit && result.input == nullptr,
            "effective owned DDS aggregate limit fails atomically");
}

} // namespace

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
        materializesMixedExternalAndSolidScene(root / "effective-mixed");
        materializesExactSolidColorEffectiveScene(root / "effective-solid");
        deduplicatesAndClampsSolidColors(root / "effective-solid-dedup");
        rejectsInvalidSolidColorsAtomically(root / "effective-solid-invalid");
        enforcesSolidColorEffectiveLimits(root / "effective-solid-limits");
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
