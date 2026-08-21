#include "apex/core/parse_error.hpp"
#include "apex/domain/driver.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::domain::DriverAssetCandidate;
using apex::domain::DriverAssetKind;
using apex::domain::DriverAssetSelectionStatus;
using apex::domain::DriverDataLimits;
using apex::formats::Kn5File;
using apex::formats::Kn5Node;
using apex::formats::KnhFile;
using apex::formats::KnhNode;
using apex::formats::KsAnimation;
using apex::formats::KsAnimationFrame;
using apex::formats::KsAnimationTrack;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::array<float, 16> identity(float x = 0.0F, float y = 0.0F, float z = 0.0F) {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
}

Kn5Node node(std::string name, std::string kind = "node") {
    Kn5Node result;
    result.name = std::move(name);
    result.kind = std::move(kind);
    result.transform = identity();
    return result;
}

Kn5File model_fixture() {
    Kn5File model;
    model.source = "driver.kn5";
    model.root = node("ROOT");
    auto cockpit = node("Cockpit");
    cockpit.children.push_back(node("Driver:RIG_HEAD"));
    cockpit.children.push_back(node("Driver:RIG_HAND_L"));
    cockpit.children.push_back(node("Driver:RIG_HAND_R"));
    cockpit.children.push_back(node("Driver:RIG_CENTER"));
    cockpit.children.push_back(node("Helmet", "mesh"));
    auto unrelated = node("Unrelated");
    unrelated.children.push_back(node("UnrelatedMesh", "mesh"));
    model.root.children.push_back(std::move(cockpit));
    model.root.children.push_back(std::move(unrelated));
    return model;
}

KnhNode pose_node(std::string name, float x) {
    KnhNode result;
    result.name = std::move(name);
    result.transform = identity(x, 0, 0);
    return result;
}

KnhFile pose_fixture() {
    KnhFile pose;
    pose.source = "driver_base_pos.knh";
    pose.root = pose_node("DRIVER:RIG_CENTER", 2);
    pose.root.children.push_back(pose_node("DRIVER:RIG_HEAD", 3));
    pose.root.children.push_back(pose_node("DRIVER:RIG_HAND_L", 4));
    pose.root.children.push_back(pose_node("DRIVER:RIG_HAND_R", 5));
    pose.root.children.push_back(pose_node("POSE_ONLY", 6));
    pose.node_count = 5;
    return pose;
}

KsAnimation animation_fixture() {
    KsAnimation animation;
    animation.source = "steer.ksanim";
    animation.version = 2;
    KsAnimationTrack track;
    track.name = "driver:rig_head";
    track.animated = true;
    track.frames = {
        KsAnimationFrame{{0, 0, 0, 1}, {0, 0, 0}, {1, 1, 1}},
        KsAnimationFrame{{0, 0, 0, 1}, {10, 20, 30}, {1, 1, 1}},
    };
    animation.tracks.push_back(std::move(track));
    return animation;
}

void parses_ordered_driver_configuration() {
    const auto config = apex::domain::parse_driver_config(
        "[MODEL]\nNAME=driver\nPOSITION=1, 2, 3\nNAME=driver_last\n"
        "[STEER_ANIMATION]\nNAME=steer.ksanim\nLOCK=540\n"
        "[SHIFT_ANIMATION]\nBLEND_TIME=.1\nPOSITIVE_TIME=.2\nSTATIC_TIME=.3\n"
        "NEGATIVE_TIME=.4\nPRELOAD_RPM=1200\nINVERT_SHIFTING_HANDS=1\n"
        "[HIDE_OBJECT_2]\nNAME=Helmet\n[HIDE_OBJECT_0]\nNAME=Gloves\n",
        "driver3d.ini");
    require(config.model.has_value() && config.model->name == "driver_last", "last model key wins");
    require(config.model->position == std::array<float, 3>{1, 2, 3}, "model position");
    require(config.model->section_index == 0 && config.model->line == 1, "model provenance");
    require(config.steer.has_value() && config.steer->lock == 540, "steering configuration");
    require(config.shift.has_value() && config.shift->preload_rpm == 1200 &&
                config.shift->invert_shifting_hands, "shift configuration");
    require(config.hide_objects.size() == 2 && config.hide_objects[0].name == "Helmet" &&
                config.hide_objects[1].name == "Gloves", "indexed hidden object order");
}

void reports_malformed_and_limited_configuration() {
    const auto malformed = apex::domain::parse_driver_config(
        "[MODEL]\nNAME=x\nPOSITION=1,2\n[STEER_ANIMATION]\nLOCK=nan\n");
    require(malformed.has_errors() == false, "recoverable malformed fields remain warnings");
    require(!malformed.diagnostics.empty(), "malformed fields have diagnostics");
    require(malformed.model->position == std::array<float, 3>{0, 0, 0}, "invalid vector fallback");

    auto direct = apex::formats::parse_ini("[MODEL]\nNAME=x\nPOSITION=1,2,3\n", "direct.ini");
    direct.sections[0].entries[1].value = "4,5,6";
    direct.sections[0].entries[1].typed = apex::formats::IniValue{"4,5,6", std::string("stale typed value")};
    const auto reparsed = apex::domain::parse_driver_config(direct);
    require(reparsed.model->position == std::array<float, 3>{4, 5, 6},
            "driver validates raw direct INI values");

    const auto document = apex::formats::parse_ini("[A]\nX=1\n[B]\nY=2\n", "limit.ini");
    DriverDataLimits limits;
    limits.maxSections = 1;
    bool limited = false;
    try {
        (void)apex::domain::parse_driver_config(document, limits);
    } catch (const apex::core::ParseError& error) {
        limited = error.code() == "SECTION_LIMIT";
    }
    require(limited, "driver section limit");

    DriverDataLimits name_limits;
    name_limits.maxNameBytes = 2;
    bool name_limited = false;
    try {
        (void)apex::domain::parse_driver_config(
            apex::formats::parse_ini("[MODEL]\nNAME=long_name\n", "name.ini"), name_limits);
    } catch (const apex::core::ParseError& error) {
        name_limited = error.code() == "NAME_LIMIT";
    }
    require(name_limited, "driver name limit");

    bool truncated = false;
    try {
        (void)apex::domain::parse_driver_config("[MODEL]\nNAME=x\nPOSITION=1,2,3\\", "truncated.ini");
    } catch (const apex::core::ParseError& error) {
        truncated = error.code() == "TRUNCATED_CONTINUATION";
    }
    require(truncated, "truncated continuation is rejected by the INI layer");
}

void validates_capability_relative_assets_and_selection() {
    const auto accepted = apex::domain::request_driver_asset(
        DriverAssetKind::base_pose_knh, "drivers/driver_base_pos.knh", "driver3d.ini", 4);
    require(accepted.accepted && accepted.relative_path == "drivers/driver_base_pos.knh", "safe asset request");
    require(!apex::domain::request_driver_asset(DriverAssetKind::model_kn5, "../driver.kn5").accepted,
            "asset traversal rejected");
    require(!apex::domain::request_driver_asset(DriverAssetKind::model_kn5, "driver.ksanim").accepted,
            "asset extension rejected");

    const std::vector<DriverAssetCandidate> candidates = {
        {"cars/a/Driver.KN5"}, {"cars/b/driver.kn5"}, {"cars/a/other.kn5"}};
    const auto ambiguous = apex::domain::select_driver_model_asset(candidates, "driver");
    require(ambiguous.status == DriverAssetSelectionStatus::ambiguous && ambiguous.matches.size() == 2,
            "basename ambiguity is explicit");
    const std::vector<DriverAssetCandidate> one = {{"cars/driver.kn5"}};
    const auto resolved = apex::domain::select_driver_model_asset(one, "DRIVER");
    require(resolved.status == DriverAssetSelectionStatus::resolved && resolved.selected == 0,
            "case insensitive exact basename selection");
    const auto missing = apex::domain::select_driver_model_asset(one, "other");
    require(missing.status == DriverAssetSelectionStatus::missing && !missing.diagnostics.empty(),
            "missing model is diagnosed");
    const std::vector<DriverAssetCandidate> unsafe = {{"../driver.kn5"}};
    require(apex::domain::select_driver_model_asset(unsafe, "driver").status ==
                DriverAssetSelectionStatus::missing,
            "unsafe candidate paths are ignored");
    bool candidate_limited = false;
    try {
        DriverDataLimits limits;
        limits.maxNames = 1;
        (void)apex::domain::select_driver_model_asset(candidates, "driver", limits);
    } catch (const apex::core::ParseError& error) {
        candidate_limited = error.code() == "CANDIDATE_LIMIT";
    }
    require(candidate_limited, "asset candidate limit");
}

void applies_pose_animation_and_hidden_subtrees() {
    auto model = model_fixture();
    const auto pose_result = apex::domain::apply_driver_base_pose(model, pose_fixture());
    require(pose_result.applied == 4 && pose_result.unmatched_pose.size() == 1 &&
                model.root.children[0].children[0].transform[12] == 3,
            "pose applies to matching KN5 nodes");

    const auto animation_result = apex::domain::apply_driver_animation(model, animation_fixture(), .25F);
    require(animation_result.animated_tracks == 1 && animation_result.matched_tracks == 1 &&
                animation_result.applied == 1 && model.root.children[0].children[0].transform[12] == 5,
            "animated node track is sampled and applied");
    model.root.children[0].children[4].transform[12] = 99;
    const auto hidden = apex::domain::resolve_driver_hidden_subtrees(
        model, std::vector<std::string>{"cockpit", "Helmet", "COCKPIT", "missing"});
    require(hidden.requested == 3 && hidden.matched == 2 && hidden.mesh_count == 1 &&
                hidden.matches[0].mesh_names[0] == "Helmet" && hidden.matches[1].mesh_names[0] == "Helmet" &&
                hidden.unmatched.size() == 1,
            "hidden resolution deduplicates overlapping mesh identities");
    require(model.root.children[1].children[0].transform[12] == 0,
            "hidden resolution does not mutate unrelated meshes");
}

void validates_api_boundaries_and_iterative_depth() {
    auto model = model_fixture();
    auto animation = animation_fixture();
    animation.tracks[0].frames[1].position[0] = std::numeric_limits<float>::quiet_NaN();
    const auto animation_result = apex::domain::apply_driver_animation(model, animation, .25F);
    require(animation_result.applied == 0 && !animation_result.diagnostics.empty(),
            "non-finite direct animation is diagnosed");

    const auto pose = pose_fixture();
    DriverDataLimits output_limits;
    output_limits.maxNames = 0;
    const auto pose_result = apex::domain::apply_driver_base_pose(model, pose, output_limits);
    require(!pose_result.unmatched_pose.empty() ||
                std::any_of(pose_result.diagnostics.begin(), pose_result.diagnostics.end(), [](const auto& item) {
                    return item.code == "UNMATCHED_OUTPUT_LIMIT";
                }),
            "unmatched output limit is diagnosed");

    Kn5File deep;
    deep.source = "deep.kn5";
    deep.root = node("root");
    auto* cursor = &deep.root;
    for (std::size_t index = 0; index < 3000; ++index) {
        cursor->children.push_back(node(index == 2999 ? "DEEP_TARGET" : "branch"));
        cursor = &cursor->children.back();
    }
    bool depth_limited = false;
    try {
        (void)apex::domain::resolve_driver_hidden_subtrees(
            deep, std::vector<std::string>{"DEEP_TARGET"});
    } catch (const apex::core::ParseError& error) {
        depth_limited = error.code() == "DEPTH_LIMIT";
    }
    require(depth_limited, "deep KN5 traversal is iterative and bounded");
}

void audits_required_rig_surfaces() {
    const auto config = apex::domain::parse_driver_config(
        "[MODEL]\nNAME=driver\nPOSITION=0,0,0\n[STEER_ANIMATION]\nNAME=steer.ksanim\n"
        "[SHIFT_ANIMATION]\n");
    auto model = model_fixture();
    const auto pose = pose_fixture();
    const auto animation = animation_fixture();
    const auto audit = apex::domain::audit_driver_rig(config, model, &pose, &animation, &animation);
    require(audit.pose_nodes == 5 && audit.pose_applied == 4 && audit.steering_tracks == 1 &&
                audit.steering_matched == 1 && audit.shift_tracks == 1 && audit.shift_matched == 1 &&
                !audit.has_errors(), "complete driver rig audit");
}

}  // namespace

int main() {
    try {
        parses_ordered_driver_configuration();
        reports_malformed_and_limited_configuration();
        validates_capability_relative_assets_and_selection();
        applies_pose_animation_and_hidden_subtrees();
        validates_api_boundaries_and_iterative_depth();
        audits_required_rig_surfaces();
        std::cout << "Driver tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Driver tests failed: " << error.what() << '\n';
        return 1;
    }
}
