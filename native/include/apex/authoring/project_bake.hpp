#pragma once

#include "apex/authoring/project.hpp"
#include "apex/formats/kn5_bake.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace apex::authoring {

// This adapter owns the conversion boundary between the transaction model and
// the KN5 writer. It deliberately maps only node, mesh, and geometry edits. Other
// ProjectState categories remain outside the KN5 bake contract until their
// serializers are implemented.
struct ProjectBakeLimits {
    std::size_t maxNodeEdits = 10'000;
    std::size_t maxMeshEdits = 10'000;
    std::size_t maxGeometryEdits = 10'000;
    std::size_t maxBaselines = 10'000;
    std::size_t maxStringBytes = 4096;
    std::size_t maxTotalStringBytes = 16u * 1024u * 1024u;
    std::size_t maxBaselineBytes = 256u * 1024u * 1024u;
};

class ProjectBakeError final : public std::runtime_error {
public:
    ProjectBakeError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

// Build an owned KN5 bake description from a caller-owned project state and
// geometry-baseline table. Keys and optional values are copied exactly, so
// false values and zero-valued optionals are not lost or normalized.
[[nodiscard]] formats::Kn5BakeProject buildKn5BakeProject(
    const ProjectState& state, const GeometryBaselines& baselines,
    ProjectBakeLimits limits = {});

[[nodiscard]] inline formats::Kn5BakeProject build_kn5_bake_project(
    const ProjectState& state, const GeometryBaselines& baselines,
    ProjectBakeLimits limits = {}) {
    return buildKn5BakeProject(state, baselines, limits);
}

} // namespace apex::authoring
