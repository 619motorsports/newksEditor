#include "apex/formats/ini.hpp"
#include "apex/render/csp_node_state.hpp"
#include "apex/scene/kn5_scene.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

apex::formats::Kn5Node mesh(std::string name, float x) {
    apex::formats::Kn5Node result;
    result.type = 2U;
    result.kind = "mesh";
    result.name = std::move(name);
    result.active = true;
    result.visible = true;
    result.renderable = true;
    result.castShadows = true;
    result.vertexStride = 11U;
    result.materialId = 0U;
    result.vertices.resize(33U, 0.0F);
    result.vertices[0U] = x;
    result.vertices[11U] = x;
    result.vertices[22U] = x;
    result.indices = {0U, 1U, 2U};
    result.bounds = {x, 0.0F, 0.0F, 0.0F};
    return result;
}

struct Fixture {
    apex::formats::Kn5File model;
    apex::scene::SceneSnapshot scene;
};

Fixture fixture() {
    Fixture result;
    result.model.materials.resize(1U);
    result.model.materials[0U].name = "Body";
    result.model.materials[0U].shader = "ksPerPixel";
    result.model.root.type = 1U;
    result.model.root.kind = "node";
    result.model.root.name = "ROOT";
    result.model.root.active = true;
    result.model.root.transform = {
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        1.0F,
        0.0F,
        0.0F,
        0.0F,
        0.0F,
        1.0F,
    };
    result.model.root.children.push_back(mesh("MeshA", 2.0F));
    result.model.root.children.push_back(mesh("MeshB", 4.0F));
    result.scene = apex::scene::convertKn5ToScene(result.model);
    return result;
}

apex::csp::CspConfigModel config(std::string_view text) {
    return apex::csp::evaluate_csp_config(apex::formats::parse_csp_ini(text, "mesh-adjustment.ini"));
}

void resolves_source_order_and_executes_plan_state() {
    const Fixture value = fixture();
    const auto resolved = apex::render::resolve_csp_node_render_states(
        config("[MESH_ADJUSTMENT_0]\nMATERIALS=Body\nMESHES=MeshA\n"
               "IS_TRANSPARENT=1\nLAYER=5.5\nLOD_IN=1\nLOD_OUT=8\nCAST_SHADOWS=0\n"
               "[MESH_ADJUSTMENT_1]\nMESHES=MeshA\nCONDITION=ALWAYS_OFF\n"
               "LAYER=2.25\n"
               "[MESH_ADJUSTMENT_2]\nMESHES=MeshB\nLOD_IN=10\n"),
        value.model,
        value.scene);
    require(resolved.ok() && resolved.overrides.size() == 2U,
            "matching CSP mesh sections should produce one override per node");
    const auto& first = resolved.overrides[0U];
    require(first.node == 1U && first.is_transparent == true && first.layer == 2.25 && first.lod_in == 1.0 &&
                first.lod_out == 8.0 && first.cast_shadows == false,
            "later CSP sections should replace fields in source order");

    apex::render::RenderPlanOptions options;
    options.node_state_overrides = resolved.overrides;
    const auto plan = apex::render::build_render_plan(value.scene, options);
    require(plan.items.size() == 1U, "resolved CSP LOD should execute in the plan");
    require(plan.items.front().node == 1U, "resolved CSP LOD should retain the matching source node");
    require(plan.items.front().transparent, "resolved CSP transparency should execute in the plan");
    require(plan.items.front().layer == 2.25, "resolved CSP layer should execute in the plan");
    require(plan.shadow_casters.empty(), "resolved CSP shadow state should execute in the plan");
}

void diagnoses_invalid_values_and_bounds_outputs() {
    const Fixture value = fixture();
    const auto invalid = apex::render::resolve_csp_node_render_states(
        config("[MESH_ADJUSTMENT_BAD]\nMESHES=MeshA\nLAYER=nan\nLOD_IN=nope\n"), value.model, value.scene);
    require(invalid.ok() && invalid.overrides.empty() && invalid.diagnostics.size() == 2U &&
                std::all_of(invalid.diagnostics.begin(),
                            invalid.diagnostics.end(),
                            [](const auto& item) { return item.code == "INVALID_MESH_OVERRIDE" && item.node == 1U; }),
            "invalid mesh-state values should be ignored with attributed "
            "diagnostics");

    apex::render::CspNodeStateLimits limits;
    limits.max_overrides = 1U;
    const auto limited = apex::render::resolve_csp_node_render_states(
        config("[MESH_ADJUSTMENT_ALL]\nMESHES=Mesh?\nCAST_SHADOWS=0\n"), value.model, value.scene, limits);
    require(!limited.ok() && limited.limit_exceeded && limited.overrides.empty() &&
                limited.diagnostics.front().code == "CSP_NODE_STATE_OVERRIDE_LIMIT",
            "node-state output count should fail closed before partial output "
            "escapes");

    limits = {};
    limits.max_output_bytes = 1U;
    const auto byte_limited = apex::render::resolve_csp_node_render_states(
        config("[MESH_ADJUSTMENT_ALL]\nMESHES=Mesh?\nCAST_SHADOWS=0\n"), value.model, value.scene, limits);
    require(!byte_limited.ok() && byte_limited.limit_exceeded &&
                byte_limited.diagnostics.front().code == "CSP_NODE_STATE_OUTPUT_LIMIT",
            "node-state output bytes should be bounded before output allocation");
}

void rejects_malformed_model_scene_identity() {
    Fixture value = fixture();
    value.scene.nodes[1U].name = "Different";
    const auto result = apex::render::resolve_csp_node_render_states(
        config("[MESH_ADJUSTMENT_0]\nMESHES=MeshA\nLAYER=1\n"), value.model, value.scene);
    require(!result.ok() && result.overrides.empty() && result.diagnostics.front().code == "SCENE_MODEL_IDENTITY",
            "mismatched untrusted model and scene identities should fail closed");
}

}  // namespace

int main() {
    try {
        resolves_source_order_and_executes_plan_state();
        diagnoses_invalid_values_and_bounds_outputs();
        rejects_malformed_model_scene_identity();
        std::cout << "CSP node-state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CSP node-state tests failed: " << error.what() << '\n';
        return 1;
    }
}
