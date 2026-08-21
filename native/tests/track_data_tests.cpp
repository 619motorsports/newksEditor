#include "apex/core/parse_error.hpp"
#include "apex/domain/track_data.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using apex::core::ParseError;
using apex::domain::TrackDataLimits;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function>
void expects_error(Function&& function, std::string_view code) {
    try {
        function();
    } catch (const ParseError& error) {
        require(error.code() == code, "unexpected track-data error code");
        return;
    }
    throw std::runtime_error("malformed or over-limit track data was accepted");
}

void parses_sepang_surface_fields_in_order() {
    // Values and field order are from test/content/tracks/sepang/data/surfaces.ini.
    const auto surfaces = apex::domain::parse_track_surfaces(
        "[SURFACE_0]\nKEY=TARMAC\nFRICTION=0.99\nDAMPING=0\nWAV=\nWAV_PITCH=0\nFF_EFFECT=NULL\nDIRT_ADDITIVE=0\nIS_VALID_TRACK=1\nBLACK_FLAG_TIME=0\nSIN_HEIGHT=0\nSIN_LENGTH=0\nIS_PITLANE=0\nVIBRATION_GAIN=0\nVIBRATION_LENGTH=0\n\n"
        "[SURFACE_1]\nKEY=KERB\nFRICTION=0.95\nDAMPING=0\nWAV=kerb.wav\nWAV_PITCH=1.3\nFF_EFFECT=1\nDIRT_ADDITIVE=0\nIS_VALID_TRACK=1\nBLACK_FLAG_TIME=0\nSIN_HEIGHT=0\nSIN_LENGTH=0\nIS_PITLANE=0\nVIBRATION_GAIN=0.6\nVIBRATION_LENGTH=1.5\n",
        "sepang/data/surfaces.ini");
    require(surfaces.surfaces.size() == 2, "Sepang surface count");
    require(surfaces.surfaces[0].key == "TARMAC" && surfaces.surfaces[0].friction == 0.99,
            "Sepang TARMAC projection");
    require(surfaces.surfaces[1].key == "KERB" && surfaces.surfaces[1].wav == "kerb.wav" &&
                surfaces.surfaces[1].vibration_length == 1.5,
            "Sepang KERB projection");
    require(surfaces.surfaces[1].fields.front().key == "KEY" &&
                surfaces.surfaces[1].fields.back().key == "VIBRATION_LENGTH",
            "ordered surface fields");
    const auto matched = apex::domain::resolve_runtime_surface("21KERB019", &surfaces);
    require(matched.status == apex::domain::RuntimeSurfaceStatus::matched &&
                matched.surface->key == "KERB" && matched.sector_id == 21,
            "runtime substring match");
}

void diagnoses_surface_duplicates_and_runtime_ambiguity() {
    const auto surfaces = apex::domain::parse_track_surfaces(
        "[SURFACE_0]\nKEY=ROAD\nFRICTION=0.98\n[SURFACE_0]\nKEY=ROAD_MAIN\nDAMPING=nope\nUNKNOWN=x\n[SURFACE_2]\nKEY=ROAD\n[SURFACE_3]\nKEY=KERB\n",
        "malformed-surfaces.ini");
    require(surfaces.surfaces.size() == 4, "duplicate surface sections retained");
    bool duplicate_index = false, duplicate_key = false, finite = false, unknown = false;
    for (const auto& diagnostic : surfaces.diagnostics) {
        duplicate_index = duplicate_index || diagnostic.code == "DUPLICATE_SURFACE_INDEX";
        duplicate_key = duplicate_key || diagnostic.code == "DUPLICATE_SURFACE_KEY";
        finite = finite || diagnostic.code == "NON_FINITE_VALUE";
        unknown = unknown || diagnostic.code == "UNKNOWN_SURFACE_FIELD";
    }
    require(duplicate_index && duplicate_key && finite && unknown, "surface diagnostics");
    const auto ambiguous = apex::domain::resolve_runtime_surface("1ROAD_MAIN", &surfaces);
    require(ambiguous.status == apex::domain::RuntimeSurfaceStatus::ambiguous &&
                ambiguous.candidates.size() == 2,
            "ambiguous runtime substring match");
}

void audits_markers_and_surface_names() {
    const std::vector<std::string> names = {"AC_START_0", "AC_START_2", "AC_PIT_0",
                                            "AC_TIME_0_L", "AC_HOTLAP_START_0"};
    const std::vector<apex::domain::TrackMeshName> meshes = {
        {"1ROAD_main", true}, {"1GRASS", true}, {"2MUD", true}, {"0ROAD_visual", true}};
    const auto surfaces = apex::domain::parse_track_surfaces("[SURFACE_0]\nKEY=ROAD\n[SURFACE_1]\nKEY=KERB\n");
    const auto audit = apex::domain::audit_track_markers(names, meshes, &surfaces);
    require(audit.starts == 2 && audit.pits == 1 && audit.time_gates == 1 && audit.hotlap,
            "marker counts");
    require(audit.unmatched_physical == std::vector<std::string>{"2MUD"}, "unmatched physical");
    require(audit.has_errors(), "marker audit missing time endpoint error");
    bool gap = false, missing_endpoint = false;
    for (const auto& diagnostic : audit.diagnostics) {
        gap = gap || diagnostic.code == "MARKER_GAP";
        missing_endpoint = missing_endpoint || diagnostic.code == "INCOMPLETE_TIME_GATE";
    }
    require(gap && missing_endpoint, "marker diagnostics");
}

void parses_sepang_camera_basis_and_metadata() {
    // Header and CAMERA_0 values are from test/content/tracks/sepang/data/cameras.ini.
    const auto cameras = apex::domain::parse_track_cameras(
        "[HEADER]\nVERSION=3\nCAMERA_COUNT=2\nSET_NAME=TV 1\n"
        "[CAMERA_0]\nNAME=cam01\nPOSITION=59.3541,5.13246,29.4725\nFORWARD=0.863623,0.0299959,-0.503246\nUP=-0.0259166,0.999548,0.015102\nMIN_FOV=4\nMAX_FOV=28\nIN_POINT=0.953054\nOUT_POINT=0.9999\nNEAR_PLANE=0.1\nFAR_PLANE=5000\nSPLINE=\nIS_FIXED=0\n"
        "[CAMERA_1]\nNAME=cam02\nPOSITION=-244.198,12.4337,63.6706\nFORWARD=0.940596,-0.079084,-0.330203\nUP=0.0746197,0.996868,-0.0261958\nMIN_FOV=24\nMAX_FOV=35\nIN_POINT=0.0164027\nOUT_POINT=0.0800339\nNEAR_PLANE=0.1\nFAR_PLANE=5000\n",
        "sepang/data/cameras.ini");
    require(cameras.version == 3 && cameras.name == "TV 1" && cameras.cameras.size() == 2,
            "Sepang camera header");
    require(cameras.cameras[0].position[0] == 59.3541 && cameras.cameras[0].max_fov == 28,
            "Sepang camera optics");
    require(cameras.diagnostics.empty(), "valid Sepang camera diagnostics");
}

void diagnoses_camera_gaps_basis_and_safe_spline_requests() {
    const auto cameras = apex::domain::parse_track_cameras(
        "[HEADER]\nCAMERA_COUNT=2\n[CAMERA_0]\nFORWARD=0,2,0\nUP=0,1,0\nMIN_FOV=20\nMAX_FOV=10\nIN_POINT=.8\nOUT_POINT=.2\nNEAR_PLANE=5\nFAR_PLANE=1\nSPLINE=..\\bad.csv\n[CAMERA_2]\n",
        "bad-cameras.ini");
    bool gap = false, basis = false, fov = false, clip = false, path = false;
    for (const auto& diagnostic : cameras.diagnostics) {
        gap = gap || diagnostic.code == "NON_CONTIGUOUS_CAMERA";
        basis = basis || diagnostic.code == "NON_NORMALIZED_BASIS";
        fov = fov || diagnostic.code == "INVALID_FOV";
        clip = clip || diagnostic.code == "INVALID_CLIP_PLANES";
        path = path || diagnostic.code == "UNSAFE_SPLINE_PATH";
    }
    require(gap && basis && fov && clip && path, "camera diagnostics");
    const auto request = apex::domain::request_camera_spline(cameras.cameras[0], "bad-cameras.ini");
    require(!request.accepted && request.code == "UNSAFE_SPLINE_PATH", "safe spline request");
    apex::domain::CameraData valid;
    valid.spline = "splines/follow.csv";
    const auto valid_request = apex::domain::request_camera_spline(valid, "camera.ini");
    require(valid_request.accepted && valid_request.relative_path == "splines/follow.csv",
            "relative spline request without filesystem access");
}

void rotates_samples_and_bounds_spline_rows() {
    const auto spline = apex::domain::parse_camera_spline("0, 0, 0\ninvalid\n0 1 0\n", "space.csv");
    require(spline.points.size() == 1, "delimiter mode follows first nonempty row");
    require(!spline.diagnostics.empty(), "malformed spline row diagnostic");
    const auto valid = apex::domain::parse_camera_spline("0,0,0\n1,0,0\n2,0,0\n", "move.csv");
    require(valid.points.size() == 3 && valid.length == 2, "spline points and length");
    const auto rotated = apex::domain::rotate_camera_spline(valid.points, 90);
    require(std::abs(rotated[1][0]) < 1e-12 && std::abs(rotated[1][2] + 1) < 1e-12,
            "spline rotation");
    const auto end = apex::domain::sample_camera_spline(rotated, 1);
    require(std::abs(end[2] + 2.49700004) < 0.00001, "game spline endpoint rule");
    TrackDataLimits line_limits;
    line_limits.maxLineBytes = 3;
    expects_error([&] { (void)apex::domain::parse_camera_spline("0,0,0\n", "line.csv", line_limits); }, "LINE_LIMIT");
    TrackDataLimits point_limits;
    point_limits.maxSplinePoints = 1;
    expects_error([&] { (void)apex::domain::parse_camera_spline("0,0,0\n1,0,0\n", "points.csv", point_limits); }, "SPLINE_LIMIT");
    const std::string invalid("0,0,\xff", 6);
    expects_error([&] { (void)apex::domain::parse_camera_spline(invalid, "utf8.csv"); }, "INVALID_UTF8");
}

void appliesTrackLimitsBeforeIniProjection() {
    TrackDataLimits line_limits;
    line_limits.maxLineBytes = 3;
    expects_error([&] {
        (void)apex::domain::parse_track_surfaces("[SURFACE_0]\n", "surface.ini", line_limits);
    }, "LINE_LIMIT");

    TrackDataLimits input_limits;
    input_limits.maxInputBytes = 4;
    expects_error([&] {
        (void)apex::domain::parse_track_cameras("[HEADER]\n", "camera.ini", input_limits);
    }, "INPUT_LIMIT");
}

}  // namespace

int main() {
    try {
        parses_sepang_surface_fields_in_order();
        diagnoses_surface_duplicates_and_runtime_ambiguity();
        audits_markers_and_surface_names();
        parses_sepang_camera_basis_and_metadata();
        diagnoses_camera_gaps_basis_and_safe_spline_requests();
        rotates_samples_and_bounds_spline_rows();
        appliesTrackLimitsBeforeIniProjection();
        std::cout << "track data tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "track data tests failed: " << error.what() << '\n';
        return 1;
    }
}
