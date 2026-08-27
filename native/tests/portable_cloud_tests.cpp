#include "apex/render/device.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace apex::render;

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

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void requireNear(float actual, float expected, float tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(std::string(message));
    }
}

void constantsAndLayout() {
    require(portable_cloud_max_count == 512U && portable_cloud_texture_count == 7U &&
                portable_cloud_vertices_per_billboard == 6U &&
                portable_cloud_vertex_float_count == 8U &&
                portable_cloud_vertex_stride_bytes == 32U &&
                portable_cloud_max_vertex_count == 3072U &&
                portable_cloud_max_vertex_bytes == 98304U,
            "cloud layout constants");
    require(sizeof(PortableCloudVertex) == 32U &&
                offsetof(PortableCloudVertex, center_x) == 8U &&
                offsetof(PortableCloudVertex, u) == 20U &&
                offsetof(PortableCloudVertex, v) == 24U &&
                offsetof(PortableCloudVertex, speed) == 28U,
            "cloud vertex ABI layout");
}

void validation() {
    require(validatePortableCloudRequest({}).accepted(), "default cloud request accepted");
    PortableCloudSettings bad{};
    bad.width = std::numeric_limits<float>::quiet_NaN();
    require(validatePortableCloudRequest(bad).status == PortableCloudStatus::invalid_request,
            "non-finite width rejected");
    bad = {};
    bad.height = std::numeric_limits<float>::infinity();
    require(validatePortableCloudRequest(bad).status == PortableCloudStatus::invalid_request,
            "non-finite height rejected");
    bad = {};
    bad.radius = -1.0F;
    require(validatePortableCloudRequest(bad).status == PortableCloudStatus::invalid_request,
            "negative radius rejected");
    bad = {};
    bad.base_speed = 1.1F;
    require(validatePortableCloudRequest(bad).status == PortableCloudStatus::invalid_request,
            "excessive speed rejected");
    bad = {};
    bad.count = portable_cloud_max_count + 1U;
    require(validatePortableCloudRequest(bad).status == PortableCloudStatus::resource_limit,
            "cloud count bounded");
    PortableCloudBuildOptions options{};
    options.world_detail = std::numeric_limits<float>::quiet_NaN();
    require(validatePortableCloudRequest({}, options).status ==
                PortableCloudStatus::invalid_request,
            "non-finite detail rejected");
    options.world_detail = 5.1F;
    require(validatePortableCloudRequest({}, options).status ==
                PortableCloudStatus::invalid_request,
            "excessive detail rejected");
    options.world_detail = 5.0F;
    options.texture_count = portable_cloud_texture_count + 1U;
    require(validatePortableCloudRequest({}, options).status ==
                PortableCloudStatus::resource_limit,
            "texture count bounded");
    require(buildPortableCloudLayout({}).status == PortableCloudStatus::ready,
            "default build ready");
    PortableCloudSettings empty{};
    empty.count = 0U;
    require(buildPortableCloudLayout(empty).status == PortableCloudStatus::empty,
            "zero count is an empty build");
    empty = {};
    empty.width = 0.0F;
    require(buildPortableCloudLayout(empty).status == PortableCloudStatus::empty,
            "zero width is an empty build");
    options = {};
    options.texture_count = 0U;
    require(buildPortableCloudLayout({}, options).status == PortableCloudStatus::empty,
            "zero textures is an empty build");
}

void deterministicSeedOneAndRuns() {
    PortableCloudSettings settings;
    settings.width = 16.0F;
    settings.height = 7.0F;
    settings.radius = 20.0F;
    settings.count = 42U;
    settings.base_speed = 0.2F;
    PortableCloudBuildOptions options;
    options.seed = 1U;
    const auto first = buildPortableCloudLayout(settings, options);
    const auto second = buildPortableCloudLayout(settings, options);
    require(first.ready() && first.billboards.size() == 42U &&
                first.vertices.size() == 252U && first.billboards == second.billboards,
            "seed-one deterministic billboards");
    require(first.vertices == second.vertices && first.texture_runs == second.texture_runs,
            "seed-one deterministic vertex stream");
    const auto& cloud = first.billboards.front();
    require(cloud.index == 0U && cloud.texture == 4U,
            "seed-one first cloud identity");
    requireNear(cloud.theta, 0.45582858F, 1.0e-5F, "seed-one first theta");
    requireNear(cloud.radius, 20.408413F, 1.0e-4F, "seed-one first radius");
    requireNear(cloud.speed, 0.1617481F, 1.0e-5F, "seed-one first speed");
    requireNear(cloud.position[1], 20.408413F, 1.0e-4F, "seed-one first position");
    for (const auto& vertex : first.vertices) {
        require(std::isfinite(vertex.corner_x) && std::isfinite(vertex.corner_y) &&
                    std::isfinite(vertex.center_x) && std::isfinite(vertex.center_y) &&
                    std::isfinite(vertex.center_z) && std::isfinite(vertex.u) &&
                    std::isfinite(vertex.v) && std::isfinite(vertex.speed),
                "cloud vertices finite");
    }
    for (std::size_t index = 0U; index < first.texture_runs.size(); ++index) {
        const auto& run = first.texture_runs[index];
        require(run.vertex_count == run.cloud_count * 6U &&
                    run.first_vertex + run.vertex_count <= first.vertices.size(),
                "cloud texture run bounds");
        if (index != 0U) {
            require(first.texture_runs[index - 1U].texture != run.texture &&
                        first.texture_runs[index - 1U].first_vertex +
                                first.texture_runs[index - 1U].vertex_count == run.first_vertex,
                    "cloud runs preserve adjacent construction order");
        }
    }
    require(first.vertices[0].corner_x == -8.0F && first.vertices[0].corner_y == -3.5F &&
                first.vertices[0].u == 0.0F && first.vertices[0].v == 1.0F &&
                first.vertices[1].corner_x == 8.0F && first.vertices[2].v == 0.0F,
            "six-vertex billboard corners");
}

void boundsAndModes() {
    PortableCloudSettings settings;
    settings.count = portable_cloud_max_count;
    PortableCloudBuildOptions options;
    options.world_detail = 5.0F;
    const auto maximum = buildPortableCloudLayout(settings, options);
    require(maximum.ready() && maximum.billboards.size() == portable_cloud_max_count &&
                maximum.vertices.size() == portable_cloud_max_vertex_count,
            "maximum cloud layout remains bounded");
    settings.base_speed = 0.0F;
    const auto stopped = buildPortableCloudLayout(settings, options);
    require(stopped.ready() && !stopped.billboards.empty(), "zero-speed layout ready");
    for (const auto& cloud : stopped.billboards) {
        require(cloud.speed == 0.0F, "zero base speed stops cloud motion");
    }
    options.world_detail = 0.0F;
    require(buildPortableCloudLayout(settings, options).status == PortableCloudStatus::empty,
            "zero world detail is empty");
    options.world_detail = 2.5F;
    settings.count = 100U;
    const auto half = buildPortableCloudLayout(settings, options);
    require(half.ready() && half.billboards.size() == 50U,
            "world detail applies recovered count multiplier");
}

void executionConstantsAndBatchValidation() {
    const PortableCloudSettings settings{};
    const auto layout = buildPortableCloudLayout(settings);
    require(layout.ready(), "cloud execution fixture layout ready");

    BufferDescription buffer_description;
    buffer_description.size_bytes = layout.vertices.size() *
                                    sizeof(PortableCloudVertex);
    buffer_description.usage = BufferUsage::vertex;
    TestBuffer buffer(buffer_description);
    TextureDescription texture_description;
    texture_description.width = 4U;
    texture_description.height = 4U;
    texture_description.usage = TextureUsage::sampled;
    TestTexture texture(texture_description);
    TestSampler sampler;

    PortableCloudParameters clouds;
    clouds.camera.clip_space = CameraClipSpace::vulkan;
    clouds.camera.position = {1.0F, 2.0F, 3.0F};
    clouds.elapsed_seconds = 4.0F;
    clouds.light_direction = {0.0F, -1.0F, 0.0F};
    clouds.light_color = {2.0F, 3.0F, 4.0F};
    clouds.ambient_color = {0.1F, 0.2F, 0.3F};
    clouds.fog_distance = 12000.0F;
    clouds.cloud_cover = 0.8F;
    clouds.cloud_cutoff = 0.6F;
    clouds.cloud_color = 1.2F;
    clouds.vertex_buffer = &buffer;
    clouds.texture_runs = layout.texture_runs;
    clouds.textures[0U] = &texture;
    clouds.sampler = &sampler;

    PortableCloudShaderConstants constants;
    Diagnostic diagnostic;
    require(build_portable_cloud_shader_constants(clouds, constants,
                                                  diagnostic),
            "cloud shader constants accept a bounded request");
    require(constants.camera_position_time ==
                std::array<float, 4U>{1.0F, 2.0F, 3.0F, 4.0F} &&
                constants.light_direction_fog[1U] == -1.0F &&
                constants.light_direction_fog[3U] == 12000.0F &&
                constants.light_color_cover[3U] == 0.8F &&
                constants.ambient_color_cutoff[3U] == 0.6F &&
                constants.cloud_color[0U] == 1.2F,
            "cloud shader constants use the shared 144-byte ABI");

    TextureDescription target_description;
    target_description.width = 32U;
    target_description.height = 32U;
    target_description.usage = TextureUsage::color_attachment |
                               TextureUsage::transfer_source;
    target_description.mutability = TextureMutability::mutable_data;
    TestTexture target(target_description);
    IndexedStaticMeshBatchDescription batch;
    batch.clouds = clouds;
    require(validate_indexed_static_mesh_batch_description(
                target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "cloud-only indexed batch is accepted");

    auto malformed = clouds;
    malformed.elapsed_seconds = std::numeric_limits<float>::quiet_NaN();
    require(!build_portable_cloud_shader_constants(malformed, constants,
                                                   diagnostic) &&
                diagnostic.code == "portable_cloud_camera_invalid",
            "cloud constants reject non-finite time");
    malformed = clouds;
    malformed.cloud_cover = 1.01F;
    require(!build_portable_cloud_shader_constants(malformed, constants,
                                                   diagnostic) &&
                diagnostic.code == "portable_cloud_lighting_invalid",
            "cloud constants reject excessive cover");
    malformed = clouds;
    malformed.sampler = nullptr;
    require(!build_portable_cloud_shader_constants(malformed, constants,
                                                   diagnostic) &&
                diagnostic.code == "portable_cloud_sampler_missing",
            "drawable cloud runs require a sampler");
    malformed = clouds;
    auto bad_runs = layout.texture_runs;
    bad_runs.front().vertex_count = 5U;
    malformed.texture_runs = bad_runs;
    require(!build_portable_cloud_shader_constants(malformed, constants,
                                                   diagnostic) &&
                diagnostic.code == "portable_cloud_run_invalid",
            "malformed cloud runs are rejected");
    batch.clouds = clouds;
    batch.clouds->camera.clip_space = CameraClipSpace::d3d12;
    require(validate_indexed_static_mesh_batch_description(
                target, batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::invalid_request &&
                diagnostic.code == "portable_cloud_clip_space_mismatch",
            "cloud batch rejects a backend clip-space mismatch");
}

bool executeBackend(const Backend backend) {
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

    constexpr std::array<std::array<float, 4U>, 6U> corners = {{
        {{-0.45F, -0.45F, 0.0F, 1.0F}},
        {{0.45F, -0.45F, 1.0F, 1.0F}},
        {{0.45F, 0.45F, 1.0F, 0.0F}},
        {{-0.45F, -0.45F, 0.0F, 1.0F}},
        {{0.45F, 0.45F, 1.0F, 0.0F}},
        {{-0.45F, 0.45F, 0.0F, 0.0F}},
    }};
    std::array<PortableCloudVertex, 6U> vertices{};
    for (std::size_t index = 0U; index < vertices.size(); ++index) {
        vertices[index] = {corners[index][0U], corners[index][1U],
                           0.0F, 0.0F, -0.5F,
                           corners[index][2U], corners[index][3U], 0.0F};
    }
    BufferDescription buffer_description;
    buffer_description.size_bytes = sizeof(vertices);
    buffer_description.usage = BufferUsage::vertex;
    buffer_description.memory = BufferMemory::device_local;
    buffer_description.mutability = BufferMutability::immutable;
    BufferResult buffer = device.create_buffer(
        buffer_description, std::as_bytes(std::span(vertices)));
    require(buffer.ok(), "backend creates portable cloud vertices");

    const std::array<std::uint8_t, 16U> pixels = {
        0U, 0U, 0U, 255U, 0U, 0U, 0U, 255U,
        0U, 0U, 0U, 255U, 0U, 0U, 0U, 255U};
    TextureDescription cloud_description;
    cloud_description.width = 2U;
    cloud_description.height = 2U;
    cloud_description.format = TextureFormat::rgba8_unorm;
    cloud_description.usage = TextureUsage::sampled |
                              TextureUsage::transfer_destination;
    cloud_description.mutability = TextureMutability::immutable;
    TextureUploadPlan upload;
    upload.subresources.push_back(
        {0U, 0U, 2U, 2U, 8U,
         std::as_bytes(std::span<const std::uint8_t>(pixels))});
    TextureResult cloud_texture =
        device.create_texture(cloud_description, upload);
    require(cloud_texture.ok(), "backend uploads a portable cloud texture");

    SamplerDescription sampler_description;
    sampler_description.min_filter = SamplerFilter::linear;
    sampler_description.mag_filter = SamplerFilter::linear;
    sampler_description.mip_filter = SamplerFilter::linear;
    sampler_description.address_u = SamplerAddressMode::repeat;
    sampler_description.address_v = SamplerAddressMode::repeat;
    sampler_description.address_w = SamplerAddressMode::repeat;
    SamplerResult sampler = device.create_sampler(sampler_description);
    require(sampler.ok(), "backend creates a portable cloud sampler");

    TextureDescription target_description;
    target_description.width = 33U;
    target_description.height = 33U;
    target_description.format = TextureFormat::rgba8_unorm;
    target_description.usage = TextureUsage::color_attachment |
                               TextureUsage::transfer_source;
    target_description.mutability = TextureMutability::mutable_data;
    TextureResult target = device.create_texture(target_description);
    require(target.ok(), "backend creates a portable cloud target");

    const std::array<PortableCloudTextureRun, 1U> runs = {
        PortableCloudTextureRun{0U, 0U, 6U, 1U}};
    PortableCloudParameters clouds;
    clouds.camera.clip_space = backend == Backend::Vulkan
                                   ? CameraClipSpace::vulkan
                                   : CameraClipSpace::d3d12;
    clouds.light_direction = {0.0F, -1.0F, 0.0F};
    clouds.cloud_cover = 1.0F;
    clouds.cloud_cutoff = 1.0F;
    clouds.cloud_color = 1.0F;
    clouds.fog_distance = 1.0e9F;
    clouds.vertex_buffer = buffer.buffer.get();
    clouds.texture_runs = runs;
    clouds.textures[0U] = cloud_texture.texture.get();
    clouds.sampler = sampler.sampler.get();
    IndexedStaticMeshBatchDescription batch;
    batch.clouds = clouds;
    const IndexedStaticMeshBatchResult rendered =
        device.draw_indexed_static_mesh_batch_and_readback(
            *target.texture, batch);
    require(rendered.ok() && rendered.rgba8.size() == 33U * 33U * 4U,
            "backend executes a portable cloud-only batch");
    bool changed = false;
    for (std::size_t offset = 0U; offset < rendered.rgba8.size(); offset += 4U) {
        changed = changed ||
                  std::to_integer<std::uint8_t>(rendered.rgba8[offset]) > 128U;
    }
    require(changed, "portable cloud shader changes covered pixels");

    IndexedStaticMeshBatchDescription empty_batch;
    empty_batch.clouds.emplace();
    empty_batch.clouds->camera.clip_space = clouds.camera.clip_space;
    const IndexedStaticMeshBatchResult empty =
        device.draw_indexed_static_mesh_batch_and_readback(
            *target.texture, empty_batch);
    require(empty.ok(), "backend accepts an intentionally empty cloud layout");
    return true;
}

} // namespace

int main() {
    try {
        constantsAndLayout();
        validation();
        deterministicSeedOneAndRuns();
        boundsAndModes();
        executionConstantsAndBatchValidation();
        (void)executeBackend(Backend::Vulkan);
#if defined(_WIN32)
        (void)executeBackend(Backend::D3D12);
#endif
        std::cout << "portable cloud tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "portable cloud tests failed: " << error.what() << '\n';
        return 1;
    }
}
