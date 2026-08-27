#pragma once

#include "apex/render/device.hpp"

#include <array>
#include <cstdint>

namespace apex::render {

// These variants describe the four installed stock packages whose reflection
// arithmetic has been recovered from DXBC. Pixel blob SHA-256 identities:
// base b5cc236cef21d3d01f7a80510e64b7a9a21bf5727c0f5a0b93b6e2fdf1f18a58,
// AT 0f354b59932a913f9e5e9b9ccbfda4fdeca15d271d07e5e4522f99821c72e8ab,
// NMDetail 2e45a59c20284802cda77ae02488d0f44aaa8955492767e75dcd0c29b7631222,
// AT_NMDetail 51fd6d6c1bc4f18c1d978ddde362d09391fd719fac80ca9b347fdb95e743e4d6.
// These variants do not describe the portable bindings used to transport the
// cube and semantic controls.
enum class StockMultiMapReflectionVariant : std::uint8_t {
    base,
    alpha_tested,
    normal_detail,
    alpha_tested_normal_detail,
};

enum class StockMultiMapReflectionBranch : std::uint8_t {
    additive,
    special_lerp,
    default_lerp,
};

enum class StockMultiMapCubeSampleOperation : std::uint8_t {
    explicit_level,
    bias,
};

struct StockMultiMapReflectionInput {
    StockMultiMapReflectionVariant variant =
        StockMultiMapReflectionVariant::base;
    KsPerPixelMultiMapReflectionConstants controls{};
    // Both vectors use the installed vertex/pixel-stage convention. The view
    // vector points from the camera to the surface.
    std::array<float, 3U> surface_normal = {0.0F, 1.0F, 0.0F};
    std::array<float, 3U> camera_to_surface = {0.0F, -1.0F, 0.0F};
    float maps_specular_exponent = 1.0F;
    float ks_specular_exponent = 30.0F;
    float maps_reflection = 1.0F;
    std::array<float, 3U> lit_rgb{};
    std::array<float, 3U> cube_rgb{};
    // Alpha-tested packages preserve their already-computed material alpha.
    float material_alpha = 1.0F;
};

struct StockMultiMapReflectionResult {
    bool ready = false;
    Diagnostic diagnostic;
    StockMultiMapReflectionBranch branch =
        StockMultiMapReflectionBranch::default_lerp;
    StockMultiMapCubeSampleOperation sample_operation =
        StockMultiMapCubeSampleOperation::explicit_level;
    // DirectX cubemap coordinate after the recovered X-orientation handling.
    std::array<float, 3U> cube_direction{};
    float mip_level = 0.0F;
    float fresnel = 0.0F;
    std::array<float, 3U> rgb{};
    float alpha = 1.0F;
};

// Evaluate only the source-evidenced reflection portion of the installed
// MultiMap pixel shaders. Direct lighting and the sampled cube value are
// explicit inputs so this function remains a deterministic semantic oracle,
// not a CPU renderer or a claim about the portable resource ABI.
[[nodiscard]] StockMultiMapReflectionResult
evaluate_stock_multimap_reflection(
    const StockMultiMapReflectionInput& input) noexcept;

}  // namespace apex::render
