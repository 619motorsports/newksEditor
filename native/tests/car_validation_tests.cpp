#include "apex/domain/car_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using apex::domain::CarFindingSeverity;
using apex::domain::CarLodHierarchyInput;
using apex::domain::CarValidationLimits;
using apex::formats::Kn5File;
using apex::formats::Kn5Node;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

Kn5Node node(std::string name, std::array<float, 3> translation = {}) {
    Kn5Node result;
    result.type = 1;
    result.kind = "node";
    result.name = std::move(name);
    result.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                        translation[0], translation[1], translation[2], 1};
    return result;
}

Kn5Node cube(std::string name = "COLLISION") {
    Kn5Node result;
    result.type = 2;
    result.kind = "mesh";
    result.name = std::move(name);
    result.transform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                        0, 0, 0, 1};
    result.vertexStride = 3;
    result.vertices = {
        -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1,
        -1, -1, 1, 1, -1, 1, 1, 1, 1, -1, 1, 1};
    result.indices = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
                      0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7,
                      0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2};
    result.materialId = 0;
    return result;
}

Kn5File collider_model() {
    Kn5File result;
    apex::formats::Kn5Material material;
    material.name = "collision";
    material.shader = "GL";
    result.materials.push_back(std::move(material));
    result.root = node("ROOT");
    result.root.children.push_back(cube());
    return result;
}

bool has_code(const apex::domain::CarValidationReport& report, std::string_view code,
              CarFindingSeverity severity) {
    for (const auto& finding : report.findings)
        if (finding.code == code && finding.severity == severity) return true;
    return false;
}

bool has_lod_code(const apex::domain::CarLodValidationReport& report,
                  std::string_view code, CarFindingSeverity severity) {
    for (const auto& finding : report.findings)
        if (finding.code == code && finding.severity == severity) return true;
    return false;
}

Kn5Node car_lod_root(std::string name, std::string_view omitted = {},
                     bool include_high_detail = true,
                     float left_front_offset = 0.0F) {
    Kn5Node root = node(std::move(name));
    for (const auto required : {"SUSP_LF", "SUSP_LR", "SUSP_RF", "SUSP_RR",
                                "WHEEL_LF", "WHEEL_LR", "WHEEL_RF", "WHEEL_RR",
                                "COCKPIT_LR", "STEER_LR", "DISC_LF", "DISC_LR",
                                "DISC_RF", "DISC_RR"}) {
        if (std::string_view(required) == omitted) continue;
        std::array<float, 3> position{};
        const std::string text(required);
        if (text.find("LF") != std::string::npos)
            position = {1.0F + (text == "WHEEL_LF" ? left_front_offset : 0.0F),
                        0.0F, 2.0F};
        else if (text.find("LR") != std::string::npos) position = {1.0F, 0.0F, -2.0F};
        else if (text.find("RF") != std::string::npos) position = {-1.0F, 0.0F, 2.0F};
        else if (text.find("RR") != std::string::npos) position = {-1.0F, 0.0F, -2.0F};
        root.children.push_back(node(text, position));
    }
    if (include_high_detail) {
        root.children.push_back(node("COCKPIT_HR"));
        root.children.push_back(node("STEER_HR"));
    }
    return root;
}

std::vector<std::uint8_t> read_fixture(std::string_view relative_path) {
    const std::array<std::string_view, 3U> roots = {
        "test/content/", "../test/content/", "../../test/content/"};
    for (const auto root : roots) {
        std::ifstream input(std::string(root) + std::string(relative_path),
                            std::ios::binary | std::ios::ate);
        if (!input) continue;
        const auto end = input.tellg();
        if (end < 0) continue;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
        input.seekg(0, std::ios::beg);
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (input.good() || input.eof()) return bytes;
    }
    return {};
}

void audits_closed_collider_and_geometry() {
    const auto model = collider_model();
    const auto report = apex::domain::audit_car_collider(model);
    require(report.valid() && report.meshes == 1 && report.vertices == 8 && report.triangles == 12,
            "closed collider geometry summary");
    require(report.topology.closed && report.topology.boundary_edges == 0 &&
                report.topology.non_manifold_edges == 0, "closed collider topology");
    require(report.bounds.has_value() && report.bounds->size[0] == 2.0F, "collider bounds");
}

void audits_hierarchy_axes_and_pivots() {
    Kn5File model;
    model.root = node("ROOT");
    for (const auto name : {"SUSP_LF", "SUSP_LR", "SUSP_RF", "SUSP_RR", "WHEEL_LF", "WHEEL_LR",
                            "WHEEL_RF", "WHEEL_RR", "COCKPIT_LR", "STEER_LR", "DISC_LF", "DISC_LR",
                            "DISC_RF", "DISC_RR"}) {
        std::array<float, 3> position{};
        const auto text = std::string(name);
        if (text.find("LF") != std::string::npos) position = {1, 0, 2};
        else if (text.find("LR") != std::string::npos) position = {1, 0, -2};
        else if (text.find("RF") != std::string::npos) position = {-1, 0, 2};
        else if (text.find("RR") != std::string::npos) position = {-1, 0, -2};
        model.root.children.push_back(node(text, position));
    }
    const auto report = apex::domain::audit_car_hierarchy(model);
    require(report.valid() && report.required_nodes_present == 14, "required hierarchy nodes");
    require(has_code(report, "LOD_METADATA_STAGED", CarFindingSeverity::warning), "staged LOD diagnostic");
    Kn5File wrong = model;
    wrong.root.children[4].transform[12] = -3;
    const auto wrong_report = apex::domain::audit_car_hierarchy(wrong);
    require(has_code(wrong_report, "WHEEL_AXIS", CarFindingSeverity::error), "wheel axis diagnostic");

    Kn5File duplicate = model;
    duplicate.root.children.push_back(node("WHEEL_LF"));
    duplicate.root.children.push_back(node("ZZZ"));
    duplicate.root.children.push_back(node("ZZZ"));
    duplicate.root.children.push_back(node("AAA"));
    duplicate.root.children.push_back(node("AAA"));
    const auto duplicate_report = apex::domain::audit_car_hierarchy(duplicate);
    require(has_code(duplicate_report, "DUPLICATE_NODE_NAME", CarFindingSeverity::warning),
            "duplicate hierarchy node diagnostic");
    std::size_t aaa_position = duplicate_report.findings.size();
    std::size_t zzz_position = duplicate_report.findings.size();
    for (std::size_t index = 0; index < duplicate_report.findings.size(); ++index) {
        if (duplicate_report.findings[index].node == "AAA") aaa_position = index;
        if (duplicate_report.findings[index].node == "ZZZ") zzz_position = index;
    }
    require(aaa_position < zzz_position, "duplicate hierarchy diagnostics are ordered");
    Kn5File mesh_name = model;
    apex::formats::Kn5Material material;
    material.name = "collision";
    material.shader = "GL";
    mesh_name.materials.push_back(std::move(material));
    mesh_name.root.children.push_back(cube("WHEEL_LF"));
    const auto mesh_name_report = apex::domain::audit_car_hierarchy(mesh_name);
    require(!has_code(mesh_name_report, "DUPLICATE_NODE_NAME", CarFindingSeverity::warning),
            "mesh names do not count as duplicate hierarchy nodes");
}

void audits_merged_car_lod_hierarchies() {
    Kn5File merged;
    merged.root = node("CAR_LODS");
    Kn5Node lod0 = node("lod0.kn5");
    lod0.children.push_back(car_lod_root("LOD0"));
    merged.root.children.push_back(std::move(lod0));
    Kn5Node lod1 = node("lod1.kn5");
    lod1.children.push_back(car_lod_root("LOD1", "DISC_RR", true, 0.1F));
    merged.root.children.push_back(std::move(lod1));

    const std::array<CarLodHierarchyInput, 2U> inputs = {
        CarLodHierarchyInput{0U, 0U, "lod0.kn5"},
        CarLodHierarchyInput{1U, 1U, "lod1.kn5"}};
    const auto report = apex::domain::audit_car_lod_hierarchies(merged, inputs);
    require(report.valid() && report.lods.size() == 2U &&
                report.lods[0].required_nodes_present == 14U &&
                report.lods[1].required_nodes_present == 13U,
            "merged car LOD summaries preserve manifest order");
    require(has_lod_code(report, "REQUIRED_NODE_MISSING", CarFindingSeverity::warning) &&
                has_lod_code(report, "HIGH_DETAIL_NODE_IN_REDUCED_LOD",
                             CarFindingSeverity::warning) &&
                has_lod_code(report, "PIVOT_OFFSET", CarFindingSeverity::warning) &&
                has_lod_code(report, "LOD_PIVOT_MISMATCH", CarFindingSeverity::warning),
            "merged car LOD findings match the reference workspace audit");
    require(std::all_of(report.findings.begin(), report.findings.end(),
                        [](const auto& finding) {
                            return !finding.file.empty();
                        }),
            "merged car LOD findings retain source file attribution");

    const std::array<CarLodHierarchyInput, 2U> duplicate_root = {
        CarLodHierarchyInput{0U, 0U, "lod0.kn5"},
        CarLodHierarchyInput{0U, 1U, "lod1.kn5"}};
    const auto duplicate =
        apex::domain::audit_car_lod_hierarchies(merged, duplicate_root);
    require(!duplicate.valid() &&
                has_lod_code(duplicate, "LOD_ROOT_DUPLICATE",
                             CarFindingSeverity::error),
            "duplicate merged LOD roots fail closed");

    const std::array<CarLodHierarchyInput, 1U> invalid_root = {
        CarLodHierarchyInput{9U, 0U, "missing.kn5"}};
    const auto invalid =
        apex::domain::audit_car_lod_hierarchies(merged, invalid_root);
    require(!invalid.valid() &&
                has_lod_code(invalid, "LOD_ROOT_INVALID", CarFindingSeverity::error),
            "out-of-range merged LOD root fails closed");

    CarValidationLimits limits;
    limits.maxLods = 1U;
    const auto bounded =
        apex::domain::audit_car_lod_hierarchies(merged, inputs, limits);
    require(!bounded.valid() &&
                has_lod_code(bounded, "LOD_COUNT_LIMIT", CarFindingSeverity::error),
            "merged car LOD count is bounded before hierarchy traversal");

    Kn5File minimal;
    minimal.root = node("");
    minimal.root.children.push_back(node(""));
    const std::string oversized_file(9U, 'x');
    const std::array<CarLodHierarchyInput, 1U> oversized_input = {
        CarLodHierarchyInput{0U, 0U, oversized_file}};
    CarValidationLimits file_limits;
    file_limits.maxStringBytes = 8U;
    const auto invalid_file = apex::domain::audit_car_lod_hierarchies(
        minimal, oversized_input, file_limits);
    require(!invalid_file.valid() &&
                has_lod_code(invalid_file, "LOD_FILE_INVALID",
                             CarFindingSeverity::error) &&
                invalid_file.findings.front().file.empty(),
            "oversized LOD file names fail without copying unsafe text");
}

void audits_repository_car_lods() {
    constexpr std::string_view base = "cars/619_gen6_arca_base/";
    constexpr std::array<std::string_view, 4U> files = {
        "619_gen6_fusion13.kn5", "619_gen6_fusion13_lod_b.kn5",
        "619_gen6_fusion13_lod_c.kn5", "619_gen6_fusion13_lod_d.kn5"};
    Kn5File merged;
    merged.root = node("CAR_LODS");
    std::array<CarLodHierarchyInput, files.size()> inputs{};
    for (std::size_t index = 0U; index < files.size(); ++index) {
        const auto bytes = read_fixture(std::string(base) + std::string(files[index]));
        if (bytes.empty()) return;
        apex::formats::Kn5ParseOptions options;
        options.metadataOnly = true;
        options.hierarchyOnly = true;
        auto parsed = apex::formats::parseKn5(bytes, std::string(files[index]), options);
        merged.materials.insert(merged.materials.end(),
                                std::make_move_iterator(parsed.materials.begin()),
                                std::make_move_iterator(parsed.materials.end()));
        Kn5Node wrapper = node(std::string(files[index]));
        wrapper.children.push_back(std::move(parsed.root));
        merged.root.children.push_back(std::move(wrapper));
        inputs[index] = {index, static_cast<std::uint32_t>(index), files[index]};
    }

    const auto report = apex::domain::audit_car_lod_hierarchies(merged, inputs);
    require(report.valid() && report.lods.size() == files.size(),
            "repository car LOD hierarchy audit succeeds (errors=" +
                std::to_string(report.errors) + ", warnings=" +
                std::to_string(report.warnings) + ", lods=" +
                std::to_string(report.lods.size()) + ")");
    const std::array<std::size_t, 4U> expected_required = {10U, 5U, 9U, 0U};
    for (std::size_t index = 0U; index < expected_required.size(); ++index)
        require(report.lods[index].required_nodes_present == expected_required[index],
                "repository car LOD required-node counts match JavaScript");
    require(report.errors == 0U && report.warnings == 40U,
            "repository car LOD finding counts match JavaScript");
}

void rejects_malformed_indices_and_nonfinite_values() {
    auto malformed = collider_model();
    malformed.root.children[0].indices[0] = 99;
    const auto invalid = apex::domain::audit_car_collider(malformed);
    require(!invalid.valid() && has_code(invalid, "INVALID_INDEX", CarFindingSeverity::error),
            "invalid mesh index");
    malformed = collider_model();
    malformed.root.children[0].vertices[0] = std::numeric_limits<float>::quiet_NaN();
    const auto nonfinite = apex::domain::audit_car_collider(malformed);
    require(!nonfinite.valid() && has_code(nonfinite, "NON_FINITE_VERTEX", CarFindingSeverity::error),
            "non-finite mesh vertex");
    malformed = collider_model();
    malformed.root.children[0].materialId = 3;
    const auto material = apex::domain::audit_car_collider(malformed);
    require(!material.valid() && has_code(material, "INVALID_MATERIAL_INDEX", CarFindingSeverity::error),
            "invalid material index");
    malformed = collider_model();
    malformed.root.children[0].kind = "node";
    const auto identity = apex::domain::audit_car_collider(malformed);
    require(!identity.valid() && has_code(identity, "NODE_IDENTITY", CarFindingSeverity::error),
            "node type identity");
    malformed = collider_model();
    malformed.root.transform[0] = std::numeric_limits<float>::infinity();
    const auto matrix = apex::domain::audit_car_collider(malformed);
    require(!matrix.valid() && has_code(matrix, "NON_FINITE_MATRIX", CarFindingSeverity::error),
            "non-finite node transform");
    malformed = collider_model();
    malformed.root.children[0].vertices[0] = std::numeric_limits<float>::max();
    malformed.root.children[0].vertices[9] = -std::numeric_limits<float>::max();
    const auto coordinate = apex::domain::audit_car_collider(malformed);
    require(!coordinate.valid() && has_code(coordinate, "TOPOLOGY_COORDINATE_RANGE", CarFindingSeverity::error) &&
                has_code(coordinate, "NON_FINITE_BOUNDS", CarFindingSeverity::error),
            "topology coordinate range");

    auto visual = collider_model();
    visual.root.children[0].vertices[0] = std::numeric_limits<float>::max();
    visual.root.children[0].vertices[9] = -std::numeric_limits<float>::max();
    const auto visual_bounds = apex::domain::audit_car_collider(collider_model(), &visual);
    require(visual_bounds.valid() && has_code(visual_bounds, "VISUAL_MODEL_INVALID", CarFindingSeverity::warning),
            "visual bounds overflow is diagnosed");
}

void enforces_depth_and_geometry_budgets() {
    Kn5File deep;
    deep.root = node("ROOT");
    auto* cursor = &deep.root;
    for (int index = 0; index < 8; ++index) {
        cursor->children.push_back(node("N" + std::to_string(index)));
        cursor = &cursor->children.back();
    }
    CarValidationLimits depth_limits;
    depth_limits.maxDepth = 3;
    const auto deep_report = apex::domain::audit_car_hierarchy(deep, depth_limits);
    require(!deep_report.valid() && has_code(deep_report, "DEPTH_LIMIT", CarFindingSeverity::error),
            "hierarchy depth budget");
    auto model = collider_model();
    CarValidationLimits vertex_limits;
    vertex_limits.maxVertices = 4;
    const auto vertex_report = apex::domain::audit_car_collider(model, nullptr, vertex_limits);
    require(!vertex_report.valid() && has_code(vertex_report, "VERTEX_LIMIT", CarFindingSeverity::error),
            "vertex budget");
    CarValidationLimits finding_limits;
    finding_limits.maxFindings = 1;
    const auto bounded = apex::domain::audit_car_hierarchy(deep, finding_limits);
    require(bounded.findings.size() <= 1, "finding output budget");

    Kn5File branching;
    branching.root = node("ROOT");
    branching.root.children.push_back(node("LEFT"));
    branching.root.children.push_back(node("RIGHT"));
    CarValidationLimits scheduled_limits;
    scheduled_limits.maxNodes = 2;
    const auto scheduled = apex::domain::audit_car_hierarchy(branching, scheduled_limits);
    require(!scheduled.valid() && has_code(scheduled, "NODE_SCHEDULE_LIMIT", CarFindingSeverity::error),
            "scheduled node budget");

    auto nested = collider_model();
    apex::formats::Kn5MaterialProperty property;
    property.name = "p";
    nested.materials[0].properties.push_back(property);
    CarValidationLimits property_limits;
    property_limits.maxMaterialProperties = 0;
    const auto properties = apex::domain::audit_car_collider(nested, nullptr, property_limits);
    require(!properties.valid() && has_code(properties, "MATERIAL_PROPERTY_LIMIT", CarFindingSeverity::error),
            "material property budget");

    nested = collider_model();
    apex::formats::Kn5MaterialResource resource;
    resource.slot = "tx";
    resource.textureId = 21;
    nested.materials[0].resources.push_back(resource);
    CarValidationLimits resource_limits;
    resource_limits.maxMaterialResources = 0;
    const auto resources = apex::domain::audit_car_collider(nested, nullptr, resource_limits);
    require(!resources.valid() && has_code(resources, "MATERIAL_RESOURCE_LIMIT", CarFindingSeverity::error),
            "material resource budget");
    const auto bind_report = apex::domain::audit_car_collider(nested);
    require(bind_report.valid(), "serialized shader bind point is not a texture index");

    nested = collider_model();
    nested.root.children[0].type = 3;
    nested.root.children[0].kind = "skinnedMesh";
    apex::formats::Kn5Bone bone;
    bone.name = "bone";
    nested.root.children[0].bones.push_back(bone);
    CarValidationLimits bone_limits;
    bone_limits.maxBones = 0;
    const auto bones = apex::domain::audit_car_collider(nested, nullptr, bone_limits);
    require(!bones.valid() && has_code(bones, "BONE_LIMIT", CarFindingSeverity::error),
            "mesh bone budget");
}

}  // namespace

int main() {
    try {
        audits_closed_collider_and_geometry();
        audits_hierarchy_axes_and_pivots();
        audits_merged_car_lod_hierarchies();
        audits_repository_car_lods();
        rejects_malformed_indices_and_nonfinite_values();
        enforces_depth_and_geometry_budgets();
        std::cout << "Car validation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Car validation tests failed: " << error.what() << '\n';
        return 1;
    }
}
