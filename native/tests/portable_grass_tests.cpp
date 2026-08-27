#include "apex/render/device.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace apex::render;

namespace {

class TestBuffer final : public Buffer {
public:
    explicit TestBuffer(BufferDescription description,
                        Backend backend = Backend::Vulkan)
        : backend_(backend), info_({description}) {}
    Backend backend() const noexcept override { return backend_; }
    const BufferInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    BufferInfo info_;
};

class TestTexture final : public Texture {
public:
    explicit TestTexture(TextureDescription description,
                         Backend backend = Backend::Vulkan)
        : backend_(backend), info_({description}) {}
    Backend backend() const noexcept override { return backend_; }
    const TextureInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    TextureInfo info_;
};

class TestSampler final : public Sampler {
public:
    explicit TestSampler(Backend backend = Backend::Vulkan)
        : backend_(backend) {}
    Backend backend() const noexcept override { return backend_; }
    const SamplerInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    SamplerInfo info_;
};

class TestDepthAttachment final : public DepthAttachment {
public:
    explicit TestDepthAttachment(DepthAttachmentDescription description,
                                 Backend backend = Backend::Vulkan)
        : backend_(backend), info_({description}) {}
    Backend backend() const noexcept override { return backend_; }
    const DepthAttachmentInfo& info() const noexcept override { return info_; }
private:
    Backend backend_;
    DepthAttachmentInfo info_;
};

void require(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void requireNear(const float actual, const float expected, const float epsilon,
                 const std::string_view message) {
    require(std::isfinite(actual) && std::abs(actual - expected) <= epsilon, message);
}

std::array<float, 3U> position(const PortableGrassVertex& vertex) {
    return {vertex.position_x, vertex.position_y, vertex.position_z};
}

float distance(const std::array<float, 3U>& left, const std::array<float, 3U>& right) {
    const auto delta = std::array<float, 3U>{left[0U] - right[0U], left[1U] - right[1U],
                                             left[2U] - right[2U]};
    return std::sqrt(delta[0U] * delta[0U] + delta[1U] * delta[1U] + delta[2U] * delta[2U]);
}

std::array<float, 3U> cross(const std::array<float, 3U>& left,
                            const std::array<float, 3U>& right) {
    return {left[1U] * right[2U] - left[2U] * right[1U],
            left[2U] * right[0U] - left[0U] * right[2U],
            left[0U] * right[1U] - left[1U] * right[0U]};
}

float triangle_area(const PortableGrassVertex& first, const PortableGrassVertex& second,
                    const PortableGrassVertex& third) {
    const auto ab = std::array<float, 3U>{second.position_x - first.position_x,
                                          second.position_y - first.position_y,
                                          second.position_z - first.position_z};
    const auto ac = std::array<float, 3U>{third.position_x - first.position_x,
                                          third.position_y - first.position_y,
                                          third.position_z - first.position_z};
    const auto normal = cross(ab, ac);
    return 0.5F * std::sqrt(normal[0U] * normal[0U] + normal[1U] * normal[1U] +
                             normal[2U] * normal[2U]);
}

PortableGrassSourceTriangle triangle(const std::uint32_t source_id = 7U) {
    PortableGrassSourceTriangle value;
    value.source_id = source_id;
    value.vertices[0U].position = {0.0F, 0.0F, 0.0F};
    value.vertices[1U].position = {2.0F, 0.0F, 0.0F};
    value.vertices[2U].position = {0.0F, 0.0F, 2.0F};
    for (auto& vertex : value.vertices)
        vertex.normal = {0.0F, 1.0F, 0.0F};
    return value;
}

void accepts_source_triangle_and_emits_expanded_abi() {
    const std::array triangles = {triangle()};
    PortableGrassSettings settings;
    settings.density = 1.0F;
    settings.height = 2.0F;
    settings.width = 0.5F;
    settings.atlas_columns = 4U;
    settings.atlas_rows = 2U;
    settings.atlas_tile = 5U;
    PortableGrassBuildOptions options;
    options.seed = 42U;

    const auto result = buildPortableGrassLayout(triangles, settings, options);
    require(result.ready() && result.blades.size() == 2U,
            "source triangle emits deterministic blade count");
    require(result.vertices.size() == result.blades.size() * 6U,
            "each blade emits six vertices");
    require(result.blades.front().source_triangle == 7U,
            "source identity is preserved");
    require(result.blades[0U].angle_degrees != result.blades[1U].angle_degrees,
            "blade orientation varies deterministically per blade");
    require(result.vertices.front().u == 0.25F && result.vertices.front().v == 1.0F &&
                result.vertices[2U].u == 0.5F && result.vertices[2U].v == 0.5F,
            "atlas tile is baked into expanded UVs");
    require(result.vertices.front().position_y == 0.0F &&
                result.vertices[2U].position_y > result.vertices.front().position_y,
            "expanded blade has a ground base and raised top");
    require(result.vertices[0U].tip_weight == 0.0F &&
                result.vertices[1U].tip_weight == 0.0F &&
                result.vertices[2U].tip_weight == 1.0F,
            "expanded blade carries tile-independent wind tip weights");
    for (std::size_t blade = 0U; blade < result.blades.size(); ++blade) {
        const auto offset = blade * 6U;
        requireNear(distance(position(result.vertices[offset]), position(result.vertices[offset + 1U])),
                    result.blades[blade].width, 1.0e-5F, "bottom edge is blade width");
        requireNear(distance(position(result.vertices[offset + 5U]),
                             position(result.vertices[offset + 2U])),
                    result.blades[blade].width, 1.0e-5F, "top edge is blade width");
        require(triangle_area(result.vertices[offset], result.vertices[offset + 1U],
                              result.vertices[offset + 2U]) > 1.0e-6F &&
                    triangle_area(result.vertices[offset + 3U], result.vertices[offset + 4U],
                                  result.vertices[offset + 5U]) > 1.0e-6F,
                "expanded blade contains two nondegenerate triangles");
    }
    for (const auto& vertex : result.vertices) {
        require(std::isfinite(vertex.position_x) && std::isfinite(vertex.position_y) &&
                    std::isfinite(vertex.position_z) && std::isfinite(vertex.normal_x) &&
                    std::isfinite(vertex.normal_y) && std::isfinite(vertex.normal_z) &&
                    std::isfinite(vertex.u) && std::isfinite(vertex.v),
                "expanded grass ABI remains finite");
    }
}

void rejects_malformed_requests() {
    const std::array triangles = {triangle()};
    PortableGrassSettings settings;
    PortableGrassBuildOptions options;

    options.declared_triangle_count = triangles.size() + 1U;
    require(validatePortableGrassRequest(triangles, settings, options).code ==
                "grass_source_span_truncated",
            "truncated source span is rejected");
    options = {};
    settings.density = std::numeric_limits<float>::quiet_NaN();
    require(validatePortableGrassRequest(triangles, settings, options).code ==
                "grass_settings_invalid",
            "nonfinite density is rejected");
    settings = {};
    settings.width = std::numeric_limits<float>::infinity();
    require(validatePortableGrassRequest(triangles, settings, options).code ==
                "grass_settings_invalid",
            "infinite width is rejected");
    settings = {};
    settings.atlas_columns = 65U;
    require(validatePortableGrassRequest(triangles, settings, options).code ==
                "grass_atlas_grid_invalid",
            "oversized atlas grid is rejected");
    settings = {};
    settings.atlas_tile = 16U;
    require(validatePortableGrassRequest(triangles, settings, options).code ==
                "grass_atlas_tile_invalid",
            "atlas tile outside grid is rejected");
    settings = {};
    auto malformed = triangles;
    malformed[0U].vertices[1U].position[0U] = std::numeric_limits<float>::quiet_NaN();
    require(validatePortableGrassRequest(malformed, settings, options).code ==
                "grass_source_vertex_nonfinite",
            "nonfinite source vertex is rejected");
    malformed = triangles;
    malformed[0U].vertices[1U].position[0U] = 1.0e9F;
    require(validatePortableGrassRequest(malformed, settings, options).code ==
                "grass_source_coordinate_limit",
            "excessive source coordinate is bounded");
    malformed = triangles;
    malformed[0U].vertices[0U].normal = {1001.0F, 0.0F, 0.0F};
    require(validatePortableGrassRequest(malformed, settings, options).code ==
                "grass_source_normal_limit",
            "excessive source normal is bounded");
}

void rejects_degenerate_and_non_upward_triangles() {
    auto degenerate = triangle();
    degenerate.vertices[2U].position = degenerate.vertices[1U].position;
    auto tilted = triangle(8U);
    tilted.vertices[1U].position = {0.0F, 2.0F, 0.0F};
    tilted.vertices[2U].position = {0.0F, 0.0F, 2.0F};
    const std::array triangles = {degenerate, tilted};
    PortableGrassSettings settings;
    settings.density = 10.0F;
    const auto result = buildPortableGrassLayout(triangles, settings);
    require(result.status == PortableGrassStatus::empty &&
                result.diagnostics.rejected_degenerate_triangles == 1U &&
                result.diagnostics.rejected_non_upward_triangles == 1U,
            "degenerate and non-upward triangles are rejected");
}

void applies_budgets_and_reports_truncation() {
    const std::array triangles = {triangle()};
    PortableGrassSettings settings;
    settings.density = 100.0F;
    PortableGrassBuildOptions options;
    options.max_blades = 3U;
    options.max_candidates = 4U;
    const auto result = buildPortableGrassLayout(triangles, settings, options);
    require(result.ready() && result.blades.size() == 3U && result.vertices.size() == 18U,
            "blade budget bounds output allocation");
    require(result.diagnostics.candidate_count == 3U &&
                result.diagnostics.generation_truncated &&
                result.code == "grass_layout_ready_truncated",
            "generation budget reports deterministic truncation");

    options.max_blades = portable_grass_max_blades + 1U;
    require(validatePortableGrassRequest(triangles, settings, options).status ==
                PortableGrassStatus::resource_limit,
            "maximum blade budget is bounded");
}

void remains_deterministic_for_same_seed_and_inputs() {
    const std::array triangles = {triangle(), triangle(11U)};
    PortableGrassSettings settings;
    settings.density = 4.0F;
    settings.height_variation = 0.25F;
    settings.width_variation = 0.5F;
    settings.angle_degrees = 17.0F;
    PortableGrassBuildOptions options;
    options.seed = 0x12345678U;
    const auto first = buildPortableGrassLayout(triangles, settings, options);
    const auto second = buildPortableGrassLayout(triangles, settings, options);
    require(first.ready() && first.blades == second.blades && first.vertices == second.vertices &&
                first.diagnostics.candidate_count == second.diagnostics.candidate_count,
            "same seed produces identical blades and vertices");
    require(first.blades.size() == 16U && first.vertices.size() == 96U &&
                first.blades.front().index == 0U &&
                first.blades.front().source_triangle == 7U &&
                first.blades[8U].index == 8U &&
                first.blades[8U].source_triangle == 11U,
            "deterministic platform-independent blade identity is stable");
    options.seed++;
    const auto different = buildPortableGrassLayout(triangles, settings, options);
    require(first.vertices != different.vertices,
            "different seed changes deterministic placement");
}

void validates_execution_contract_before_backend_allocation() {
    const std::array triangles = {triangle()};
    PortableGrassSettings settings;
    settings.density = 1.0F;
    const auto layout = buildPortableGrassLayout(triangles, settings);
    require(layout.ready(), "grass execution layout is ready");

    BufferDescription buffer_description;
    buffer_description.size_bytes =
        layout.vertices.size() * sizeof(PortableGrassVertex);
    buffer_description.usage = BufferUsage::vertex;
    TestBuffer buffer(buffer_description);
    TextureDescription atlas_description;
    atlas_description.width = 4U;
    atlas_description.height = 4U;
    atlas_description.format = TextureFormat::rgba8_unorm;
    atlas_description.usage = TextureUsage::sampled;
    TestTexture atlas(atlas_description);
    TestSampler sampler;

    PortableGrassParameters grass;
    grass.camera.clip_space = CameraClipSpace::vulkan;
    grass.camera.position = {1.0F, 2.0F, 3.0F};
    grass.sun_direction = {0.0F, 2.0F, 0.0F};
    grass.sun_color = {2.0F, 3.0F, 4.0F};
    grass.ambient_color = {0.1F, 0.2F, 0.3F};
    grass.fog_color = {0.4F, 0.5F, 0.6F};
    grass.fog_distance = 1000.0F;
    grass.fog_blend = 0.8F;
    grass.wetness = 0.25F;
    grass.elapsed_seconds = 4.0F;
    grass.wind_direction = {3.0F, 4.0F};
    grass.wind_strength = 0.5F;
    grass.vertex_buffer = &buffer;
    grass.vertex_count = static_cast<std::uint32_t>(layout.vertices.size());
    grass.atlas = &atlas;
    grass.sampler = &sampler;

    Diagnostic diagnostic;
    PortableGrassShaderConstants constants;
    require(build_portable_grass_shader_constants(grass, constants, diagnostic),
            "grass execution constants accept bounded resources");
    require(constants.camera_position_fog ==
                std::array<float, 4U>{1.0F, 2.0F, 3.0F, 1000.0F} &&
                constants.sun_direction_fog_blend[1U] == 1.0F &&
                constants.sun_direction_fog_blend[3U] == 0.8F &&
                constants.ambient_color_wetness[3U] == 0.25F &&
                constants.fog_color_time[3U] == 4.0F &&
                std::abs(constants.wind_direction_strength[0U] - 0.6F) < 1.0e-6F &&
                std::abs(constants.wind_direction_strength[1U] - 0.8F) < 1.0e-6F &&
                constants.wind_direction_strength[2U] == 0.5F,
            "grass execution constants use the shared 160-byte ABI");

    TextureDescription target_description;
    target_description.width = 32U;
    target_description.height = 32U;
    target_description.usage = TextureUsage::color_attachment |
                               TextureUsage::transfer_source;
    target_description.mutability = TextureMutability::mutable_data;
    TestTexture target(target_description);
    TestDepthAttachment depth({32U, 32U, 1U});
    IndexedStaticMeshBatchDescription batch;
    batch.depth_attachment = &depth;
    batch.grass = grass;
    require(validate_indexed_static_mesh_batch_description(
                target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "grass-only indexed batch is accepted");

    auto no_depth_batch = batch;
    no_depth_batch.depth_attachment = nullptr;
    require(validate_indexed_static_mesh_batch_description(
                target, no_depth_batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "portable_grass_depth_attachment_missing",
            "drawable grass requires a persistent depth attachment");

    auto malformed = grass;
    malformed.elapsed_seconds = std::numeric_limits<float>::quiet_NaN();
    require(!build_portable_grass_shader_constants(
                malformed, constants, diagnostic) &&
                diagnostic.code == "portable_grass_lighting_invalid",
            "grass constants reject nonfinite time");
    malformed = grass;
    malformed.vertex_count = 5U;
    require(!build_portable_grass_shader_constants(
                malformed, constants, diagnostic) &&
                diagnostic.code == "portable_grass_vertex_count_invalid",
            "grass constants reject incomplete blades");
    malformed = grass;
    malformed.atlas = nullptr;
    require(!build_portable_grass_shader_constants(
                malformed, constants, diagnostic) &&
                diagnostic.code == "portable_grass_resource_missing",
            "drawable grass requires an atlas");

    auto alias_batch = batch;
    alias_batch.grass->atlas = &target;
    require(validate_indexed_static_mesh_batch_description(
                target, alias_batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "portable_grass_atlas_target_alias",
            "grass atlas cannot alias the color target");
    auto clip_batch = batch;
    clip_batch.grass->camera.clip_space = CameraClipSpace::d3d12;
    require(validate_indexed_static_mesh_batch_description(
                target, clip_batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "portable_grass_clip_space_mismatch",
            "grass batch rejects a backend clip-space mismatch");
}

bool executes_backend_grass_pass(const Backend backend) {
    DeviceOptions options;
    options.headless = true;
    options.enable_validation = true;
    options.allow_software = true;
    options.prefer_software = true;
    DeviceResult created = create_device(backend, options);
    if (!created.ok()) {
        std::cout << "SKIP " << backend_name(backend) << ": "
                  << created.diagnostic.code << ": "
                  << created.diagnostic.message << '\n';
        return false;
    }
    Device& device = *created.device;

    const std::array<PortableGrassVertex, 6U> vertices = {{
        {-0.45F, -0.45F, 0.5F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F,
         1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F},
        {0.45F, -0.45F, 0.5F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F,
         1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F},
        {0.45F, 0.45F, 0.5F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F,
         1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 1.0F},
        {-0.45F, -0.45F, 0.5F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F,
         1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F},
        {0.45F, 0.45F, 0.5F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F,
         1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 1.0F},
        {-0.45F, 0.45F, 0.5F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
         1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 1.0F},
    }};
    BufferDescription buffer_description;
    buffer_description.size_bytes = sizeof(vertices);
    buffer_description.usage = BufferUsage::vertex;
    buffer_description.memory = BufferMemory::device_local;
    buffer_description.mutability = BufferMutability::immutable;
    BufferResult buffer = device.create_buffer(
        buffer_description, std::as_bytes(std::span(vertices)));
    require(buffer.ok(), "backend creates portable grass vertices");

    const std::array<std::uint8_t, 16U> pixels = {
        0U, 255U, 0U, 255U, 0U, 255U, 0U, 255U,
        0U, 255U, 0U, 255U, 0U, 255U, 0U, 255U};
    TextureDescription atlas_description;
    atlas_description.width = 2U;
    atlas_description.height = 2U;
    atlas_description.format = TextureFormat::rgba8_unorm;
    atlas_description.usage = TextureUsage::sampled |
                              TextureUsage::transfer_destination;
    atlas_description.mutability = TextureMutability::immutable;
    TextureUploadPlan upload;
    upload.subresources.push_back(
        {0U, 0U, 2U, 2U, 8U,
         std::as_bytes(std::span<const std::uint8_t>(pixels))});
    TextureResult atlas = device.create_texture(atlas_description, upload);
    require(atlas.ok(), "backend uploads portable grass atlas");

    SamplerDescription sampler_description;
    sampler_description.min_filter = SamplerFilter::linear;
    sampler_description.mag_filter = SamplerFilter::linear;
    sampler_description.mip_filter = SamplerFilter::linear;
    sampler_description.address_u = SamplerAddressMode::clamp_to_edge;
    sampler_description.address_v = SamplerAddressMode::clamp_to_edge;
    SamplerResult sampler = device.create_sampler(sampler_description);
    require(sampler.ok(), "backend creates portable grass sampler");

    TextureDescription target_description;
    target_description.width = 33U;
    target_description.height = 33U;
    target_description.format = TextureFormat::rgba8_unorm;
    target_description.usage = TextureUsage::color_attachment |
                               TextureUsage::transfer_source;
    target_description.mutability = TextureMutability::mutable_data;
    TextureResult target = device.create_texture(target_description);
    require(target.ok(), "backend creates portable grass target");
    DepthAttachmentResult depth =
        device.create_depth_attachment({33U, 33U, 1U});
    require(depth.ok(), "backend creates portable grass depth target");

    PortableGrassParameters grass;
    grass.camera.clip_space = backend == Backend::Vulkan
                                  ? CameraClipSpace::vulkan
                                  : CameraClipSpace::d3d12;
    grass.camera.position = {0.0F, 0.0F, -1.0F};
    grass.sun_direction = {0.0F, 0.0F, 1.0F};
    grass.sun_color = {1.0F, 1.0F, 1.0F};
    grass.ambient_color = {0.1F, 0.1F, 0.1F};
    grass.fog_distance = 1.0e9F;
    grass.vertex_buffer = buffer.buffer.get();
    grass.vertex_count = static_cast<std::uint32_t>(vertices.size());
    grass.atlas = atlas.texture.get();
    grass.sampler = sampler.sampler.get();
    IndexedStaticMeshBatchDescription batch;
    batch.depth_attachment = depth.attachment.get();
    batch.clear_depth = true;
    batch.grass = grass;
    const IndexedStaticMeshBatchResult rendered =
        device.draw_indexed_static_mesh_batch_and_readback(
            *target.texture, batch);
    require(rendered.ok() && rendered.rgba8.size() == 33U * 33U * 4U,
            "backend executes a portable grass-only batch");
    const std::size_t center = (16U * 33U + 16U) * 4U;
    require(std::to_integer<std::uint8_t>(rendered.rgba8[center + 1U]) > 96U &&
                std::to_integer<std::uint8_t>(rendered.rgba8[center]) < 16U,
            "portable grass shader writes the expected green center pixel");
    return true;
}

} // namespace

int main() {
    try {
        static_assert(sizeof(PortableGrassVertex) == 14U * sizeof(float));
        accepts_source_triangle_and_emits_expanded_abi();
        rejects_malformed_requests();
        rejects_degenerate_and_non_upward_triangles();
        applies_budgets_and_reports_truncation();
        remains_deterministic_for_same_seed_and_inputs();
        validates_execution_contract_before_backend_allocation();
        (void)executes_backend_grass_pass(Backend::Vulkan);
#if defined(_WIN32)
        (void)executes_backend_grass_pass(Backend::D3D12);
#endif
    } catch (const std::exception& error) {
        std::fprintf(stderr, "portable grass test failure: %s\n", error.what());
        return 1;
    }
    return 0;
}
