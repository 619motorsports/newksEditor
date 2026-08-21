#include "apex/render/frame_plan.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::render::FrameGraphError;
using apex::render::FrameGraphErrorCode;
using apex::render::FramePass;
using apex::render::FramePassGraph;
using apex::render::FramePassKind;
using apex::render::FramePlanOptions;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

apex::scene::SceneSnapshot transparent_scene() {
    apex::scene::SceneSnapshot scene;
    scene.workspace_kind = "track";
    const auto material = scene.add_material({"opaque", "ksPerPixel", apex::scene::BlendMode::opaque});
    const auto glass = scene.add_material({"glass", "ksWindscreen", apex::scene::BlendMode::alpha_blend});
    apex::scene::SceneNode root_node;
    root_node.name = "root";
    const auto root = scene.add_node(root_node);
    apex::scene::SceneNode body;
    body.name = "body";
    body.kind = apex::scene::NodeKind::mesh;
    body.material = material;
    (void)scene.add_node(body, root);
    apex::scene::SceneNode windshield;
    windshield.name = "windshield";
    windshield.kind = apex::scene::NodeKind::mesh;
    windshield.transparent = true;
    windshield.material = glass;
    (void)scene.add_node(windshield, root);
    return scene;
}

std::vector<std::string> names(const FramePassGraph& graph) {
    std::vector<std::string> result;
    for (const FramePass& pass : graph.passes) result.push_back(pass.name);
    return result;
}

bool has_diagnostic(const apex::render::FramePlan& plan, const std::string& code,
                    const std::string& effect) {
    return std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(),
                       [&](const auto& diagnostic) {
                           return diagnostic.code == code && diagnostic.effect == effect;
                       });
}

void test_pass_order_and_resources() {
    const auto plan = apex::render::build_frame_plan(transparent_scene());
    require((names(plan.graph) == std::vector<std::string>{
                                 "local_shadow", "reflection_capture", "directional_shadow", "sky",
                                 "opaque", "opaque_scene_resolve", "transparent", "hdr_resolve",
                                 "bloom", "postprocess"}), "frame pass order");
    const auto opaque = std::find_if(plan.graph.passes.begin(), plan.graph.passes.end(),
                                     [](const FramePass& pass) { return pass.name == "opaque"; });
    require(opaque != plan.graph.passes.end(), "opaque pass exists");
    require(std::find(opaque->writes.begin(), opaque->writes.end(),
                      apex::render::FrameResourceKind::depth_buffer) != opaque->writes.end(),
            "opaque depth output");
    const auto transparent = std::find_if(plan.graph.passes.begin(), plan.graph.passes.end(),
                                          [](const FramePass& pass) { return pass.name == "transparent"; });
    require(transparent != plan.graph.passes.end(), "transparent pass exists");
    require(std::find(transparent->dependencies.begin(), transparent->dependencies.end(),
                      "opaque_scene_resolve") != transparent->dependencies.end(),
            "transparent resolve dependency");
    require(std::find(transparent->reads.begin(), transparent->reads.end(),
                      apex::render::FrameResourceKind::opaque_scene_color) != transparent->reads.end(),
            "transparent scene color input");
    require(!plan.shader_execution_supported, "shader execution remains staged");
    require(has_diagnostic(plan, "shader_execution_staged", "transparent_lighting"),
            "staged shader diagnostic");
}

void test_disabled_and_capability_gating() {
    FramePlanOptions options;
    options.include_local_shadows = false;
    options.include_refraction = false;
    options.include_bloom = false;
    options.capabilities.reflection_capture = false;
    const auto plan = apex::render::build_frame_plan(transparent_scene(), options);
    const auto plan_names = names(plan.graph);
    require(std::find(plan_names.begin(), plan_names.end(), "local_shadow") == plan_names.end(),
            "disabled local shadow pass");
    require(has_diagnostic(plan, "effect_disabled", "local_shadows"), "local shadow diagnostic");
    require(has_diagnostic(plan, "capability_unavailable", "reflection_capture"),
            "reflection capability diagnostic");
    require(has_diagnostic(plan, "effect_disabled", "refraction_scene_resolve"),
            "refraction diagnostic");
    require(has_diagnostic(plan, "effect_disabled", "bloom"), "bloom diagnostic");
}

void test_graph_errors_and_stable_order() {
    FramePassGraph graph;
    graph.passes = {
        {"last", FramePassKind::postprocess, {"first"}, {}, {}, false},
        {"first", FramePassKind::sky, {}, {}, {}, false},
        {"middle", FramePassKind::opaque, {"first"}, {}, {}, false},
    };
    const auto ordered = apex::render::order_frame_pass_graph(graph);
    require((names(ordered) == std::vector<std::string>{"first", "last", "middle"}),
            "stable graph order");

    FramePassGraph cycle;
    cycle.passes = {
        {"a", FramePassKind::sky, {"b"}, {}, {}, false},
        {"b", FramePassKind::opaque, {"a"}, {}, {}, false},
    };
    try {
        (void)apex::render::order_frame_pass_graph(cycle);
        require(false, "cycle must fail");
    } catch (const FrameGraphError& error) {
        require(error.code() == FrameGraphErrorCode::cycle, "cycle error code");
    }

    FramePassGraph limited;
    limited.passes = {{"a", FramePassKind::sky, {}, {}, {}, false}};
    apex::render::FramePlanLimits limits;
    limits.max_passes = 0;
    try {
        (void)apex::render::order_frame_pass_graph(limited, limits);
        require(false, "pass limit must fail");
    } catch (const FrameGraphError& error) {
        require(error.code() == FrameGraphErrorCode::too_many_passes, "pass limit error code");
    }

    FramePassGraph too_many_resources;
    too_many_resources.passes = {{"a", FramePassKind::sky, {},
                                  {apex::render::FrameResourceKind::hdr_ms_color},
                                  {apex::render::FrameResourceKind::backbuffer}, false}};
    limits = {};
    limits.max_resources = 1;
    try {
        (void)apex::render::order_frame_pass_graph(too_many_resources, limits);
        require(false, "resource limit must fail");
    } catch (const FrameGraphError& error) {
        require(error.code() == FrameGraphErrorCode::too_many_resources,
                "resource limit error code");
    }

    FramePassGraph too_many_dependencies;
    too_many_dependencies.passes = {
        {"a", FramePassKind::sky, {}, {}, {}, false},
        {"b", FramePassKind::opaque, {"a"}, {}, {}, false},
    };
    limits = {};
    limits.max_dependencies = 0;
    try {
        (void)apex::render::order_frame_pass_graph(too_many_dependencies, limits);
        require(false, "dependency limit must fail");
    } catch (const FrameGraphError& error) {
        require(error.code() == FrameGraphErrorCode::too_many_dependencies,
                "dependency limit error code");
    }

    FramePassGraph missing_resource;
    missing_resource.passes = {{"post", FramePassKind::postprocess, {},
                                {apex::render::FrameResourceKind::bloom_chain},
                                {apex::render::FrameResourceKind::backbuffer}, false}};
    try {
        (void)apex::render::order_frame_pass_graph(missing_resource);
        require(false, "missing resource must fail");
    } catch (const FrameGraphError& error) {
        require(error.code() == FrameGraphErrorCode::missing_resource,
                "missing resource error code");
    }

    FramePassGraph external_resource;
    external_resource.resources = {
        {apex::render::FrameResourceKind::bloom_chain, true},
    };
    external_resource.passes = {{"post", FramePassKind::postprocess, {},
                                 {apex::render::FrameResourceKind::bloom_chain},
                                 {apex::render::FrameResourceKind::backbuffer}, false}};
    (void)apex::render::order_frame_pass_graph(external_resource);
}

}  // namespace

int main() {
    try {
        test_pass_order_and_resources();
        test_disabled_and_capability_gating();
        test_graph_errors_and_stable_order();
        std::cout << "Frame plan tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "Frame plan tests failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
