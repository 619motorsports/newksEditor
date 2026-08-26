#pragma once

#include "apex/formats/fbx.hpp"
#include "apex/formats/ksanim.hpp"
#include "apex/scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace apex::formats {

struct FbxStaticMesh {
    std::int64_t object_id = 0;
    std::string name;
    std::vector<float> positions;
    std::vector<std::uint32_t> triangle_indices;
    // When an FBX UV layer is supported, this is two floats per emitted
    // vertex. Face-varying seams are represented by expanded vertices.
    std::vector<float> uvs;
};

struct FbxNodeTransform {
    scene::NodeId node = scene::invalid_node_id;
    scene::Matrix4 local = scene::identity_matrix;
    scene::Matrix4 world = scene::identity_matrix;
};

struct FbxNodeGeometry {
    scene::NodeId node = scene::invalid_node_id;
    std::uint32_t mesh = 0;
};

// A bounded source record for a file texture connected to an FBX material.
// The converter does not read this file. A later application adapter must use
// an explicit AssetSource grant and must apply the recovered channel order.
struct FbxMaterialFileTextureCandidate {
    scene::MaterialId material = scene::invalid_material_id;
    std::int64_t texture_object_id = 0;
    std::string channel;
    // ksEditor discards the directory from FbxFileTexture::GetFileName before
    // it searches its configured texture folders.
    std::string basename;
    // This is the zero-based index in the parsed Connections table. The
    // converter uses it to retain source-object order within one channel.
    std::size_t connection_order = 0;
};

// The native animation bridge intentionally exposes only the bounded subset
// whose FBX evidence is available: local transform channels with explicit
// linear key interpolation.  Unsupported curve interpolation/channel data is
// reported in diagnostics and is never silently approximated.
struct FbxAnimationClip {
    std::string name;
    double duration = 0.0;
    std::size_t source_track_count = 0;
    KsAnimation animation;
};

enum class FbxConversionSeverity { warning, error };

struct FbxConversionDiagnostic {
    FbxConversionSeverity severity = FbxConversionSeverity::warning;
    std::string code;
    std::string message;
    std::string path;
};

struct FbxSceneConversion {
    scene::SceneSnapshot snapshot;
    std::vector<FbxStaticMesh> meshes;
    std::vector<FbxNodeTransform> transforms;
    std::vector<FbxNodeGeometry> node_geometry;
    std::vector<FbxMaterialFileTextureCandidate> file_texture_candidates;
    std::vector<FbxAnimationClip> animations;
    std::vector<FbxConversionDiagnostic> diagnostics;
    bool complete = true;
};

struct FbxConversionLimits {
    std::size_t max_nodes = 1'000'000;
    std::size_t max_materials = 1'000'000;
    std::size_t max_textures = 1'000'000;
    std::size_t max_texture_references = 1'000'000;
    std::size_t max_meshes = 1'000'000;
    std::size_t max_vertices = 10'000'000;
    std::size_t max_indices = 30'000'000;
    std::size_t max_connections = 4'000'000;
    std::size_t max_properties = 4'000'000;
    std::size_t max_property_values = 10'000'000;
    std::size_t max_depth = 256;
    std::size_t max_string_bytes = 1u * 1024u * 1024u;
    std::size_t max_diagnostics = 10'000;
    std::size_t max_diagnostic_bytes = 16u * 1024u * 1024u;
    std::size_t max_diagnostic_path_bytes = 1u * 1024u * 1024u;
    std::size_t max_output_bytes = 512u * 1024u * 1024u;
    std::size_t max_animation_stacks = 64u;
    std::size_t max_animation_layers = 256u;
    std::size_t max_animation_curves = 1'000'000u;
    std::size_t max_animation_tracks = 100'000u;
    std::size_t max_animation_keys = 10'000'000u;
    std::size_t max_animation_clips = 64u;
    std::size_t max_animation_frames = 100u;
    std::size_t max_animation_merged_frames = 10'000'000u;
};

class FbxConversionError final : public std::runtime_error {
public:
    explicit FbxConversionError(FbxConversionDiagnostic diagnostic);
    [[nodiscard]] const FbxConversionDiagnostic& diagnostic() const noexcept { return diagnostic_; }

private:
    FbxConversionDiagnostic diagnostic_;
};

[[nodiscard]] FbxSceneConversion convertFbxScene(const FbxDocument& document,
                                                 FbxConversionLimits limits = {});

struct FbxConversionCapabilityDetail {
    bool static_geometry = true;
    bool node_transforms = true;
    bool material_assignment = true;
    bool external_texture_references = false;
    bool skinning = false;
    bool animation = false;
    bool images = false;
    bool layer_mappings = false;
    std::string diagnostic;
};

[[nodiscard]] FbxConversionCapabilityDetail fbxSceneConversionCapability();

inline FbxSceneConversion convert_fbx_scene(const FbxDocument& document,
                                            FbxConversionLimits limits = {}) {
    return convertFbxScene(document, std::move(limits));
}

}  // namespace apex::formats
