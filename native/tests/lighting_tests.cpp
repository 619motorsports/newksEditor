#include "apex/core/parse_error.hpp"
#include "apex/render/directional_shadow.hpp"
#include "apex/render/lighting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace apex::render;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <std::size_t N>
bool finite(std::array<float, N> value) {
    return std::all_of(value.begin(), value.end(), [](float component) {
        return std::isfinite(component);
    });
}

template <typename Function>
void expectParseError(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const apex::core::ParseError&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

constexpr std::string_view curves = R"ini(
[HEADER]
VERSION=3
ANGLE_GAMMA=2.0
HDR_OFF_MULT=1.25
[HORIZON]
LOW=10,20,30,255
HIGH=40,50,60,128
[SKY]
LOW=70,80,90,255
HIGH=100,110,120,255
[SUN]
LOW=130,140,150,255
HIGH=160,170,180,255
[AMBIENT]
LOW=20,30,40,128
HIGH=50,60,70,255
)ini";

constexpr std::string_view weather = R"ini(
[LAUNCHER]
NAME=Fixture Weather
[FOG]
COLOR=1.0,2.0,3.0
BLEND=0.75
DISTANCE=1200
[CLOUDS]
COVER=0.5
CUTOFF=0.7
COLOR=0.3
WIDTH=16
HEIGHT=7
RADIUS=20
NUMBER=42
BASE_SPEED_MULT=0.2
)ini";

void weatherAndExposure() {
    const auto& presets = stockWeatherPresets();
    require(presets.size() == 7u && presets[0].id == "1_heavy_fog" &&
                defaultWeatherPreset().id == "5_light_clouds", "stock weather presets");
    const auto parsed = parseKsWeatherLighting(curves, weather, "fixture-weather");
    require(parsed.name == "Fixture Weather" && parsed.version == 3u &&
                parsed.cloud_number == 42u && parsed.fog_color[1] == 2.0F,
            "weather INI conversion");
    const auto evaluated = evaluateKsLighting(parsed, {0.0F, 2.0F, 0.0F});
    require(std::abs(evaluated.sun_height - 1.0F) < 1e-6F &&
                std::abs(evaluated.angle_mix) < 1e-6F && finite(evaluated.sky_color),
            "normalized sun and weather interpolation");
    const auto direction = sunDirectionFromAngles(90.0F, 0.0F);
    require(std::abs(direction[0] - 1.0F) < 1e-5F && std::abs(direction[1]) < 1e-5F,
            "sun angle conversion");
    require(evaluateKsLighting(parsed, {0, 0, 0}).sun_height == 0.0F,
            "zero sun direction follows JS normalization");

    require(std::abs(ksEditorAutoExposure(1.0F) - 0.32F) < 1e-6F &&
                ksEditorAutoExposure(0.0F) == ks_editor_exposure_max &&
                ksEditorAutoExposure(std::numeric_limits<float>::quiet_NaN()) ==
                    ks_editor_exposure_max,
            "bounded auto exposure");
    const auto mapped = ksEditorYebisToneMap({1.0F, 2.0F, 4.0F});
    require(finite(mapped) && mapped[0] >= 0.0F && mapped[0] <= 1.0F,
            "finite display curve");
    const auto glare = ksEditorGlareBrightPass({4.0F, 6.0F, 10.0F});
    require(glare[0] == 0.0F && glare[1] == 1.0F && glare[2] == 5.0F,
            "bright pass threshold");
    require(std::abs(ksEditorBloomCompositeScale() - 0.01064F) < 1e-6F,
            "bloom composite metadata");
    const auto kernel = ksEditorBloomGaussianKernel();
    require(kernel.sample_count >= 1u && kernel.sample_count <= 15u &&
                std::abs(kernel.weights[0]) > 0.0F && finite(kernel.channel_sigmas),
            "bounded bloom kernel");

    expectParseError([] { static_cast<void>(parseKsWeatherLighting("[HEADER]\nVERSION=2", weather, "bad")); },
                     "unsupported weather version must fail");
    expectParseError([] { static_cast<void>(parseKsWeatherLighting(curves, "[FOG]\nCOLOR=1,2", "bad")); },
                     "truncated weather vector must fail");
    expectParseError([] { static_cast<void>(parseKsWeatherLighting(
                            "[HEADER]\nVERSION=3\n[HORIZON]\nLOW=1,2,3x,255", weather, "bad")); },
                     "malformed numeric curve must fail");
    apex::core::ParseLimits limits;
    limits.maxInputBytes = 4u;
    expectParseError([&] { static_cast<void>(parseKsWeatherLighting(curves, weather, "bad", limits)); },
                     "weather input limit must fail");
    limits = {};
    limits.maxStringBytes = 8u;
    expectParseError([&] { static_cast<void>(parseKsWeatherLighting(curves, "[LAUNCHER]\nNAME=too-long-weather", "bad", limits)); },
                     "weather string limit must fail");
    limits = {};
    limits.maxTracks = 2u;
    expectParseError([&] { static_cast<void>(parseKsWeatherLighting(curves, weather, "bad", limits)); },
                     "weather line limit must fail");
}

void cspAndShadows() {
    const auto line = cspLineLightSample({0, 0, 0}, {10, 0, 0}, {12, 2, 0},
                                          {0, 0, 0}, {1, 2, 3});
    require(line.value > 1.0F && line.clamped_value == 1.0F && line.point[0] == 10.0F &&
                line.color[1] == 2.0F, "line light clamp");
    require(cspLineLightSample({std::numeric_limits<float>::infinity(), 0, 0}, {1, 0, 0}, {0, 0, 0},
                                {1, 2, 3}, {4, 5, 6}).distance_inverse == 0.0F,
            "non-finite line input is rejected");
    require(!cspLightReceiverVisible("interior", false) &&
                cspLightReceiverVisible("exterior", false), "receiver visibility");
    require(cspLightDistanceFade(200.0F) > 0.4F && cspLightDistanceFade(300.0F) == 0.0F,
            "distance fade");
    const auto cone = cspSpotConePacking({0, 0, -1}, 90.0F);
    require(cone.enabled && cspSpotConeFactor({0, 0, -1}, {0, 0, -1}, 90.0F) >= 0.0F,
            "spot cone packing");
    require(cspSpotEdgeFactors({0, 1, 0}, {1, 1, 1}, 0.5F, {0, -1, 0})[0] >= 0.0F,
            "spot edge factors");
    require(!cspSpotConePacking({std::numeric_limits<float>::infinity(), 0, 0}, 90.0F).enabled &&
                !cspSpotEdgePacking({0, 1, 0}, {std::numeric_limits<float>::infinity(), 0, 0}, 1.0F).enabled &&
                cspSpotConeFactor({0, -1, 0}, {std::numeric_limits<float>::infinity(), 0, 0}, 90.0F) == 0.0F &&
                cspSpotEdgeFactors({0, 1, 0}, {1, 1, 1}, 1.0F,
                                    {std::numeric_limits<float>::infinity(), 0, 0})[0] == 0.0F,
            "non-finite spot inputs are rejected");
    require(cspSecondarySpotPacking(10.0F, 0.5F).enabled &&
                cspSecondarySpotAttenuation(5.0F, 10.0F, 0.5F) >= 0.0F,
            "secondary spot attenuation");

    require(cspLocalShadowFilter("LIGHT_HEADLIGHT", false).mode == 2u &&
                cspLocalShadowFilter("light", true).kernel == 15u,
            "local shadow filter variants");
    require(cspLocalShadowFilter("light", false, "<built-in:SelfLight_Headlights>").mode == 2u &&
                cspLocalShadowFilter("LIGHT_HEADLIGHTXYZ", false).mode == 0u,
            "case-insensitive and bounded headlight detection");
    ExponentialShadowInput esm_input;
    esm_input.range = 80.0F;
    esm_input.shadow_spot = 90.0F;
    const auto esm = cspExponentialShadowParams(esm_input);
    require(esm.max_range == 30.0F && esm.exp_factor == 20.0F && esm.boost > 0.0F &&
                esm.clip_plane == 0.5F && esm.clip_sphere == 0.5F,
            "bounded ESM constants");
    ExponentialShadowInput explicit_zero;
    explicit_zero.shadow_clip_sphere_authored = true;
    explicit_zero.shadow_clip_sphere = 0.0F;
    require(cspExponentialShadowParams(explicit_zero).clip_sphere == 0.0F,
            "authored zero clip sphere is preserved");
    ExponentialShadowInput zero_defaults;
    zero_defaults.range = 0.0F;
    zero_defaults.exp_factor = 0.0F;
    require(cspExponentialShadowParams(zero_defaults).max_range == 10.0F &&
                cspExponentialShadowParams(zero_defaults).exp_factor == 20.0F,
            "zero ESM fields use JS missing-value defaults");
    require(resolveCspExponentialShadow(0.5F, 5.0F, 0.2F, esm) >= 0.0F,
            "ESM resolve");

    DirectionalShadowInput directional;
    directional.eye = {0, 1, 5};
    directional.target = {0, 0, 0};
    directional.map_size = 1u << 30u;
    const auto cascades = computeDirectionalShadowCascades(directional);
    require(cascades.cascades.size() == 3u && cascades.map_size == 16384u &&
                cascades.splits[0] < cascades.splits[1] && finite(cascades.cascades[2].matrix),
            "bounded directional cascades");
    ProbeShadowInput probe;
    probe.map_size = 0;
    const auto probe_result = computeDirectionalProbeShadowCascades(probe);
    require(probe_result.cascades.size() == 3u && probe_result.map_size == 1u &&
                finite(probe_result.cascades[0].matrix), "probe cascade matrices");
    LocalShadowInput local;
    local.esm.shadow_range_authored = true;
    local.esm.shadow_range = 4.0F;
    local.spot = 150.0F;
    local.range = 20.0F;
    local.direction = {0, 0, 0};
    const auto local_result = computeLocalLightShadow(local);
    require(local_result.far_plane == 4.0F && finite(local_result.matrix) &&
                local_result.direction[1] < 0.0F, "local shadow fallback direction");
    require(local_result.esm.boost < 10.0F,
            "local shadow derives automatic boost from effective spot");
    require(shadowCasterEnabled(false, true) && !shadowCasterEnabled(true, false),
            "shadow caster override");
}

void convertsDirectionalCascadeClipSpace() {
    const LightingMat4 identity = {1.0F, 0.0F, 0.0F, 0.0F,
                                   0.0F, 1.0F, 0.0F, 0.0F,
                                   0.0F, 0.0F, 1.0F, 0.0F,
                                   0.0F, 0.0F, 0.0F, 1.0F};
    const auto d3d = convertDirectionalShadowCascadeMatrix(identity,
                                                             CameraClipSpace::d3d12);
    require(d3d.ok() && d3d.matrix[5] == 1.0F && d3d.matrix[10] == 0.5F &&
                d3d.matrix[14] == 0.5F && d3d.matrix[15] == 1.0F,
            "D3D12 cascade conversion maps depth to zero-one");
    const auto vulkan = convertDirectionalShadowCascadeMatrix(identity,
                                                                CameraClipSpace::vulkan);
    require(vulkan.ok() && vulkan.matrix[5] == -1.0F && vulkan.matrix[10] == 0.5F &&
                vulkan.matrix[14] == 0.5F,
            "Vulkan cascade conversion maps depth and flips y");

    const LightingMat4 representative = {2.0F, 3.0F, 5.0F, 7.0F,
                                          11.0F, 13.0F, 17.0F, 19.0F,
                                          23.0F, 29.0F, 31.0F, 37.0F,
                                          41.0F, 43.0F, 47.0F, 53.0F};
    const auto converted = convertDirectionalShadowCascadeMatrix(
        representative, CameraClipSpace::d3d12);
    require(converted.ok() && converted.matrix[2] == 6.0F &&
                converted.matrix[6] == 18.0F && converted.matrix[10] == 34.0F &&
                converted.matrix[14] == 50.0F && finite(converted.matrix),
            "representative cascade matrix conversion is numeric and finite");

    const auto webgl = convertDirectionalShadowCascadeMatrix(identity,
                                                               CameraClipSpace::webgl);
    require(!webgl.ok() &&
                webgl.status == DirectionalShadowClipSpaceStatus::unsupported &&
                webgl.code == "directional_shadow_clip_space_unsupported",
            "WebGL target is rejected as a native conversion target");
    const auto unknown = convertDirectionalShadowCascadeMatrix(
        identity, static_cast<CameraClipSpace>(255U));
    require(!unknown.ok() && unknown.status == DirectionalShadowClipSpaceStatus::unsupported,
            "unknown clip space is controlled");

    LightingMat4 non_finite = identity;
    non_finite[3] = std::numeric_limits<float>::quiet_NaN();
    const auto malformed = convertDirectionalShadowCascadeMatrix(
        non_finite, CameraClipSpace::vulkan);
    require(!malformed.ok() &&
                malformed.status == DirectionalShadowClipSpaceStatus::invalid_input &&
                malformed.code == "directional_shadow_matrix_non_finite",
            "non-finite cascade matrix is rejected");
}

void reflectionsAndBounds() {
    const auto faces = webglCubemapFaces();
    require(ksEditorCubemap().faces_per_frame == 1u && faces.size() == 6u &&
                faces[0].target == "negative-x" && faces[2].up[2] == 1,
            "cubemap face budget and orientation");
    const std::array<ReflectionCaptureItem, 2> items = {{{"environment", false, true, false},
                                                          {"selected", false, false, true}}};
    const auto environment = selectReflectionCaptureItems(items);
    require(environment.mode == ReflectionCaptureMode::environment &&
                environment.item_indices.size() == 1u, "environment capture selection");
    const auto explicit_selection = selectReflectionCaptureItems(items, "selected");
    require(explicit_selection.mode == ReflectionCaptureMode::explicit_subtree &&
                explicit_selection.item_indices.size() == 1u, "explicit capture selection");
    require(selectReflectionCaptureItems({}, {}, {}, 0, true).mode == ReflectionCaptureMode::disabled,
            "isolated capture disabled");
    require(selectReflectionCaptureItems({}, {}, "track", 0).mode == ReflectionCaptureMode::scene,
            "track capture selection");
    require(selectReflectionCaptureItems({}, {}, {}, 0).source_kind == LightingSource::procedural_fallback,
            "reflection fallback is explicitly labeled");
    const auto blur = reflectionBlurFromExponent(128.0F);
    require(blur.base > 0.0F && blur.mip > 0.0F && blur.mip < blur.base,
            "reflection mip heuristic");
    require(reflectionFresnel(0.5F, 0.04F, 5.0F, 1.0F) >= 0.0F,
            "reflection Fresnel");
    const auto fallback = portableReflectionEnvironment({0, -1, 0}, 2.0F,
                                                         {1, 1, 1}, {0.5F, 0.6F, 0.7F},
                                                         {0.1F, 0.2F, 0.3F}, 0.5F);
    require(fallback.source_kind == LightingSource::procedural_fallback && finite(fallback.color),
            "labeled procedural reflection fallback");
    const auto nan_fallback = portableReflectionEnvironment(
        {std::numeric_limits<float>::quiet_NaN(), 0, 0}, 9999.0F,
        {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, 1.0F);
    require(finite(nan_fallback.color), "finite fallback under untrusted input");
    const auto zero_fallback = portableReflectionEnvironment({0, 0, 0}, 0,
                                                              {3, 4, 5}, {1, 1.5F, 2},
                                                              {.5F, .6F, .7F}, 0);
    require(finite(zero_fallback.color), "zero direction remains finite");
}

void evaluatesSourceEvidencedDirectionalReceiver() {
    constexpr std::array<float, directional_shadow_cascade_count> splits = {
        2.0F, 12.0F, 50.0F};
    require(select_directional_shadow_cascade(2.0F, splits) == 0U &&
                select_directional_shadow_cascade(2.0001F, splits) == 1U &&
                select_directional_shadow_cascade(12.0F, splits) == 1U &&
                select_directional_shadow_cascade(12.0001F, splits) == 2U &&
                select_directional_shadow_cascade(50.0F, splits) == 2U &&
                !select_directional_shadow_cascade(50.0001F, splits).has_value(),
            "directional receiver uses hard 2/12/50 split boundaries");

    constexpr std::array<float, 3> inside = {0.5F, 0.5F, 0.5F};
    std::array<float, 9> samples{};
    samples.fill(0.4F);
    const auto occluded = evaluate_directional_shadow_pcf(inside, 0.0F, samples);
    require(occluded.has_value() && *occluded == 0.0F,
            "nine shallower D32 taps produce full occlusion");
    samples.fill(0.5F);
    const auto lit = evaluate_directional_shadow_pcf(inside, 0.0F, samples);
    require(lit.has_value() && *lit == 1.0F,
            "equal receiver and sampled depth is lit");
    samples.fill(0.4F);
    samples.front() = 0.6F;
    const auto one_tap = evaluate_directional_shadow_pcf(inside, 0.0F, samples);
    require(one_tap.has_value() &&
                std::abs(*one_tap - 1.0F / 9.0F) < 0.000001F,
            "explicit 3x3 PCF returns one ninth for one lit tap");
    samples.fill(0.4999F);
    const auto biased = evaluate_directional_shadow_pcf(inside, 0.0002F, samples);
    require(biased.has_value() && *biased == 1.0F,
            "per-cascade bias applies before the depth comparison");
    require(evaluate_directional_shadow_pcf({0.0F, 0.5F, 0.5F}, 0.0F, {}) ==
                1.0F,
            "out-of-map receiver coordinates are fully lit without sampling");
    require(!evaluate_directional_shadow_pcf(
                 inside, 0.0F, std::span<const float>(samples).first(8U))
                 .has_value(),
            "truncated PCF tap input is rejected");
    samples[3] = std::numeric_limits<float>::quiet_NaN();
    require(!evaluate_directional_shadow_pcf(inside, 0.0F, samples).has_value(),
            "non-finite sampled depth is rejected");
}

} // namespace

int main() {
    try {
        weatherAndExposure();
        cspAndShadows();
        convertsDirectionalCascadeClipSpace();
        evaluatesSourceEvidencedDirectionalReceiver();
        reflectionsAndBounds();
        std::cout << "lighting tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "lighting tests failed: " << error.what() << '\n';
        return 1;
    }
}
