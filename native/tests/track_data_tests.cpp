#include "apex/core/parse_error.hpp"
#include "apex/domain/track_data.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using apex::core::ParseError;
using apex::domain::TrackSurface;
using apex::domain::TrackDataLimits;
using apex::domain::TrackSurfaces;

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

TrackSurface serializable_surface(std::size_t index, std::string key) {
    TrackSurface surface;
    surface.index = index;
    surface.key = std::move(key);
    surface.friction = 0.1234567;
    surface.damping = -0.0000004;
    surface.dirt_additive = 0.25;
    surface.black_flag_time = 12.5;
    surface.is_valid_track = true;
    surface.is_pitlane = false;
    surface.sin_height = -1.25;
    surface.sin_length = 0.0000005;
    surface.vibration_gain = 0.6;
    surface.vibration_length = 1.5;
    surface.wav = " kerb.wav ";
    surface.wav_pitch = 1.3;
    surface.ff_effect = " road ";
    return surface;
}

void serializes_surfaces_with_canonical_fields() {
    TrackSurfaces config;
    config.source = "serialize-surfaces.ini";
    config.surfaces.push_back(serializable_surface(4U, " ROAD "));
    const auto output = apex::domain::serialize_track_surfaces_ini(config);
    require(output ==
                "[SURFACE_4]\nKEY=ROAD\nFRICTION=0.123457\nDAMPING=0\n"
                "DIRT_ADDITIVE=0.25\nBLACK_FLAG_TIME=12.5\nIS_VALID_TRACK=1\n"
                "IS_PITLANE=0\nSIN_HEIGHT=-1.25\nSIN_LENGTH=0\nVIBRATION_GAIN=0.6\n"
                "VIBRATION_LENGTH=1.5\nWAV=kerb.wav\nWAV_PITCH=1.3\nFF_EFFECT=road\n",
            "surface serializer follows canonical JavaScript field order and numbers");

    config.surfaces[0].damping = -0.0078125;
    config.surfaces[0].friction = 0.0234375;
    config.surfaces[0].black_flag_time = -0.0234375;
    config.surfaces[0].sin_length = 0.0078125;
    const auto exact_halfway =
        apex::domain::serialize_track_surfaces_ini(config);
    require(exact_halfway.find("FRICTION=0.023438\n") !=
                    std::string::npos &&
                exact_halfway.find("DAMPING=-0.007813\n") !=
                    std::string::npos &&
                exact_halfway.find("BLACK_FLAG_TIME=-0.023438\n") !=
                    std::string::npos &&
                exact_halfway.find("SIN_LENGTH=0.007813\n") !=
                    std::string::npos,
            "surface serializer follows JavaScript exact-halfway rounding");
    const auto reparsed = apex::domain::parse_track_surfaces(output, config.source);
    require(reparsed.surfaces.size() == 1U && reparsed.surfaces[0].index == 4U &&
                reparsed.surfaces[0].key == "ROAD" && reparsed.surfaces[0].is_valid_track &&
                !reparsed.surfaces[0].is_pitlane && reparsed.surfaces[0].wav_pitch == 1.3,
            "serialized surface round-trips through the native parser");
}

void preserves_sparse_indices_and_unknown_fields() {
    const auto parsed = apex::domain::parse_track_surfaces(
        "[SURFACE_4]\nKEY=ROAD\nCUSTOM_ALPHA=one\n"
        "[SURFACE_9]\nKEY=KERB\nCUSTOM_BETA=two\n",
        "sparse-surfaces.ini");
    const auto output = apex::domain::serialize_track_surfaces_ini(parsed);
    require(output.find("[SURFACE_4]\n") != std::string::npos &&
                output.find("[SURFACE_9]\n") != std::string::npos &&
                output.find("CUSTOM_ALPHA=one\n") != std::string::npos &&
                output.find("CUSTOM_BETA=two\n") != std::string::npos,
            "surface serializer preserves sparse indices and ordered unknown fields");
    const auto reparsed = apex::domain::parse_track_surfaces(output, "sparse-roundtrip.ini");
    require(reparsed.surfaces.size() == 2U && reparsed.surfaces[0].index == 4U &&
                reparsed.surfaces[1].index == 9U && reparsed.surfaces[0].fields.size() == 15U &&
                reparsed.surfaces[0].fields.back().key == "CUSTOM_ALPHA" &&
                reparsed.surfaces[1].fields.back().key == "CUSTOM_BETA",
            "unknown surface fields remain in source order after round-trip");
}

void rejects_invalid_surface_serialization_input() {
    TrackSurfaces empty;
    empty.source = "invalid-surfaces.ini";
    expects_error([&] { (void)apex::domain::serialize_track_surfaces_ini(empty); },
                  "EMPTY_SURFACE_MANIFEST");

    TrackSurfaces duplicate;
    duplicate.source = "duplicate-surfaces.ini";
    duplicate.surfaces.push_back(serializable_surface(2U, "ROAD"));
    duplicate.surfaces.push_back(serializable_surface(2U, "KERB"));
    expects_error([&] { (void)apex::domain::serialize_track_surfaces_ini(duplicate); },
                  "DUPLICATE_SURFACE_INDEX");

    TrackSurfaces invalid;
    invalid.source = "unsafe-surfaces.ini";
    invalid.surfaces.push_back(serializable_surface(0U, "  "));
    expects_error([&] { (void)apex::domain::serialize_track_surfaces_ini(invalid); },
                  "EMPTY_SURFACE_FIELD");
    invalid.surfaces[0] = serializable_surface(0U, "ROAD");
    invalid.surfaces[0].wav = "bad\nname.wav";
    expects_error([&] { (void)apex::domain::serialize_track_surfaces_ini(invalid); },
                  "UNSAFE_TEXT");
    invalid.surfaces[0].wav = "safe.wav";
    invalid.surfaces[0].friction = std::numeric_limits<double>::quiet_NaN();
    expects_error([&] { (void)apex::domain::serialize_track_surfaces_ini(invalid); },
                  "NON_FINITE_VALUE");
    invalid.surfaces[0] = serializable_surface(0U, "ROAD");
    invalid.surfaces[0].fields.push_back({"CUSTOM=BAD", "value", 0U, 0U});
    expects_error([&] { (void)apex::domain::serialize_track_surfaces_ini(invalid); },
                  "UNSAFE_TEXT");
}

void enforces_surface_serializer_limits() {
    TrackSurfaces config;
    config.source = "limited-surfaces.ini";
    config.surfaces.push_back(serializable_surface(0U, "ROAD"));
    TrackDataLimits output_limits;
    output_limits.maxOutputBytes = 16U;
    expects_error([&] { (void)apex::domain::serialize_track_surfaces_ini(config, output_limits); },
                  "OUTPUT_LIMIT");

    TrackDataLimits section_limits;
    section_limits.maxSections = 0U;
    expects_error([&] { (void)apex::domain::serialize_track_surfaces_ini(config, section_limits); },
                  "SECTION_LIMIT");

    TrackDataLimits field_limits;
    field_limits.maxFields = 13U;
    expects_error([&] { (void)apex::domain::serialize_track_surfaces_ini(config, field_limits); },
                  "FIELD_LIMIT");
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

    const std::vector<std::string> no_names;
    const std::vector<apex::domain::TrackMeshName> no_meshes;
    const auto empty = apex::domain::audit_track_markers(no_names, no_meshes);
    bool missing_start = false, missing_pit = false;
    for (const auto& diagnostic : empty.diagnostics) {
        if (diagnostic.code != "MISSING_MARKER") continue;
        missing_start = missing_start ||
            (diagnostic.severity == apex::domain::TrackDiagnosticSeverity::error &&
             diagnostic.message == "AC_START_0 is missing");
        missing_pit = missing_pit ||
            (diagnostic.severity == apex::domain::TrackDiagnosticSeverity::warning &&
             diagnostic.message == "AC_PIT_0 is missing");
    }
    require(missing_start && missing_pit,
            "empty marker sets retain JavaScript missing-marker severities");

    const std::vector<std::string> mixed_case = {
        "ac_start_0", "ac_pit_0", "ac_time_0_l", "AC_TIME_0_R",
        "ac_hotlap_start_0"};
    const auto case_audit = apex::domain::audit_track_markers(mixed_case, no_meshes);
    require(case_audit.starts == 1 && case_audit.pits == 1 &&
                case_audit.time_gates == 1 && !case_audit.hotlap,
            "numbered and time markers are case-insensitive but hotlap is exact");

    const auto plus = apex::domain::resolve_runtime_surface("+1ROAD");
    require(plus.sector_id == 1 &&
                plus.status == apex::domain::RuntimeSurfaceStatus::matched,
            "positive signed physics sector");
    const auto whitespace = apex::domain::resolve_runtime_surface(
        std::string("\xc2\xa0") + "+2ROAD");
    require(whitespace.sector_id == 2 &&
                whitespace.status == apex::domain::RuntimeSurfaceStatus::matched,
            "JavaScript whitespace before physics sector");
    const auto unsafe = apex::domain::resolve_runtime_surface("9007199254740992ROAD");
    require(unsafe.sector_id == 0 &&
                unsafe.status == apex::domain::RuntimeSurfaceStatus::not_physics,
            "unsafe JavaScript integer is not a physics sector");
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
        serializes_surfaces_with_canonical_fields();
        preserves_sparse_indices_and_unknown_fields();
        rejects_invalid_surface_serialization_input();
        enforces_surface_serializer_limits();
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
