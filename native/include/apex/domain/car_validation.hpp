#pragma once

#include "apex/formats/kn5.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace apex::domain {

// These checks consume already parsed KN5 data. The parser remains
// responsible for byte-level validation; this layer bounds all traversals and
// aggregate work before computing derived audit values.
struct CarValidationLimits {
    std::size_t maxNodes = 1'000'000;
    std::size_t maxDepth = 256;
    std::size_t maxMeshes = 1'000'000;
    std::size_t maxVertices = 10'000'000;
    std::size_t maxIndices = 30'000'000;
    std::size_t maxMaterials = 100'000;
    std::size_t maxTextures = 100'000;
    std::size_t maxMaterialProperties = 1'000'000;
    std::size_t maxMaterialResources = 1'000'000;
    std::size_t maxBones = 1'000'000;
    std::size_t maxStringBytes = 1U * 1024U * 1024U;
    std::size_t maxTopologyVertices = 10'000'000;
    std::size_t maxTopologyEdges = 30'000'000;
    std::size_t maxFindings = 10'000;
};

enum class CarFindingSeverity { warning, error };

struct CarValidationFinding {
    CarFindingSeverity severity = CarFindingSeverity::warning;
    std::string code;
    std::string message;
    std::string node;
};

struct CarBounds {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
    std::array<float, 3> size{};
    std::array<float, 3> center{};
};

struct CarTopologySummary {
    std::size_t welded_vertices = 0;
    std::size_t edges = 0;
    std::size_t boundary_edges = 0;
    std::size_t non_manifold_edges = 0;
    std::size_t degenerate_triangles = 0;
    bool closed = false;
};

struct CarValidationReport {
    std::size_t nodes = 0;
    std::size_t meshes = 0;
    std::size_t vertices = 0;
    std::size_t indices = 0;
    std::size_t triangles = 0;
    std::size_t materials = 0;
    std::size_t textures = 0;
    std::size_t material_properties = 0;
    std::size_t material_resources = 0;
    std::size_t bones = 0;
    std::size_t required_nodes_present = 0;
    std::optional<CarBounds> bounds;
    CarTopologySummary topology;
    float pivot_deviation = 0.0F;
    std::vector<CarValidationFinding> findings;
    std::size_t errors = 0;
    std::size_t warnings = 0;

    [[nodiscard]] bool valid() const noexcept { return errors == 0; }
};

// Audits SDK-required hierarchy and wheel/pivot conventions. KN5 does not
// carry the browser workspace LOD annotations, so this audits the one parsed
// hierarchy as LOD 0 and reports that omission explicitly.
[[nodiscard]] CarValidationReport audit_car_hierarchy(
    const apex::formats::Kn5File& model, CarValidationLimits limits = {});

// Audits a collision KN5, optionally comparing its world bounds with the
// visual model. A null visual model is a valid request and skips that check.
[[nodiscard]] CarValidationReport audit_car_collider(
    const apex::formats::Kn5File& collider,
    const apex::formats::Kn5File* visual_model = nullptr,
    CarValidationLimits limits = {});

}  // namespace apex::domain
