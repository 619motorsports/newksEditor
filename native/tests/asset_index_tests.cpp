#include "apex/assets/asset_index.hpp"
#include "apex/core/parse_error.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace apex::assets;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

AssetFileInput file(std::string path, std::uint64_t size = 0, std::uint64_t modified = 0) {
    AssetFileInput result;
    result.webkit_relative_path = std::move(path);
    result.name = result.webkit_relative_path;
    result.size = size;
    result.last_modified = modified;
    return result;
}

void normalizes_and_resolves_safely() {
    require(normalize_asset_path(".\\extension\\textures\\..\\paint.dds") == "extension/paint.dds", "cross-platform normalization");
    require(normalize_asset_path("/extension//textures/body.dds") == "extension/textures/body.dds", "leading slash normalization");
    require(normalize_asset_path("'?textures/common/water_normal.dds'") == "textures/common/water_normal.dds", "quoted URL normalization");
    require(normalize_asset_path("../../outside.dds").empty(), "root traversal rejected");
    require(normalize_asset_path("C:\\outside.dds").empty(), "drive path rejected");
    const std::vector<AssetFileInput> files = {file("car/extension/textures/body.dds"), file("car/skins/red/glass.png")};
    const auto index = create_asset_file_index(files);
    require(resolve_asset_file(index, "extension\\textures\\BODY.dds").matched_by == AssetMatchKind::exact, "exact resolution");
    require(resolve_asset_file(index, "textures/body.dds").matched_by == AssetMatchKind::suffix, "suffix resolution");
    require(resolve_asset_file(index, "glass.png").status == AssetResolutionStatus::resolved, "basename resolution");
}

void reports_ambiguity_and_folder_identity() {
    const std::vector<AssetFileInput> files = {
        file("car/extension/a/shared.dds", 1, 2), file("car/extension/b/shared.dds", 1, 2)};
    const auto index = create_asset_file_index(files);
    const auto ambiguous = resolve_asset_file(index, "shared.dds");
    require(ambiguous.status == AssetResolutionStatus::ambiguous && ambiguous.matches.size() == 2, "basename ambiguity");
    const std::vector<AssetModelDescriptor> models = {{files[0], false}};
    require(asset_folder_matches_model_files(index, models), "model identity match");
    const AssetFileInput changed = file("car/extension/a/shared.dds", 1, 9);
    require(!asset_folder_matches_model_files(index, std::vector<AssetModelDescriptor>{{changed, false}}), "changed identity mismatch");
}

void discovers_skins_resources_and_animations() {
    const std::vector<AssetFileInput> files = {
        file("car/skins/02_red/body.dds"), file("car/skins/02_red/Plate_D.dds"), file("car/skins/02_red/ui_skin.json"),
        file("car/skins/10_blue/nested/ignored.dds"), file("car/skins/10_blue/ui_skin.json"), file("car/animations/shift.KSANIM")};
    const auto index = create_asset_file_index(files);
    const auto skins = discover_asset_skins(index);
    require(skins.size() == 2 && skins[0].name == "02_red" && skins[0].files.size() == 2 && skins[0].metadata_files.size() == 1 &&
                skins[1].name == "10_blue" && skins[1].files.empty() && skins[1].metadata_files.size() == 1, "immediate skin discovery");
    const auto animations = discover_asset_animations(index);
    require(animations.size() == 1 && animations[0].basename == "shift.KSANIM", "animation discovery");
    const std::vector<ExternalResourceReference> resources = {{"textures/body.dds"}, {"textures\\BODY.dds"}, {"embedded.dds"}};
    require(external_resource_paths(resources) == std::vector<std::string>{"embedded.dds", "textures/body.dds"}, "unique external resources");
    const auto matches = match_skin_textures(skins[0].files, std::vector<std::string>{"textures/BODY.DDS", "missing.dds"});
    require(matches.files.size() == 1 && matches.files[0].name == "body.dds" && matches.missing == std::vector<std::string>{"missing.dds"}, "texture matching");

    const auto collision_index = create_asset_file_index(std::vector<AssetFileInput>{
        file("car/skins/collision/body.dds"), file("car/skins/collision/BODY.DDS"),
        file("car/skins/collision/ui_skin.json"), file("car/skins/collision/UI_SKIN.JSON")});
    const auto collision_skins = discover_asset_skins(collision_index);
    const auto collision = match_skin_textures(collision_skins[0].files, std::vector<std::string>{"body.dds"});
    require(collision.ambiguous.size() == 1 && collision.ambiguous[0].entries.size() == 2 &&
                collision_skins[0].metadata_files.size() == 2, "case collisions remain ambiguous");
}

void enforces_untrusted_input_limits() {
    const std::vector<AssetFileInput> files = {file("a.dds"), file("b.dds")};
    AssetIndexLimits limits;
    limits.maxFiles = 1;
    bool caught = false;
    try { (void)create_asset_file_index(files, limits); }
    catch (const apex::core::ParseError& error) { caught = error.code() == "FILE_LIMIT"; }
    require(caught, "file count limit");
    AssetIndexLimits path_limits;
    path_limits.maxPathBytes = 2;
    const auto index = create_asset_file_index(files, path_limits);
    require(index.entries.empty(), "path limit drops unsafe entries");

    AssetFileIndex oversized_lookup;
    oversized_lookup.entries.push_back({file("car/a.dds"), "car/a.dds", "a.dds", "a.dds"});
    oversized_lookup.exact.push_back({"a.dds", {0, 0, 0}});
    AssetIndexLimits match_limits;
    match_limits.maxMatches = 2;
    caught = false;
    try { (void)resolve_asset_file(oversized_lookup, "a.dds", match_limits); }
    catch (const apex::core::ParseError& error) { caught = error.code() == "MATCH_LIMIT"; }
    require(caught, "resolution caps matches before copying");
    oversized_lookup.exact[0].second = {9};
    caught = false;
    try { (void)resolve_asset_file(oversized_lookup, "a.dds"); }
    catch (const apex::core::ParseError& error) { caught = error.code() == "INVALID_INDEX"; }
    require(caught, "resolution validates entry indices");

    AssetIndexLimits texture_input_limits;
    texture_input_limits.maxTextureInputs = 1;
    caught = false;
    try { (void)match_skin_textures(std::span<const AssetEntry>{}, std::vector<std::string>{"a.dds", "b.dds"}, texture_input_limits); }
    catch (const apex::core::ParseError& error) { caught = error.code() == "INPUT_LIMIT"; }
    require(caught, "texture input aggregate limit");
    AssetIndexLimits texture_output_limits;
    texture_output_limits.maxTextureOutputs = 1;
    caught = false;
    try { (void)match_skin_textures(std::span<const AssetEntry>{}, std::vector<std::string>{"a.dds", "b.dds"}, texture_output_limits); }
    catch (const apex::core::ParseError& error) { caught = error.code() == "OUTPUT_LIMIT"; }
    require(caught, "texture output aggregate limit");

    AssetIndexLimits descriptor_limits;
    descriptor_limits.maxFiles = 1;
    caught = false;
    try {
        (void)asset_folder_matches_model_files(index,
            std::vector<AssetModelDescriptor>{{files[0], false}, {files[1], false}}, descriptor_limits);
    } catch (const apex::core::ParseError& error) { caught = error.code() == "FILE_LIMIT"; }
    require(caught, "model descriptor count limit");
}

}  // namespace

int main() {
    try {
        normalizes_and_resolves_safely();
        reports_ambiguity_and_folder_identity();
        discovers_skins_resources_and_animations();
        enforces_untrusted_input_limits();
        std::cout << "Asset index tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Asset index tests failed: " << error.what() << '\n';
        return 1;
    }
}
