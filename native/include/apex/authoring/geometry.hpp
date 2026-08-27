#pragma once

#include "apex/formats/kn5.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace apex::authoring {

using Matrix4 = formats::Kn5Matrix4;

struct GeometryBounds {
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    std::array<float, 3> center{};
    std::array<float, 3> size{};
    float radius = 0.0F;
};

struct StaticGeometryMetrics {
    GeometryBounds bounds;
    std::size_t vertices = 0;
    std::size_t triangles = 0;
};

struct GeometryEdit {
    bool remove_degenerate = false;
    bool reverse_winding = false;
    bool recalculate_normals = false;
    std::optional<Matrix4> transform;
};

class GeometryError final : public std::runtime_error {
public:
    GeometryError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

struct GeometryBaseline {
    std::vector<float> vertices;
    std::vector<std::uint16_t> indices;
    std::optional<std::array<float, 4>> bounds;
};

using GeometryBaselines = std::map<std::string, GeometryBaseline>;

struct GeometryLimits {
    // Baseline capture is a potentially large copy of untrusted mesh data.
    // Keep its budget separate from the KN5 serializer budget.
    std::size_t maxOutputBytes = 256u * 1024u * 1024u;
};

[[nodiscard]] GeometryBounds static_geometry_bounds(const formats::Kn5Node& node);
[[nodiscard]] StaticGeometryMetrics static_geometry_metrics(const formats::Kn5Node& node);

[[nodiscard]] GeometryBaselines capture_static_geometry_baselines(
    const formats::Kn5Node& root, GeometryLimits limits = {});

// The returned copy uses the supplied baseline topology and vertices. It does
// not mutate node, and rejects non-static, malformed, non-finite, or out of
// range topology data.
[[nodiscard]] formats::Kn5Node repair_static_topology(
    const formats::Kn5Node& node, const GeometryEdit& edit,
    const std::vector<float>& baseline_vertices,
    const std::vector<std::uint16_t>& baseline_indices);

struct GeometryTransformResult {
    std::vector<float> vertices;
    std::array<float, 4> bounds{};
    GeometryBounds metrics;
    bool mirrored = false;
};

[[nodiscard]] GeometryTransformResult transform_static_geometry(
    const formats::Kn5Node& node, const Matrix4& transform,
    const std::vector<float>& baseline_vertices);

// Apply edits by stable root/child paths ("root", "0", "0/1"). Warnings
// are emitted for missing, non-static, or skinned targets; a failed edit does
// not partially mutate the target. Existing baselines are restored first.
std::size_t apply_geometry_edits(
    formats::Kn5Node& root, const std::map<std::string, GeometryEdit>& edits,
    const GeometryBaselines* baselines = nullptr,
    std::vector<std::string>* warnings = nullptr);

} // namespace apex::authoring
