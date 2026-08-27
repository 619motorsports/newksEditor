#pragma once

#include "apex/formats/kn5.hpp"
#include "apex/render/material_profile.hpp"
#include "apex/render/render_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace apex::render {

enum class DrawPrimitiveKind : std::uint8_t { static_mesh, skinned_mesh };

inline constexpr std::uint32_t invalid_draw_texture_index = std::numeric_limits<std::uint32_t>::max();

struct DrawPacketFlags {
    bool transparent = false;
    bool blend_enabled = false;
    bool alpha_to_coverage = false;
    bool depth_test = true;
    bool depth_write = true;
    bool wireframe = false;
    bool selected = false;
    bool cast_shadows = false;
    // Stock MaterialFilterSM defaults doubleFaceShadow to false. A parsed
    // stock path therefore uses back-face culling; explicit callers may set
    // this to true to request the source-evidenced no-cull shadow pass.
    // KN5 parsing does not currently populate this field.
    bool double_face_shadow = false;
};

struct DrawResourceSlot {
    std::string slot;
    // KN5 textureId is the shader bind point. The resolved table index is
    // separate because the texture name, rather than the bind point, selects
    // the embedded KN5 texture.
    std::uint32_t bind_point = 0;
    std::uint32_t texture_index = invalid_draw_texture_index;
    // Original spelling from the resolved KN5 texture table. Empty means the
    // material declared no texture name and remains a staged/missing binding.
    std::string texture;
};

struct DrawPacket {
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    apex::scene::MaterialId material = apex::scene::invalid_material_id;
    DrawPrimitiveKind primitive = DrawPrimitiveKind::static_mesh;
    std::uint32_t vertex_offset = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    // KN5 stores the stride as a count of float32 values, not bytes.
    std::uint32_t vertex_stride_floats = 0;
    std::size_t order = 0;
    float distance = 0.0F;
    double layer = 0.0;
    // True when CSP explicitly replaced transparent classification. This is
    // needed because false must override an alpha-blended material default.
    bool transparency_overridden = false;
    apex::scene::Matrix4 world_matrix = apex::scene::identity_matrix;
    std::vector<apex::scene::Matrix4> bone_palette;
    MaterialRenderProfile material_profile;
    std::vector<DrawResourceSlot> resources;
    DrawPacketFlags flags;
    // True only for a retained native Shadowgen candidate that is absent from
    // the color plan. Color submission always suppresses this packet. Shadow
    // submission can select it through its independent prepared-index mask.
    bool shadow_only = false;
    // Shader execution and texture sampling are intentionally staged. This
    // packet is a validated command description, not evidence of pixels.
    bool shader_execution_supported = false;
};

struct DrawPacketDiagnostic {
    enum class Severity : std::uint8_t { warning, error };
    Severity severity = Severity::warning;
    std::string code;
    std::string message;
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    apex::scene::MaterialId material = apex::scene::invalid_material_id;
};

struct DrawPacketUnsupportedEffect {
    std::string code;
    std::string description;
    apex::scene::NodeId node = apex::scene::invalid_node_id;
    apex::scene::MaterialId material = apex::scene::invalid_material_id;
};

struct DrawPacketLimits {
    std::size_t max_packets = 1'000'000;
    std::size_t max_scene_nodes = 1'000'000;
    std::size_t max_scene_materials = 1'000'000;
    std::size_t max_vertices = 10'000'000;
    std::size_t max_indices = 20'000'000;
    std::size_t max_bones = 1'000'000;
    std::size_t max_resources_per_packet = 4'096;
    std::size_t max_string_bytes = 1U << 20;
    std::size_t max_diagnostics = 100'000;
    std::size_t max_unsupported_effects = 4'096;
    std::size_t max_packet_bytes = 256U * 1024U * 1024U;
    std::size_t max_total_bytes = 512U * 1024U * 1024U;
    std::size_t max_cpu_skin_bytes = 256U * 1024U * 1024U;
    std::size_t max_material_properties = 4'096;
    std::size_t max_diagnostic_bytes = 16U * 1024U * 1024U;
    std::size_t max_unsupported_effect_bytes = 16U * 1024U * 1024U;
    std::size_t max_scene_textures = 1'000'000;
};

struct DrawPacketOptions {
    bool wireframe = false;
    apex::scene::NodeId selected_node = apex::scene::invalid_node_id;
    bool include_shadow_casters = true;
};

struct DrawPacketBuildResult {
    std::vector<DrawPacket> packets;
    // Complete prepared-packet permutation in source traversal order. Empty
    // keeps prepared order for manually assembled legacy plans.
    std::vector<std::uint32_t> shadow_packet_order;
    std::vector<DrawPacketDiagnostic> diagnostics;
    std::vector<DrawPacketUnsupportedEffect> unsupported_effects;
    // supported describes acceptance of the backend-neutral packet contract;
    // unsupported_effects separately records staged shader/texture behavior.
    bool supported = true;
    bool limit_exceeded = false;
    std::size_t total_bytes = 0;
    std::size_t diagnostic_bytes = 0;
    std::size_t unsupported_effect_bytes = 0;
};

class DrawPacketError final : public std::runtime_error {
public:
    DrawPacketError(std::string code, std::string message);

    [[nodiscard]] const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

[[nodiscard]] DrawPacketBuildResult build_draw_packets(
    const apex::formats::Kn5File& model, const apex::scene::SceneSnapshot& scene,
    const RenderPlan& plan, const DrawPacketOptions& options = {},
    const DrawPacketLimits& limits = {});

[[nodiscard]] inline DrawPacketBuildResult build_draw_packets(
    const apex::formats::Kn5File& model, const apex::scene::SceneSnapshot& scene,
    const DrawPacketOptions& options, const DrawPacketLimits& limits = {}) {
    return build_draw_packets(model, scene, build_render_plan(scene), options, limits);
}

// CPU skinning follows src/skinning.js and produces the same 19-float local
// vertex stream that production WebGL uploads before drawing. It intentionally
// applies stricter validation: malformed inactive influences are rejected
// instead of ignored.
[[nodiscard]] std::vector<float> skin_vertices_reference(
    std::span<const float> vertices,
    std::span<const apex::scene::Matrix4> bone_palette,
    const apex::scene::Matrix4& mesh_world,
    const DrawPacketLimits& limits = {});

[[nodiscard]] std::vector<float> skin_vertices_reference(
    const apex::formats::Kn5Node& mesh,
    const std::map<std::string, apex::scene::Matrix4>& bone_world_by_name,
    const apex::scene::Matrix4& mesh_world,
    const DrawPacketLimits& limits = {});

[[nodiscard]] inline std::vector<float> cpu_skin_vertices(
    const apex::formats::Kn5Node& mesh,
    const std::map<std::string, apex::scene::Matrix4>& bone_world_by_name,
    const apex::scene::Matrix4& mesh_world,
    const DrawPacketLimits& limits = {}) {
    return skin_vertices_reference(mesh, bone_world_by_name, mesh_world, limits);
}

}  // namespace apex::render
