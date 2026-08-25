#include "apex/app/workspace_shadow_programs.hpp"

#include <utility>

namespace apex::app {

namespace {

render::PipelineVertexLayout shadow_vertex_layout(bool skinned) {
    render::PipelineVertexLayout layout;
    layout.stride = (skinned ? 19U : 11U) * sizeof(float);
    layout.attributes = {
        {render::PipelineVertexSemantic::position,
         render::PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {render::PipelineVertexSemantic::normal,
         render::PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {render::PipelineVertexSemantic::texcoord0,
         render::PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {render::PipelineVertexSemantic::tangent,
         render::PipelineVertexAttributeFormat::float32x3, 3U, 32U},
    };
    if (skinned) {
        layout.attributes.push_back(
            {render::PipelineVertexSemantic::bone_weights,
             render::PipelineVertexAttributeFormat::float32x4, 4U, 44U});
        layout.attributes.push_back(
            {render::PipelineVertexSemantic::bone_indices,
             render::PipelineVertexAttributeFormat::float32x4, 5U, 60U});
    }
    return layout;
}

} // namespace

WorkspaceShadowPipelineResult buildWorkspaceShadowPipeline(
    std::string name, std::vector<render::PipelineShaderModule> shaders,
    render::DepthOnlyIndexedPipelineRole role) {
    render::PipelineProgram pipeline;
    pipeline.name = std::move(name);
    pipeline.shaders = std::move(shaders);
    pipeline.vertex_layout = shadow_vertex_layout(
        role == render::DepthOnlyIndexedPipelineRole::skinned);
    pipeline.targets.has_depth = true;
    pipeline.targets.depth = {
        render::PipelineRenderTargetFormat::depth32_float, 1U};
    pipeline.depth.test_enabled = true;
    pipeline.depth.write_enabled = true;
    pipeline.depth.compare = render::PipelineCompareOperation::less;
    pipeline.transform_contract =
        render::PipelineTransformContract::draw_matrices;
    if (role == render::DepthOnlyIndexedPipelineRole::
                    stock_alpha_tested_static) {
        pipeline.resources = {
            {render::PipelineResourceKind::sampled_texture,
             0U, 0U, "txDiffuse"},
            {render::PipelineResourceKind::sampler,
             0U, 3U, "samLinearShadow"},
            {render::PipelineResourceKind::uniform_buffer,
             0U, 4U, "cbMaterial"},
        };
    }

    render::Diagnostic diagnostic;
    if (!render::validate_depth_only_indexed_pipeline_contract(
            pipeline, role, diagnostic))
        return {std::move(diagnostic), std::nullopt};
    return {{}, std::move(pipeline)};
}

} // namespace apex::app
