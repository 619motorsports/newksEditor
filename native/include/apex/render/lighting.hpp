#pragma once

#include "apex/core/parse_limits.hpp"
#include "apex/render/camera.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace apex::render {

using LightingVec3 = std::array<float, 3>;
using LightingMat4 = std::array<float, 16>;

enum class LightingSource : std::uint8_t {
    source_evidenced,
    procedural_fallback,
};

struct WeatherPreset {
    std::string id;
    std::string name;
    std::string source;
    std::uint32_t version = 3;
    float angle_gamma = 1.0F;
    float hdr_off_mult = 1.0F;
    LightingVec3 horizon_low{};
    LightingVec3 horizon_high{};
    LightingVec3 sky_low{};
    LightingVec3 sky_high{};
    LightingVec3 sun_low{};
    LightingVec3 sun_high{};
    LightingVec3 ambient_low{};
    LightingVec3 ambient_high{};
    LightingVec3 fog_color{};
    float fog_blend = 1.0F;
    float fog_distance = 1.0F;
    float cloud_cover = 0.0F;
    float cloud_cutoff = 0.0F;
    float cloud_color = 0.0F;
    float cloud_width = 4.0F;
    float cloud_height = 2.0F;
    float cloud_radius = 4.0F;
    std::uint32_t cloud_number = 100;
    float cloud_base_speed = 0.01F;
    LightingSource source_kind = LightingSource::source_evidenced;
};

[[nodiscard]] const std::array<WeatherPreset, 7>& stockWeatherPresets() noexcept;
[[nodiscard]] const WeatherPreset& defaultWeatherPreset() noexcept;

[[nodiscard]] WeatherPreset parseKsWeatherLighting(
    std::string_view color_curves_text, std::string_view weather_text,
    std::string source = "weather", apex::core::ParseLimits limits = {});

struct EvaluatedLighting {
    WeatherPreset preset;
    LightingVec3 sun_direction{};
    float sun_height = 0.0F;
    float angle_mix = 1.0F;
    LightingVec3 horizon_color{};
    LightingVec3 sky_color{};
    LightingVec3 sun_color{};
    LightingVec3 ambient_color{};
    LightingVec3 fog_color{};
    float fog_blend = 1.0F;
    float fog_distance = 1.0F;
    LightingSource source_kind = LightingSource::source_evidenced;
};

[[nodiscard]] EvaluatedLighting evaluateKsLighting(const WeatherPreset& preset,
                                                   LightingVec3 sun_direction);
[[nodiscard]] LightingVec3 sunDirectionFromAngles(float heading_degrees, float height_degrees) noexcept;

inline constexpr float ks_editor_exposure_min = 0.2F;
inline constexpr float ks_editor_exposure_max = 0.5F;
inline constexpr float ks_editor_exposure_target = 0.32F;
inline constexpr float ks_editor_exposure_gamma = 1.2F;
inline constexpr float ks_editor_exposure_saturation = 0.95F;
inline constexpr float ks_editor_tonemap_curve_scale = 2.6581413745880127F;
inline constexpr float ks_editor_tonemap_curve_shoulder = 0.6653175950050354F;
inline constexpr float ks_editor_tonemap_input_floor = 1.0F / 16384.0F;
inline constexpr float ks_editor_tonemap_output_epsilon = 1.0F / 4194304.0F;
inline constexpr float ks_editor_glare_threshold = 5.0F;
inline constexpr float ks_editor_glare_luminance = 1.6F;
inline constexpr float ks_editor_glare_composite_base = 0.035F;
inline constexpr float ks_editor_glare_shape_luminance = 5.0F;
inline constexpr float ks_editor_glare_shape_bloom_luminance = 0.038F;
inline constexpr std::size_t ks_editor_bloom_kernel_samples = 15;
inline constexpr std::array<float, 4> ks_editor_bloom_dispersion = {
    1.0F, 5.399999736e-7F / 6.149999763e-7F, 4.649999994e-7F / 6.149999763e-7F, 0.0F};

[[nodiscard]] float ksEditorAutoExposure(float luminance, float target = ks_editor_exposure_target,
                                          float minimum = ks_editor_exposure_min,
                                          float maximum = ks_editor_exposure_max) noexcept;
[[nodiscard]] LightingVec3 ksEditorYebisToneMap(
    LightingVec3 rgb, float exposure = 1.0F, float gamma = ks_editor_exposure_gamma,
    float saturation = ks_editor_exposure_saturation,
    float curve_scale = ks_editor_tonemap_curve_scale,
    float curve_shoulder = ks_editor_tonemap_curve_shoulder) noexcept;
[[nodiscard]] LightingVec3 ksEditorGlareBrightPass(
    LightingVec3 rgb, float exposure = 1.0F, float threshold = ks_editor_glare_threshold,
    float remap = 1.0F) noexcept;
[[nodiscard]] float ksEditorBloomCompositeScale(
    float range = 1.0F, float glare_luminance = ks_editor_glare_luminance,
    float shape_luminance = ks_editor_glare_shape_luminance,
    float bloom_luminance = ks_editor_glare_shape_bloom_luminance) noexcept;

struct BloomKernelMetadata {
    float sigma = 0.0F;
    std::array<float, 4> channel_sigmas{};
    std::uint32_t sample_count = 1;
    std::array<float, 15> offsets{};
    std::array<float, 15> weights{};
    LightingSource source_kind = LightingSource::source_evidenced;
};

[[nodiscard]] BloomKernelMetadata ksEditorBloomGaussianKernel(
    std::int32_t level = 0, float threshold = 0.002F, float radius_scale = 0.95F,
    std::int32_t source_level = 2, float display_scale = 2.2F,
    std::array<float, 4> dispersion = ks_editor_bloom_dispersion) noexcept;

struct LineLightSample {
    LightingVec3 point{};
    float value = 0.0F;
    float clamped_value = 0.0F;
    float distance_inverse = 0.0F;
    LightingVec3 color{};
};
[[nodiscard]] LineLightSample cspLineLightSample(LightingVec3 from, LightingVec3 to,
                                                  LightingVec3 position, LightingVec3 color_from,
                                                  LightingVec3 color_to) noexcept;
[[nodiscard]] bool cspLightReceiverVisible(std::string_view view_mode, bool interior_view,
                                            std::string_view track_mode = "all",
                                            bool track_receiver = false) noexcept;
[[nodiscard]] float cspLightDistanceFade(float distance, float fade_at = 200.0F,
                                          float fade_smooth = 80.0F) noexcept;

struct SpotConePacking {
    bool enabled = false;
    LightingVec3 direction{};
    float start = 0.0F;
    float outer_cos = -1.0F;
    float inner_cos = 1.0F;
    float inverse_width = 0.0F;
    float half_angle = 0.0F;
    float sharpness = 0.0F;
};
[[nodiscard]] SpotConePacking cspSpotConePacking(LightingVec3 direction, float spot_degrees,
                                                 float spot_sharpness = 0.8F) noexcept;
[[nodiscard]] float cspSpotConeFactor(LightingVec3 direction, LightingVec3 to_light,
                                       float spot_degrees, float spot_sharpness = 0.8F) noexcept;
struct SpotEdgePacking { bool enabled = false; LightingVec3 up{}; LightingVec3 offsets{1, 1, 1}; float sharpness = 0; };
[[nodiscard]] SpotEdgePacking cspSpotEdgePacking(LightingVec3 up, LightingVec3 edge, float sharpness) noexcept;
[[nodiscard]] LightingVec3 cspSpotEdgeFactors(LightingVec3 up, LightingVec3 edge,
                                               float sharpness, LightingVec3 to_light) noexcept;
struct SecondarySpotPacking { bool enabled = false; float range_inverse = 0; float trim_start = 0; float trim_length_inverse = 0; float skip = 0; };
[[nodiscard]] SecondarySpotPacking cspSecondarySpotPacking(float range, float skip) noexcept;
[[nodiscard]] float cspSecondarySpotAttenuation(float distance, float range, float skip) noexcept;

inline constexpr std::uint32_t ks_shadow_map_size = 2048;
// CameraShadowMapped's installed ksEditor constructor uses these main-
// viewport cascade limits. The separate PreviewBuilder configuration uses
// different 2/12/50 values and must not be attributed to ksEditor.
inline constexpr std::array<float, 3> ks_editor_shadow_splits = {
    1.8F, 20.0F, 180.0F};
inline constexpr float ks_editor_shadow_range = 500.0F;
inline constexpr std::array<float, 3> ks_shadow_splits = {2.0F, 12.0F, 50.0F};
inline constexpr std::array<float, 3> ks_shadow_biases = {0.000002F, 0.000015F, 0.0003F};
inline constexpr LightingVec3 ks_sun_direction = {0.35F, 0.82F, 0.42F};
inline constexpr std::uint32_t csp_local_shadow_atlas_size = 1024;
inline constexpr std::uint32_t csp_local_shadow_cell_size = 512;
inline constexpr std::uint32_t csp_local_shadow_limit = 4;
inline constexpr std::uint32_t csp_local_shadow_samples = 4;
inline constexpr float csp_local_shadow_default_exp_factor = 20.0F;
inline constexpr float csp_local_shadow_default_clip_plane = 0.5F;
inline constexpr float csp_local_shadow_default_clip_sphere = 0.5F;
inline constexpr float csp_local_shadow_world_bias = 0.3F;

struct ShadowFilter { std::uint32_t mode = 0, kernel = 7; std::array<float, 4> weights{}; std::array<float, 4> offsets{}; bool value_aware = false; };
[[nodiscard]] ShadowFilter cspLocalShadowFilter(std::string_view section, bool extra_blur,
                                                 std::string_view source = {}) noexcept;
struct ExponentialShadowInput {
    float range = 10.0F, shadow_range = 0.0F, exp_factor = csp_local_shadow_default_exp_factor;
    float shadow_spot = 0.0F, shadow_boost = 0.0F, shadow_clip_plane = csp_local_shadow_default_clip_plane, shadow_clip_sphere = csp_local_shadow_default_clip_sphere;
    bool shadow_range_authored = true, shadow_spot_authored = false, shadow_clip_sphere_authored = false;
    bool shadow_extra_blur = false, vehicle_attached = false;
};
struct ExponentialShadowParams { float max_range=10, range_inverse=.1F, range_inverse_exp_factor=2, exp_factor=20, bias_mult=0.006F, thickness_fix_mult=10, thickness_fix_add=-9, boost=10, clip_plane=.5F, clip_sphere=.5F; bool extra_blur=false; };
[[nodiscard]] ExponentialShadowParams cspExponentialShadowParams(const ExponentialShadowInput& input) noexcept;
[[nodiscard]] float resolveCspExponentialShadow(float occluder, float receiver_distance,
                                                 float bias_factor, const ExponentialShadowParams& params) noexcept;

struct DirectionalShadowInput { LightingVec3 eye{0,0,5}, target{}, up{0,1,0}, sun_direction = ks_sun_direction; float fov_radians=0.785398F, aspect=1, near_plane=.01F, far_plane=50, scene_radius=1; std::array<float,3> splits=ks_shadow_splits; std::uint32_t map_size=ks_shadow_map_size; };
struct ShadowCascade { std::uint32_t index=0; float near_plane=0, far_plane=0, radius=0, texel_world_size=0; LightingVec3 center{}; LightingMat4 matrix{}; };
struct DirectionalShadowResult { std::vector<ShadowCascade> cascades; std::vector<float> splits; LightingVec3 forward{}, light_direction{}; std::uint32_t map_size=0; LightingSource source_kind=LightingSource::source_evidenced; };
[[nodiscard]] DirectionalShadowResult computeDirectionalShadowCascades(const DirectionalShadowInput& input);

enum class DirectionalShadowClipSpaceStatus : std::uint8_t {
    ready,
    invalid_input,
    unsupported,
};

struct DirectionalShadowClipSpaceResult {
    DirectionalShadowClipSpaceStatus status =
        DirectionalShadowClipSpaceStatus::unsupported;
    LightingMat4 matrix{};
    std::string code;
    std::string message;

    [[nodiscard]] bool ok() const noexcept {
        return status == DirectionalShadowClipSpaceStatus::ready;
    }
};

// Convert one existing WebGL-style, column-major cascade matrix to the
// selected native clip convention. Native depth maps z from [-1, 1] to [0, 1];
// Vulkan additionally flips clip-space y. Cascade computation is unchanged.
[[nodiscard]] DirectionalShadowClipSpaceResult convertDirectionalShadowCascadeMatrix(
    const LightingMat4& webgl_matrix, CameraClipSpace native_clip_space);

struct ProbeShadowInput { LightingVec3 eye{}, sun_direction = ks_sun_direction; std::array<float,3> splits=ks_shadow_splits; float scene_radius=1; std::uint32_t map_size=ks_shadow_map_size; };
[[nodiscard]] DirectionalShadowResult computeDirectionalProbeShadowCascades(const ProbeShadowInput& input);
struct LocalShadowInput { LightingVec3 position{}, direction{0,-1,0}; float spot=90, range=10; ExponentialShadowInput esm{}; };
struct LocalShadowResult { LightingMat4 matrix{}; LightingVec3 position{}, direction{}; float spot=90, near_plane=.5F, far_plane=10; ExponentialShadowParams esm{}; LightingSource source_kind=LightingSource::source_evidenced; };
[[nodiscard]] LocalShadowResult computeLocalLightShadow(const LocalShadowInput& input);
[[nodiscard]] bool shadowCasterEnabled(bool node_cast_shadows, std::optional<bool> override_cast_shadows) noexcept;

struct CubemapConfig { std::uint32_t size=512, faces_per_frame=1; float near_plane=.01F, far_plane=500, fov_degrees=90; std::uint32_t shader_slot=10; LightingSource source_kind=LightingSource::source_evidenced; };
struct CubemapFace { std::array<std::int8_t,3> direction{}; std::array<std::int8_t,3> up{}; std::string target; };
[[nodiscard]] const CubemapConfig& ksEditorCubemap() noexcept;
[[nodiscard]] const std::array<CubemapFace,6>& webglCubemapFaces() noexcept;
struct ReflectionCaptureItem { std::string name; bool transparent=false, environment=false, explicit_member=false; };
enum class ReflectionCaptureMode : std::uint8_t { disabled, explicit_subtree, environment, scene, fallback };
struct ReflectionCaptureSelection { std::vector<std::size_t> item_indices; ReflectionCaptureMode mode=ReflectionCaptureMode::fallback; std::string root_name; std::string reason; LightingSource source_kind=LightingSource::source_evidenced; };
[[nodiscard]] ReflectionCaptureSelection selectReflectionCaptureItems(std::span<const ReflectionCaptureItem> items,
                                                                        std::string_view explicit_root = {},
                                                                        std::string_view workspace_kind = {},
                                                                        float bounds_radius = 0, bool isolated = false);
[[nodiscard]] float reflectionSmoothMinimum(float a, float b) noexcept;
[[nodiscard]] float reflectionFresnel(float facing, float fresnel_c, float fresnel_exponent,
                                      float fresnel_maximum, float reflection_multiplier=1) noexcept;
struct ReflectionBlur { float base=0, mip=0; };
[[nodiscard]] ReflectionBlur reflectionBlurFromExponent(float specular_exponent) noexcept;
struct ReflectionFallback { LightingVec3 color{}; LightingSource source_kind=LightingSource::procedural_fallback; };
[[nodiscard]] ReflectionFallback portableReflectionEnvironment(LightingVec3 direction, float blur,
                                                                LightingVec3 sky_color, LightingVec3 horizon_color,
                                                                LightingVec3 fog_color, float fog_blend) noexcept;

} // namespace apex::render
