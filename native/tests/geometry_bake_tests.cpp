#include "apex/authoring/geometry.hpp"
#include "apex/formats/kn5_bake.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

using namespace apex::authoring;
using namespace apex::formats;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Kn5Node mesh() {
    Kn5Node node;
    node.type = 2u; node.kind = "mesh"; node.name = "BODY"; node.vertexStride = 11u;
    node.vertices.resize(33u, 0.0F);
    node.vertices[0] = 0; node.vertices[1] = 0; node.vertices[2] = 0;
    node.vertices[11] = 2; node.vertices[12] = 0; node.vertices[13] = 0;
    node.vertices[22] = 0; node.vertices[23] = 2; node.vertices[24] = 0;
    for (std::size_t offset = 0; offset < node.vertices.size(); offset += 11u) {
        node.vertices[offset + 5] = 1.0F;
        node.vertices[offset + 8] = 1.0F;
    }
    node.indices = {0, 1, 2}; node.bounds = {1, 1, 0, std::sqrt(2.0F)}; node.renderable = true;
    return node;
}

Kn5Node rootWithMesh() {
    Kn5Node root;
    root.type = 1u; root.kind = "node"; root.name = "root"; root.active = true;
    root.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    root.children.push_back(mesh());
    return root;
}

Kn5File model() {
    Kn5File file; file.magic = "sc6969"; file.version = 6u;
    file.textures.push_back({true, "body.dds", 3u, {1, 2, 3}, {}});
    Kn5Material material; material.name = "Body"; material.shader = "ksPerPixel";
    material.resources.push_back({"txDiffuse", 21u, "body.dds"});
    file.materials.push_back(material); file.root = rootWithMesh();
    file.root.children[0].materialId = 0u;
    return file;
}

void geometryMetricsAndTransforms() {
    auto node = mesh();
    const auto metrics = static_geometry_metrics(node);
    require(metrics.vertices == 3u && metrics.triangles == 1u && metrics.bounds.center[0] == 1.0F,
            "static geometry metrics");
    Matrix4 translate = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1};
    const auto transformed = transform_static_geometry(node, translate, node.vertices);
    require(transformed.vertices[0] == 2.0F && transformed.vertices[1] == 3.0F &&
                transformed.vertices[2] == 4.0F && transformed.bounds[0] == 3.0F &&
                transformed.bounds[1] == 4.0F && transformed.bounds[2] == 4.0F,
            "bounds-centered translation");

    Matrix4 mirrored = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const auto mirror = transform_static_geometry(node, mirrored, node.vertices);
    require(mirror.mirrored, "negative determinant is reported");
    require(std::abs(mirror.vertices[0] - 2.0F) < 1e-6F, "bounds-centered mirror");

    Matrix4 collapsed = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool threw = false;
    try { (void)transform_static_geometry(node, collapsed, node.vertices); }
    catch (const GeometryError& error) { threw = true; require(error.code() == "collapsed_transform", "collapsed transform code"); }
    require(threw, "collapsed transform rejection");

    auto large = mesh();
    large.vertices[0] = 3.0e38F;
    large.vertices[11] = std::numeric_limits<float>::max();
    large.vertices[22] = 3.0e38F;
    const auto largeBounds = static_geometry_bounds(large);
    const auto expectedCenter = static_cast<float>((static_cast<double>(large.vertices[0]) +
                                                    static_cast<double>(large.vertices[11])) * 0.5);
    require(std::isfinite(largeBounds.center[0]) && largeBounds.center[0] == expectedCenter,
            "bounds midpoint remains finite at large float magnitudes");
}

void topologyNormalsTangentsAndBaselines() {
    auto node = mesh();
    node.indices = {0, 1, 2, 0, 0, 1};
    GeometryEdit edit; edit.remove_degenerate = true; edit.reverse_winding = true; edit.recalculate_normals = true;
    const auto repaired = repair_static_topology(node, edit, node.vertices, node.indices);
    require(repaired.indices.size() == 3u && repaired.indices[0] == 0u && repaired.indices[1] == 2u &&
                repaired.indices[2] == 1u && repaired.vertices[5] < -0.99F,
            "degenerate removal winding and area normals");

    auto packed = mesh();
    packed.vertices[8] = std::bit_cast<float>(0xff0080ffu);
    packed.vertices[9] = 0; packed.vertices[10] = 0;
    const auto packedResult = transform_static_geometry(packed, Matrix4{2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, packed.vertices);
    require(std::bit_cast<std::uint32_t>(packedResult.vertices[8]) != 0xff0080ffu,
            "packed tangent is transformed and repacked");

    auto root = rootWithMesh();
    const auto baselines = capture_static_geometry_baselines(root);
    root.children[0].vertices[0] = 99.0F;
    GeometryEdit translate; translate.transform = Matrix4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1};
    std::vector<std::string> warnings;
    require(apply_geometry_edits(root, {{"0", translate}}, &baselines, &warnings) == 1u &&
                root.children[0].vertices[0] == 1.0F && warnings.empty(),
            "baseline restore before geometry reapply");
    require(apply_geometry_edits(root, {{"0", translate}}, &baselines, &warnings) == 1u &&
                root.children[0].vertices[0] == 1.0F,
            "baseline reapply is deterministic");

    auto atomicRoot = rootWithMesh();
    const auto atomicBaselines = capture_static_geometry_baselines(atomicRoot);
    atomicRoot.children[0].vertices[0] = 77.0F;
    GeometryEdit collapsed;
    collapsed.transform = Matrix4{0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    warnings.clear();
    require(apply_geometry_edits(atomicRoot, {{"0", collapsed}}, &atomicBaselines, &warnings) == 0u &&
                atomicRoot.children[0].vertices[0] == 77.0F && !warnings.empty(),
            "failed geometry edit is atomic and does not restore baseline partially");
    atomicRoot.children[0].vertices[0] = 88.0F;
    require(apply_geometry_edits(atomicRoot, {}, &atomicBaselines, &warnings) == 0u &&
                atomicRoot.children[0].vertices[0] == 88.0F,
            "empty geometry transaction does not restore baselines");

    bool threw = false;
    try { (void)capture_static_geometry_baselines(root, GeometryLimits{1u}); }
    catch (const GeometryError& error) { threw = true; require(error.code() == "output_limit", "baseline limit code"); }
    require(threw, "baseline capture enforces its preflight budget");
}

void geometryWarningsAndBakeCopy() {
    auto root = rootWithMesh();
    Kn5Node skinned; skinned.type = 3u; skinned.kind = "skinnedMesh"; skinned.name = "SKIN"; skinned.vertexStride = 19u;
    root.children.push_back(skinned);
    std::vector<std::string> warnings;
    GeometryEdit edit; edit.reverse_winding = true;
    require(apply_geometry_edits(root, {{"1", edit}, {"missing", edit}}, nullptr, &warnings) == 0u &&
                warnings.size() == 2u, "skinned and missing geometry diagnostics");
    bool threw = false;
    try { (void)apply_geometry_edits(root, {{"01", edit}}, nullptr, &warnings); }
    catch (const GeometryError& error) { threw = true; require(error.code() == "invalid_path", "geometry path code"); }
    require(threw, "geometry rejects numeric path aliases");

    const auto source = model();
    Kn5BakeProject project;
    project.materials["body"].shader = "ksPerPixelNM";
    project.materials["body"].blend_mode = 1u;
    project.materials["body"].properties["ksDiffuse"] = {0.25F};
    project.materials["body"].resources["txDiffuse"].texture = "BODY.DDS";
    project.materials["body"].resources["txDiffuse"].file = "external.dds";
    project.materials["body"].cull_mode = "NONE";
    project.meshes["body"].transparent = true;
    project.nodes["root"].active = false;
    GeometryEdit transform; transform.transform = Matrix4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1};
    project.geometry["0"] = transform;
    const auto baked = bakeKn5(source, project);
    require(source.materials[0].shader == "ksPerPixel" && source.root.children[0].transparent == false,
            "bake does not mutate source");
    require(baked.model.materials[0].shader == "ksPerPixelNM" && baked.model.materials[0].blendMode == 1u &&
                baked.model.root.active == false && baked.model.root.children[0].transparent &&
                baked.applied.materials == 1u && baked.applied.geometry == 1u,
            "bake material hierarchy and geometry edits");
    require(!baked.warnings.empty(), "unsupported CSP edit diagnostic");
    require(baked.model.materials[0].resources[0].texture == "body.dds" &&
                baked.model.materials[0].resources[0].textureId == 21u,
            "embedded texture takes precedence over CSP-only resource metadata");

    auto protectedModel = source; protectedModel.encryption = Kn5EncryptionInspection{};
    threw = false;
    try { (void)bakeKn5(protectedModel, project); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "protected_payload", "protected bake code"); }
    require(threw, "protected geometry/material edits rejected");

    Kn5BakeProject baselineOnly;
    baselineOnly.baselines = capture_static_geometry_baselines(source.root);
    const auto protectedBaseline = bakeKn5(protectedModel, baselineOnly);
    require(protectedBaseline.model.root.children[0].vertices == protectedModel.root.children[0].vertices,
            "protected baseline metadata does not trigger an edit");

    apex::core::ParseLimits tiny;
    tiny.maxOutputBytes = 1u;
    threw = false;
    try { (void)bakeKn5(source, {}, tiny); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "output_limit", "bake preflight limit code"); }
    require(threw, "bake output budget is checked before copying the model");

    Kn5BakeProject bad; bad.geometry["0"] = transform;
    auto badSource = source; badSource.root.children[0].vertices[0] = std::numeric_limits<float>::quiet_NaN();
    threw = false;
    try { (void)bakeKn5(badSource, bad); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "non_finite", "non-finite bake code"); }
    require(threw, "non-finite source rejected");

    Kn5BakeProject ambiguous;
    ambiguous.materials["Body"].shader = "one";
    ambiguous.materials["body"].shader = "two";
    threw = false;
    try { (void)bakeKn5(source, ambiguous); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "ambiguous_key", "bake collision code"); }
    require(threw, "case-insensitive bake keys are rejected");

    auto ambiguousSource = source;
    ambiguousSource.materials.push_back(ambiguousSource.materials.front());
    ambiguousSource.materials.back().name = "body";
    Kn5BakeProject materialEdit;
    materialEdit.materials["Body"].shader = "ksPerPixelNM";
    threw = false;
    try { (void)bakeKn5(ambiguousSource, materialEdit); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "ambiguous_key", "source material collision code"); }
    require(threw, "case-insensitive source material names are rejected before editing");

    ambiguousSource = source;
    ambiguousSource.textures.push_back(ambiguousSource.textures.front());
    ambiguousSource.textures.back().name = "BODY.DDS";
    materialEdit.materials["Body"].resources["txDiffuse"].texture = "body.dds";
    threw = false;
    try { (void)bakeKn5(ambiguousSource, materialEdit); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "ambiguous_key", "source texture collision code"); }
    require(threw, "case-insensitive source texture names are rejected before editing");

    Kn5BakeProject nonCanonical;
    nonCanonical.nodes["01"].active = false;
    threw = false;
    try { (void)bakeKn5(source, nonCanonical); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "invalid_path", "bake path code"); }
    require(threw, "non-canonical bake paths are rejected");

    Kn5BakeProject nonCanonicalBaseline;
    nonCanonicalBaseline.geometry["0"].reverse_winding = true;
    nonCanonicalBaseline.baselines["01"] = {};
    threw = false;
    try { (void)bakeKn5(source, nonCanonicalBaseline); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "invalid_path", "bake baseline path code"); }
    require(threw, "non-canonical baseline paths are rejected");

    Kn5BakeProject oversizedWarning;
    oversizedWarning.warnings.push_back("adapter warning");
    auto warningLimits = apex::core::ParseLimits{};
    warningLimits.maxStringBytes = 4u;
    threw = false;
    try { (void)bakeKn5(source, oversizedWarning, warningLimits); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "string_limit", "bake warning limit code"); }
    require(threw, "oversized carried bake warnings are rejected before allocation");

    Kn5BakeProject malformedBaseline;
    malformedBaseline.geometry["0"].reverse_winding = true;
    malformedBaseline.baselines["0"].vertices = {1.0F};
    threw = false;
    try { (void)bakeKn5(source, malformedBaseline); }
    catch (const Kn5BakeError& error) { threw = true; require(error.code() == "vertex_stride", "bake baseline validation code"); }
    require(threw, "malformed baseline data is rejected before bake allocation");
}

} // namespace

int main() {
    try {
        geometryMetricsAndTransforms();
        topologyNormalsTangentsAndBaselines();
        geometryWarningsAndBakeCopy();
        std::cout << "geometry bake tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "geometry bake tests failed: " << error.what() << '\n';
        return 1;
    }
}
