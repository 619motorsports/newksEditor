#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/scene/scene.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace apex::scene {

inline constexpr std::size_t default_kn5_scene_native_object_bytes =
    512U * 1024U * 1024U;
inline constexpr std::size_t default_kn5_scene_string_bytes = 1U * 1024U * 1024U;

/** A renderer-independent description of one KN5 mesh payload.
 *
 * Geometry remains owned by the parsed KN5 file. This sidecar deliberately
 * contains counts and layout only; a graphics backend can upload the source
 * arrays without making this conversion layer claim pixel-level parity.
 */
struct Kn5GeometryMetadata {
    NodeId node = invalid_node_id;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_count = 0;
    std::uint32_t triangle_count = 0;
    std::uint32_t vertex_stride = 0;
    std::uint32_t bone_count = 0;
    bool skinned = false;
};

/** Exact transformed AABB for authored preview-visible KN5 geometry.
 *
 * This matches the browser preview framing source. It excludes geometry under
 * inactive branches and geometry with authored visibility or rendering off.
 */
struct Kn5PreviewBounds {
    Vector3 minimum{};
    Vector3 maximum{};
    Vector3 center{};
    float radius = 0.0F;
};

struct Kn5SceneConversion {
    SceneSnapshot snapshot;
    std::vector<Kn5GeometryMetadata> geometry;
    std::optional<Kn5PreviewBounds> preview_bounds;
};

// Conversion limits are separate from binary KN5 parser limits. They protect
// callers that construct a Kn5File directly and bound allocations before the
// scene snapshot reserves its material/node vectors.
struct Kn5SceneLimits {
    std::size_t max_materials = 1'000'000;
    std::size_t max_nodes = 1'000'000;
    std::size_t max_depth = 1024;
    // Bound the second ownership copy from a parsed/direct Kn5File into the
    // renderer-independent snapshot. This includes native records, copied
    // strings, child links, geometry metadata, and traversal scratch.
    std::size_t max_native_object_bytes = default_kn5_scene_native_object_bytes;
    std::size_t max_string_bytes = default_kn5_scene_string_bytes;
};

class Kn5SceneError final : public std::runtime_error {
public:
    explicit Kn5SceneError(std::string message);
};

/** Convert an already validated KN5 model into the backend-neutral scene model.
 *
 * Node and material IDs are assigned in source order: materials retain their
 * serialized indices and nodes use deterministic pre-order traversal. Local
 * KN5 transforms are composed into world transforms using the same column-major
 * convention as the reference WebGL scene walk.
 */
[[nodiscard]] Kn5SceneConversion convertKn5Scene(
    const formats::Kn5File& model, const Kn5SceneLimits& limits = {});

/** Convenience view when geometry counts are not needed by the caller. */
[[nodiscard]] SceneSnapshot convertKn5ToScene(
    const formats::Kn5File& model, const Kn5SceneLimits& limits = {});

inline Kn5SceneConversion convert_kn5_scene(const formats::Kn5File& model,
                                            const Kn5SceneLimits& limits = {}) {
    return convertKn5Scene(model, limits);
}

inline SceneSnapshot convert_kn5_to_scene(const formats::Kn5File& model,
                                          const Kn5SceneLimits& limits = {}) {
    return convertKn5ToScene(model, limits);
}

}  // namespace apex::scene
