#include "apex/render/portable_grass.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace apex::render {

namespace {

constexpr float maximum_setting = 1000.0F;
constexpr float maximum_source_coordinate = 1'000'000.0F;
constexpr float maximum_source_normal_length = 1'000.0F;
constexpr float minimum_triangle_area = 1.0e-5F;
constexpr float pi = 3.14159265358979323846F;

[[nodiscard]] PortableGrassValidationResult invalid(const char* code) noexcept {
    return {PortableGrassStatus::invalid_request, code};
}

[[nodiscard]] PortableGrassValidationResult limited(const char* code) noexcept {
    return {PortableGrassStatus::resource_limit, code};
}

[[nodiscard]] bool finite(const std::array<float, 3U>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const float component) { return std::isfinite(component); });
}

[[nodiscard]] bool finite(const std::array<float, 4U>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const float component) { return std::isfinite(component); });
}

[[nodiscard]] std::array<float, 3U> subtract(const std::array<float, 3U>& left,
                                             const std::array<float, 3U>& right) noexcept {
    return {left[0U] - right[0U], left[1U] - right[1U], left[2U] - right[2U]};
}

[[nodiscard]] std::array<float, 3U> cross(const std::array<float, 3U>& left,
                                          const std::array<float, 3U>& right) noexcept {
    return {left[1U] * right[2U] - left[2U] * right[1U],
            left[2U] * right[0U] - left[0U] * right[2U],
            left[0U] * right[1U] - left[1U] * right[0U]};
}

[[nodiscard]] float dot(const std::array<float, 3U>& left,
                        const std::array<float, 3U>& right) noexcept {
    return left[0U] * right[0U] + left[1U] * right[1U] + left[2U] * right[2U];
}

[[nodiscard]] float length(const std::array<float, 3U>& value) noexcept {
    return std::sqrt(dot(value, value));
}

[[nodiscard]] std::array<float, 3U> normalized(const std::array<float, 3U>& value,
                                               const std::array<float, 3U>& fallback) noexcept {
    const float magnitude = length(value);
    return magnitude > 1.0e-8F && std::isfinite(magnitude)
               ? std::array<float, 3U>{value[0U] / magnitude, value[1U] / magnitude,
                                       value[2U] / magnitude}
               : fallback;
}

// The JS source uses a floating-point sine hash.  A fixed-width integer hash
// avoids libm differences between Vulkan and D3D12 hosts while preserving the
// source's stable per-seed/per-salt random ordering.
[[nodiscard]] std::uint32_t hash(std::uint32_t value) noexcept {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

[[nodiscard]] float random01(const std::uint32_t seed,
                             const std::uint32_t salt) noexcept {
    return static_cast<float>(hash(seed ^ hash(salt))) /
           static_cast<float>(std::numeric_limits<std::uint32_t>::max());
}

[[nodiscard]] float lerp(const float from, const float to, const float amount) noexcept {
    return from + (to - from) * amount;
}

[[nodiscard]] PortableGrassVertex vertex(const std::array<float, 3U>& position,
                                         const std::array<float, 3U>& normal,
                                         const std::array<float, 4U>& color, const float u,
                                         const float v, const float wind,
                                         const float tip_weight) noexcept {
    return {position[0U], position[1U], position[2U], normal[0U], normal[1U], normal[2U],
            u,          v,          color[0U], color[1U], color[2U], color[3U], wind,
            tip_weight};
}

} // namespace

PortableGrassValidationResult validatePortableGrassRequest(
    const std::span<const PortableGrassSourceTriangle> triangles,
    const PortableGrassSettings& settings,
    const PortableGrassBuildOptions& options) noexcept {
    if (triangles.size() > portable_grass_max_source_triangles) {
        return limited("grass_source_triangle_limit");
    }
    if (triangles.data() == nullptr && !triangles.empty()) {
        return invalid("grass_source_span_null");
    }
    if (options.declared_triangle_count != 0U &&
        options.declared_triangle_count != triangles.size()) {
        return invalid("grass_source_span_truncated");
    }
    if (options.max_blades > portable_grass_max_blades ||
        options.max_candidates > portable_grass_max_candidates) {
        return limited("grass_generation_budget_limit");
    }
    if (options.max_blades > std::numeric_limits<std::size_t>::max() /
                                portable_grass_vertices_per_blade ||
        options.max_blades * portable_grass_vertices_per_blade >
            portable_grass_max_vertex_count) {
        return limited("grass_vertex_count_limit");
    }

    if (!std::isfinite(settings.density) || !std::isfinite(settings.height) ||
        !std::isfinite(settings.width) || !std::isfinite(settings.height_variation) ||
        !std::isfinite(settings.width_variation) || !std::isfinite(settings.angle_degrees) ||
        !std::isfinite(settings.color_factor) || !std::isfinite(settings.wind) ||
        !std::isfinite(settings.minimum_upward) || !finite(settings.color) ||
        settings.density < 0.0F || settings.density > maximum_setting ||
        settings.height < 0.0F || settings.height > maximum_setting || settings.width < 0.0F ||
        settings.width > maximum_setting || settings.height_variation < 0.0F ||
        settings.height_variation > 1.0F || settings.width_variation < 0.0F ||
        settings.width_variation > 1.0F || settings.angle_degrees < -360.0F ||
        settings.angle_degrees > 360.0F || settings.color_factor < 0.0F ||
        settings.color_factor > maximum_setting || settings.wind < -maximum_setting ||
        settings.wind > maximum_setting || settings.minimum_upward < 0.0F ||
        settings.minimum_upward > 1.0F ||
        std::any_of(settings.color.begin(), settings.color.end(),
                    [](const float component) { return component < 0.0F || component > 1.0F; })) {
        return invalid("grass_settings_invalid");
    }
    if (settings.atlas_columns == 0U || settings.atlas_rows == 0U ||
        settings.atlas_columns > portable_grass_max_atlas_dimension ||
        settings.atlas_rows > portable_grass_max_atlas_dimension ||
        static_cast<std::size_t>(settings.atlas_columns) * settings.atlas_rows >
            portable_grass_max_atlas_tiles) {
        return invalid("grass_atlas_grid_invalid");
    }
    if (settings.atlas_tile >= static_cast<std::uint32_t>(
                                  static_cast<std::size_t>(settings.atlas_columns) *
                                  settings.atlas_rows)) {
        return invalid("grass_atlas_tile_invalid");
    }
    for (const auto& triangle : triangles) {
        for (const auto& source_vertex : triangle.vertices) {
            if (!finite(source_vertex.position) || !finite(source_vertex.normal)) {
                return invalid("grass_source_vertex_nonfinite");
            }
            if (std::any_of(source_vertex.position.begin(), source_vertex.position.end(),
                            [](const float component) {
                                return std::abs(component) > maximum_source_coordinate;
                            })) {
                return limited("grass_source_coordinate_limit");
            }
            const float normal_length = length(source_vertex.normal);
            if (!std::isfinite(normal_length) || normal_length > maximum_source_normal_length) {
                return limited("grass_source_normal_limit");
            }
        }
    }
    return {};
}

PortableGrassBuildResult buildPortableGrassLayout(
    const std::span<const PortableGrassSourceTriangle> triangles,
    const PortableGrassSettings& settings,
    const PortableGrassBuildOptions& options) {
    const auto validation = validatePortableGrassRequest(triangles, settings, options);
    if (!validation.accepted()) {
        return {validation.status, validation.code, {}, {}, {triangles.size(), 0U, 0U, 0U, 0U,
                                                              0U, false}};
    }

    PortableGrassBuildResult result;
    result.diagnostics.source_triangle_count = triangles.size();
    if (triangles.empty() || options.max_blades == 0U || options.max_candidates == 0U ||
        settings.density == 0.0F || settings.height == 0.0F || settings.width == 0.0F) {
        result.status = PortableGrassStatus::empty;
        result.code = "grass_layout_empty";
        return result;
    }

    const auto blade_reserve = std::min({options.max_blades, options.max_candidates,
                                         triangles.size()});
    const auto vertex_reserve =
        blade_reserve <= std::numeric_limits<std::size_t>::max() /
                               portable_grass_vertices_per_blade
            ? blade_reserve * portable_grass_vertices_per_blade
            : 0U;
    result.blades.reserve(blade_reserve);
    result.vertices.reserve(vertex_reserve);
    const float columns = static_cast<float>(settings.atlas_columns);
    const float rows = static_cast<float>(settings.atlas_rows);
    const float cell_x = static_cast<float>(settings.atlas_tile % settings.atlas_columns);
    const float cell_y = static_cast<float>(settings.atlas_tile / settings.atlas_columns);
    const std::array<float, 4U> color = {
        std::clamp(settings.color[0U] * settings.color_factor, 0.0F, maximum_setting),
        std::clamp(settings.color[1U] * settings.color_factor, 0.0F, maximum_setting),
        std::clamp(settings.color[2U] * settings.color_factor, 0.0F, maximum_setting),
        settings.color[3U]};

    for (std::size_t triangle_index = 0U; triangle_index < triangles.size();
         ++triangle_index) {
        const auto& source = triangles[triangle_index];
        const auto edge_one = subtract(source.vertices[1U].position, source.vertices[0U].position);
        const auto edge_two = subtract(source.vertices[2U].position, source.vertices[0U].position);
        const auto geometric = cross(edge_one, edge_two);
        const float double_area = length(geometric);
        if (!(double_area > 2.0F * minimum_triangle_area)) {
            ++result.diagnostics.rejected_degenerate_triangles;
            continue;
        }
        const float upward = std::abs(geometric[1U] / double_area);
        if (!(upward > settings.minimum_upward)) {
            ++result.diagnostics.rejected_non_upward_triangles;
            continue;
        }
        ++result.diagnostics.accepted_triangle_count;

        const float area = double_area * 0.5F;
        const double exact = static_cast<double>(area) * settings.density;
        if (!std::isfinite(exact) || exact < 0.0) {
            result.diagnostics.generation_truncated = true;
            break;
        }
        const auto whole = exact >= static_cast<double>(options.max_candidates)
                               ? options.max_candidates + 1U
                               : static_cast<std::size_t>(std::floor(exact));
        const float fraction = exact >= static_cast<double>(options.max_candidates)
                                   ? 0.0F
                                   : static_cast<float>(exact - static_cast<double>(whole));
        const std::uint32_t triangle_seed =
            options.seed ^ static_cast<std::uint32_t>(triangle_index * 0x9e3779b9U);
        const std::size_t desired = whole +
                                    ((fraction > 0.0F &&
                                      random01(triangle_seed, 1U) < fraction)
                                         ? 1U
                                         : 0U);
        for (std::size_t candidate = 0U; candidate < desired; ++candidate) {
            if (result.diagnostics.candidate_count >= options.max_candidates ||
                result.blades.size() >= options.max_blades) {
                result.diagnostics.generation_truncated = true;
                break;
            }
            ++result.diagnostics.candidate_count;
            const auto candidate_seed =
                triangle_seed ^ static_cast<std::uint32_t>(candidate * 0x85ebca6bU);
            const float r1 = std::sqrt(random01(candidate_seed, 2U));
            const float r2 = random01(candidate_seed, 3U);
            const std::array<float, 3U> weights = {1.0F - r1, r1 * (1.0F - r2), r1 * r2};
            std::array<float, 3U> position{};
            std::array<float, 3U> normal{};
            for (std::size_t axis = 0U; axis < 3U; ++axis) {
                position[axis] = weights[0U] * source.vertices[0U].position[axis] +
                                 weights[1U] * source.vertices[1U].position[axis] +
                                 weights[2U] * source.vertices[2U].position[axis];
                normal[axis] = weights[0U] * source.vertices[0U].normal[axis] +
                               weights[1U] * source.vertices[1U].normal[axis] +
                               weights[2U] * source.vertices[2U].normal[axis];
            }
            normal = normalized(normal, normalized(geometric, {0.0F, 1.0F, 0.0F}));
            const float height_factor =
                lerp(1.0F - settings.height_variation, 1.0F + settings.height_variation,
                     random01(candidate_seed, 4U));
            const float width_factor =
                lerp(1.0F - settings.width_variation, 1.0F + settings.width_variation,
                     random01(candidate_seed, 5U));
            const float height = settings.height * height_factor;
            const float width = settings.width * width_factor;
            // CSP's grassShape chooses a per-blade horizontal rotation from
            // two independent random components.  The optional setting is a
            // portable authoring offset, not a replacement for that spread.
            const float random_x = random01(candidate_seed, 5U) - 0.5F;
            const float random_z = random01(candidate_seed, 15U) - 0.5F;
            const float blade_angle =
                std::atan2(random_z, random_x) + settings.angle_degrees * (pi / 180.0F);
            const float blade_angle_degrees = blade_angle * (180.0F / pi);
            const auto reference = std::abs(normal[1U]) < 0.9F
                                       ? std::array<float, 3U>{0.0F, 1.0F, 0.0F}
                                       : std::array<float, 3U>{1.0F, 0.0F, 0.0F};
            const auto unrotated_side = normalized(cross(reference, normal), {1.0F, 0.0F, 0.0F});
            const auto forward = normalized(cross(normal, unrotated_side), {0.0F, 0.0F, 1.0F});
            const std::array<float, 3U> side = {
                unrotated_side[0U] * std::cos(blade_angle) + forward[0U] * std::sin(blade_angle),
                unrotated_side[1U] * std::cos(blade_angle) + forward[1U] * std::sin(blade_angle),
                unrotated_side[2U] * std::cos(blade_angle) + forward[2U] * std::sin(blade_angle)};
            const std::array<float, 3U> top = {position[0U] + normal[0U] * height,
                                                position[1U] + normal[1U] * height,
                                                position[2U] + normal[2U] * height};
            const std::array<float, 3U> left = {position[0U] - side[0U] * width * 0.5F,
                                                 position[1U] - side[1U] * width * 0.5F,
                                                 position[2U] - side[2U] * width * 0.5F};
            const std::array<float, 3U> right = {position[0U] + side[0U] * width * 0.5F,
                                                  position[1U] + side[1U] * width * 0.5F,
                                                  position[2U] + side[2U] * width * 0.5F};
            const std::array<float, 3U> top_left = {top[0U] - side[0U] * width * 0.5F,
                                                    top[1U] - side[1U] * width * 0.5F,
                                                    top[2U] - side[2U] * width * 0.5F};
            const std::array<float, 3U> top_right = {top[0U] + side[0U] * width * 0.5F,
                                                     top[1U] + side[1U] * width * 0.5F,
                                                     top[2U] + side[2U] * width * 0.5F};
            const float u0 = cell_x / columns;
            const float u1 = (cell_x + 1.0F) / columns;
            const float v0 = cell_y / rows;
            const float v1 = (cell_y + 1.0F) / rows;
            result.blades.push_back({static_cast<std::uint32_t>(result.blades.size()),
                                     source.source_id, position, normal, height, width,
                                     blade_angle_degrees, settings.wind, settings.atlas_tile});
            result.vertices.push_back(vertex(left, normal, color, u0, v1, settings.wind, 0.0F));
            result.vertices.push_back(vertex(right, normal, color, u1, v1, settings.wind, 0.0F));
            result.vertices.push_back(vertex(top_right, normal, color, u1, v0, settings.wind, 1.0F));
            result.vertices.push_back(vertex(left, normal, color, u0, v1, settings.wind, 0.0F));
            result.vertices.push_back(vertex(top_right, normal, color, u1, v0, settings.wind, 1.0F));
            result.vertices.push_back(vertex(top_left, normal, color, u0, v0, settings.wind, 1.0F));
        }
        if (result.diagnostics.generation_truncated)
            break;
    }

    result.diagnostics.blade_count = result.blades.size();
    if (result.blades.empty()) {
        result.status = PortableGrassStatus::empty;
        result.code = result.diagnostics.generation_truncated ? "grass_budget_empty"
                                                               : "grass_layout_empty";
    } else {
        result.status = PortableGrassStatus::ready;
        result.code = result.diagnostics.generation_truncated
                          ? "grass_layout_ready_truncated"
                          : "grass_layout_ready";
    }
    return result;
}

} // namespace apex::render
