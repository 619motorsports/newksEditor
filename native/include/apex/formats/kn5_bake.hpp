#pragma once

#include "apex/authoring/geometry.hpp"
#include "apex/core/parse_limits.hpp"
#include "apex/formats/kn5.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace apex::formats {

struct Kn5BakeResourceEdit {
    std::optional<std::uint32_t> bind_point;
    std::string texture;
    std::string file;
    std::optional<std::array<float, 4>> color;
};

struct Kn5BakeMaterialEdit {
    std::optional<std::string> shader;
    std::optional<std::uint32_t> blend_mode;
    std::optional<std::uint32_t> depth_mode;
    std::optional<std::string> cull_mode;
    std::map<std::string, std::vector<float>> properties;
    std::map<std::string, Kn5BakeResourceEdit> resources;
};

struct Kn5BakeMeshEdit {
    std::optional<bool> transparent;
    std::optional<bool> cast_shadows;
    std::optional<std::uint32_t> layer;
    std::optional<float> lod_in;
    std::optional<float> lod_out;
};

struct Kn5BakeNodeEdit {
    std::optional<std::string> name;
    std::optional<bool> active;
    std::optional<Kn5Matrix4> transform;
};

struct Kn5BakeProject {
    std::map<std::string, Kn5BakeMaterialEdit> materials;
    std::map<std::string, Kn5BakeMeshEdit> meshes;
    std::map<std::string, Kn5BakeNodeEdit> nodes;
    std::map<std::string, authoring::GeometryEdit> geometry;
    authoring::GeometryBaselines baselines;
};

struct Kn5BakeApplied {
    std::size_t materials = 0;
    std::size_t properties = 0;
    std::size_t resources = 0;
    std::size_t meshes = 0;
    std::size_t nodes = 0;
    std::size_t geometry = 0;
};

struct Kn5BakeResult {
    Kn5File model;
    Kn5BakeApplied applied;
    std::vector<std::string> warnings;
};

class Kn5BakeError final : public std::runtime_error {
public:
    Kn5BakeError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

[[nodiscard]] Kn5BakeResult bakeKn5(const Kn5File& source, const Kn5BakeProject& project,
                                    apex::core::ParseLimits limits = {});

[[nodiscard]] inline Kn5BakeResult bake_kn5(const Kn5File& source, const Kn5BakeProject& project,
                                            apex::core::ParseLimits limits = {}) {
    return bakeKn5(source, project, limits);
}

} // namespace apex::formats
