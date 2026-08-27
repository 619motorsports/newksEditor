#include "apex/render/portable_clouds.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace apex::render {

namespace {

constexpr float degrees_to_radians = 3.14159265358979323846F / 180.0F;
constexpr float random_scale = 1.0F / 32767.0F;
constexpr float maximum_setting = 1000.0F;

[[nodiscard]] PortableCloudValidationResult invalid(const char* code) noexcept {
    return {PortableCloudStatus::invalid_request, code};
}

[[nodiscard]] PortableCloudValidationResult limited(const char* code) noexcept {
    return {PortableCloudStatus::resource_limit, code};
}

class CloudRandom {
public:
    explicit CloudRandom(std::uint32_t seed) noexcept : state_(seed) {}

    [[nodiscard]] float next() noexcept {
        state_ = state_ * 214013U + 2531011U;
        return static_cast<float>((state_ >> 16U) & 0x7fffU) * random_scale;
    }

private:
    std::uint32_t state_ = 1U;
};

[[nodiscard]] PortableCloudVertex vertex(float corner_x, float corner_y,
                                         const PortableCloudBillboard& cloud,
                                         float u, float v) noexcept {
    return {corner_x, corner_y, cloud.position[0], cloud.position[1],
            cloud.position[2], u, v, cloud.speed};
}

} // namespace

PortableCloudValidationResult validatePortableCloudRequest(
    const PortableCloudSettings& settings,
    const PortableCloudBuildOptions& options) noexcept {
    if (!std::isfinite(settings.width) || !std::isfinite(settings.height) ||
        !std::isfinite(settings.radius) || !std::isfinite(settings.base_speed) ||
        settings.width < 0.0F || settings.height < 0.0F || settings.radius < 0.0F ||
        settings.width > maximum_setting || settings.height > maximum_setting ||
        settings.radius > maximum_setting || settings.base_speed < 0.0F ||
        settings.base_speed > 1.0F) {
        return invalid("cloud_settings_invalid");
    }
    if (settings.count > portable_cloud_max_count) {
        return limited("cloud_count_limit");
    }
    if (!std::isfinite(options.world_detail) || options.world_detail < 0.0F ||
        options.world_detail > 5.0F) {
        return invalid("cloud_world_detail_invalid");
    }
    if (options.texture_count > portable_cloud_texture_count) {
        return limited("cloud_texture_count_limit");
    }
    const double effective_count =
        std::floor(static_cast<double>(settings.count) *
                   static_cast<double>(options.world_detail) * 0.2);
    if (!std::isfinite(effective_count) || effective_count < 0.0 ||
        effective_count > static_cast<double>(portable_cloud_max_count)) {
        return limited("cloud_vertex_count_limit");
    }
    const auto count = static_cast<std::size_t>(effective_count);
    if (count > std::numeric_limits<std::size_t>::max() /
                    portable_cloud_vertices_per_billboard ||
        count * portable_cloud_vertices_per_billboard > portable_cloud_max_vertex_count ||
        count * portable_cloud_vertices_per_billboard >
            std::numeric_limits<std::size_t>::max() / portable_cloud_vertex_stride_bytes) {
        return limited("cloud_vertex_storage_limit");
    }
    return {};
}

PortableCloudBuildResult buildPortableCloudLayout(
    const PortableCloudSettings& settings,
    const PortableCloudBuildOptions& options) {
    const auto validation = validatePortableCloudRequest(settings, options);
    if (!validation.accepted()) {
        return {validation.status, validation.code, {}, {}, {}};
    }

    const auto count = static_cast<std::size_t>(std::floor(
        static_cast<double>(settings.count) * static_cast<double>(options.world_detail) * 0.2));
    if (count == 0U || options.texture_count == 0U || settings.width == 0.0F ||
        settings.height == 0.0F) {
        return {PortableCloudStatus::empty, "cloud_layout_empty", {}, {}, {}};
    }

    PortableCloudBuildResult result;
    result.status = PortableCloudStatus::ready;
    result.code = "cloud_layout_ready";
    result.billboards.reserve(count);
    result.vertices.reserve(count * portable_cloud_vertices_per_billboard);
    result.texture_runs.reserve(std::min<std::size_t>(count, options.texture_count));

    const float half_width = settings.width * 0.5F;
    const float half_height = settings.height * 0.5F;
    constexpr std::array<std::array<float, 4U>, 6U> corners = {{
        {{-1.0F, -1.0F, 0.0F, 1.0F}},
        {{1.0F, -1.0F, 1.0F, 1.0F}},
        {{1.0F, 1.0F, 1.0F, 0.0F}},
        {{-1.0F, -1.0F, 0.0F, 1.0F}},
        {{1.0F, 1.0F, 1.0F, 0.0F}},
        {{-1.0F, 1.0F, 0.0F, 0.0F}},
    }};

    CloudRandom random(options.seed);
    for (std::size_t index = 0U; index < count; ++index) {
        const float index_float = static_cast<float>(index);
        const float count_float = static_cast<float>(count);
        const float phi = ((random.next() - 0.5F) * 10.0F + 360.0F / count_float) *
                          degrees_to_radians * index_float;
        const float band = ((random.next() - 0.5F) * 5.0F + 15.0F) *
                           degrees_to_radians * static_cast<float>((index + 1U) % 5U);
        const float theta = ((random.next() - 0.5F) * 30.0F + 20.0F) *
                                degrees_to_radians + band;
        const float cloud_radius = (1.0F - std::cos(theta)) * 4.0F + settings.radius;
        const float speed = settings.base_speed == 0.0F
                                ? 0.0F
                                : std::max(0.0005F, std::min(1.0F,
                                      random.next() * settings.base_speed));
        const auto texture = static_cast<std::uint32_t>(std::min(
            static_cast<double>(options.texture_count - 1U),
            std::floor(static_cast<double>(random.next()) * options.texture_count)));
        const float ring = cloud_radius * std::sin(phi);
        PortableCloudBillboard cloud;
        cloud.index = static_cast<std::uint32_t>(index);
        cloud.phi = phi;
        cloud.theta = theta;
        cloud.radius = cloud_radius;
        cloud.speed = speed;
        cloud.texture = texture;
        cloud.position = {ring * std::cos(theta), cloud_radius * std::cos(phi),
                          -ring * std::sin(theta)};
        result.billboards.push_back(cloud);

        const auto vertex_start = static_cast<std::uint32_t>(result.vertices.size());
        auto& run = result.texture_runs;
        if (run.empty() || run.back().texture != texture) {
            run.push_back({texture, vertex_start, 0U, 0U});
        }
        run.back().vertex_count += portable_cloud_vertices_per_billboard;
        ++run.back().cloud_count;
        for (const auto& corner : corners) {
            result.vertices.push_back(vertex(corner[0] * half_width,
                                              corner[1] * half_height, cloud,
                                              corner[2], corner[3]));
        }
    }
    return result;
}

} // namespace apex::render
