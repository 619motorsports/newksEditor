#pragma once

#include "apex/render/camera.hpp"
#include "apex/platform/native_surface.hpp"
#include "apex/render/pipeline.hpp"
#include "apex/render/stock_ks_per_pixel.hpp"
#include "apex/render/texture_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace apex::render {

struct DrawPacket;
class Sampler;
class Device;
class StockKsPerPixelNativeShaderProgram;
class StockKsPerPixelNativeConstantBuffers;
class StockKsPerPixelNativeSamplers;

/** Graphics APIs supported by the native renderer. */
enum class Backend {
    Vulkan,
    D3D12,
};

/** Stable status values returned by backend discovery and initialization. */
enum class DeviceStatus {
    ready,
    unavailable,
    invalid_options,
    initialization_failed,
};

struct Diagnostic {
    std::string code;
    std::string message;
};

/** Options shared by all backend implementations. */
struct DeviceOptions {
    // Headless remains the default. Set headless=false and provide
    // native_surface to opt into Vulkan native-window presentation.
    bool headless = true;
    bool enable_validation = false;
    bool allow_software = true;
    // Opt into the Vulkan headless-surface prerequisites. This does not
    // create a target; callers must still request one from the device.
    bool enable_headless_presentation = false;
    // Prefer a software adapter when one is available. D3D12 resolves WARP
    // explicitly; Vulkan prioritizes CPU physical devices.
    bool prefer_software = false;
    std::uint32_t adapter_index = 0;
    // A native presentation source selects the Vulkan WSI surface supplied by
    // the platform layer. It is mutually exclusive with headless mode.
    std::optional<apex::platform::NativeSurfaceSource> native_surface;
};

struct DeviceInfo {
    Backend backend = Backend::Vulkan;
    std::string name;
    std::string driver;
    std::uint32_t api_major = 0;
    std::uint32_t api_minor = 0;
    std::uint32_t api_patch = 0;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    bool software = false;
};

/** Resource memory classes exposed by the backend-neutral buffer contract. */
enum class BufferMemory : std::uint8_t {
    device_local,
    host_visible,
};

/** Whether a buffer may be updated after creation. */
enum class BufferMutability : std::uint8_t {
    immutable,
    mutable_data,
};

/** Usage bits are intentionally API-neutral and may be combined. */
enum class BufferUsage : std::uint32_t {
    none = 0,
    transfer_source = 1U << 0U,
    transfer_destination = 1U << 1U,
    vertex = 1U << 2U,
    index = 1U << 3U,
    uniform = 1U << 4U,
    storage = 1U << 5U,
};

[[nodiscard]] constexpr BufferUsage operator|(BufferUsage left, BufferUsage right) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(left) |
                                    static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr BufferUsage operator&(BufferUsage left, BufferUsage right) noexcept {
    return static_cast<BufferUsage>(static_cast<std::uint32_t>(left) &
                                    static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool any(BufferUsage usage) noexcept {
    return static_cast<std::uint32_t>(usage) != 0U;
}

inline constexpr std::uint64_t max_buffer_bytes = 1ULL << 34U;
inline constexpr std::uint64_t max_texture_bytes = 1ULL << 34U;
inline constexpr std::uint32_t max_texture_dimension = 16384U;
inline constexpr std::uint32_t max_presentation_image_count = 8U;

// These flags report API prerequisites only. They do not prove that a native
// window, presentation surface, present-capable queue, or swapchain exists.
struct PresentationCapabilities {
    bool offscreen = true;
    bool swapchain_api_available = false;
    bool native_surface_api_available = false;
    bool headless_surface_api_available = false;
};

struct PresentationTargetDescription {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t image_count = 2U;
    TextureFormat format = TextureFormat::bgra8_srgb;
    bool vsync = true;
};

enum class PresentationTargetStatus : std::uint8_t {
    ready,
    invalid_description,
    unsupported,
    allocation_failed,
    execution_failed,
};

struct PresentationTargetInfo {
    PresentationTargetDescription description{};
};

class PresentationTarget {
public:
    virtual ~PresentationTarget() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const PresentationTargetInfo& info() const noexcept = 0;
};

struct PresentationTargetResult {
    PresentationTargetStatus status = PresentationTargetStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<PresentationTarget> target;

    [[nodiscard]] bool ok() const noexcept {
        return status == PresentationTargetStatus::ready && target != nullptr;
    }
};

enum class PresentationFrameStatus : std::uint8_t {
    ready,
    invalid_request,
    unsupported,
    execution_failed,
};

struct PresentationFrameResult {
    PresentationFrameStatus status = PresentationFrameStatus::unsupported;
    Diagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return status == PresentationFrameStatus::ready;
    }
};

struct BufferDescription {
    std::uint64_t size_bytes = 0;
    BufferUsage usage = BufferUsage::none;
    BufferMemory memory = BufferMemory::device_local;
    BufferMutability mutability = BufferMutability::immutable;
};

enum class BufferStatus {
    ready,
    invalid_description,
    unsupported,
    allocation_failed,
    upload_failed,
};

struct BufferInfo {
    BufferDescription description{};
};

class Buffer {
public:
    virtual ~Buffer() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const BufferInfo& info() const noexcept = 0;
};

struct BufferResult {
    BufferStatus status = BufferStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<Buffer> buffer;

    [[nodiscard]] bool ok() const noexcept {
        return status == BufferStatus::ready && buffer != nullptr;
    }
};

struct BufferUpdateResult {
    BufferStatus status = BufferStatus::unsupported;
    Diagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept { return status == BufferStatus::ready; }
};

[[nodiscard]] constexpr std::size_t texture_format_bytes_per_pixel(TextureFormat format) noexcept {
    return texture_format_info(format).bytes_per_pixel;
}

enum class TextureUsage : std::uint32_t {
    none = 0,
    sampled = 1U << 0U,
    transfer_source = 1U << 1U,
    transfer_destination = 1U << 2U,
    color_attachment = 1U << 3U,
    storage = 1U << 4U,
};

[[nodiscard]] constexpr TextureUsage operator|(TextureUsage left, TextureUsage right) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(left) |
                                     static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr TextureUsage operator&(TextureUsage left, TextureUsage right) noexcept {
    return static_cast<TextureUsage>(static_cast<std::uint32_t>(left) &
                                     static_cast<std::uint32_t>(right));
}

enum class TextureMemory : std::uint8_t {
    device_local,
    host_visible,
};

enum class TextureMutability : std::uint8_t {
    immutable,
    mutable_data,
};

struct TextureDescription {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_levels = 1;
    std::uint32_t array_layers = 1;
    TextureFormat format = TextureFormat::rgba8_unorm;
    TextureUsage usage = TextureUsage::none;
    TextureMemory memory = TextureMemory::device_local;
    TextureMutability mutability = TextureMutability::immutable;
    std::uint32_t samples = 1;
};

struct TextureUpload {
    std::uint32_t mip_level = 0;
    std::uint32_t array_layer = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch = 0;
    std::span<const std::byte> data{};
};

struct TextureUploadPlan {
    std::vector<TextureUpload> subresources;
};

enum class TextureStatus {
    ready,
    invalid_description,
    unsupported,
    allocation_failed,
    upload_failed,
};

struct TextureInfo {
    TextureDescription description{};
};

class Texture {
public:
    virtual ~Texture() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const TextureInfo& info() const noexcept = 0;
};

// A persistent, backend-neutral D32 depth attachment. It is deliberately
// separate from Texture: depth images are not CPU-uploadable color textures,
// and their lifetime/state must persist across multiple indexed draws.
enum class DepthAttachmentFormat : std::uint8_t {
    d32_float,
};

struct DepthAttachmentDescription {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t samples = 1;
    DepthAttachmentFormat format = DepthAttachmentFormat::d32_float;
    // Shader-readable depth is an allocation-time capability. It does not
    // make the attachment a CPU-uploadable color Texture.
    bool shader_readable = false;
};

struct DepthAttachmentInfo {
    DepthAttachmentDescription description{};
};

class DepthAttachment {
public:
    virtual ~DepthAttachment() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const DepthAttachmentInfo& info() const noexcept = 0;
};

enum class DepthAttachmentStatus {
    ready,
    invalid_description,
    unsupported,
    allocation_failed,
};

struct DepthAttachmentResult {
    DepthAttachmentStatus status = DepthAttachmentStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<DepthAttachment> attachment;

    [[nodiscard]] bool ok() const noexcept {
        return status == DepthAttachmentStatus::ready && attachment != nullptr;
    }
};

// A bounded synchronous readback of an initialized single-sample D32
// attachment. The result is tightly packed native float32 depth values in
// row-major order. Output dimensions must match the attachment dimensions;
// the request does not resize or filter depth data.
struct DepthAttachmentReadbackRequest {
    std::uint32_t output_width = 0U;
    std::uint32_t output_height = 0U;
};

enum class DepthAttachmentReadbackStatus : std::uint8_t {
    ready,
    invalid_request,
    unsupported,
    execution_failed,
};

struct DepthAttachmentReadbackResult {
    DepthAttachmentReadbackStatus status = DepthAttachmentReadbackStatus::unsupported;
    Diagnostic diagnostic;
    std::vector<float> depth;

    [[nodiscard]] bool ok() const noexcept {
        return status == DepthAttachmentReadbackStatus::ready;
    }
};

struct TextureResult {
    TextureStatus status = TextureStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<Texture> texture;

    [[nodiscard]] bool ok() const noexcept {
        return status == TextureStatus::ready && texture != nullptr;
    }
};

struct TextureUpdateResult {
    TextureStatus status = TextureStatus::unsupported;
    Diagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept { return status == TextureStatus::ready; }
};

// A bounded, synchronous clear/readback operation for 2D RGBA8 textures.
// The returned bytes are tightly packed canonical RGBA8, independent of the
// backend's native channel order. This is an execution contract only; it is
// not a pixel-fidelity claim for a complete renderer.
struct TextureClearReadbackRequest {
    std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    std::uint32_t mip_level = 0;
    std::uint32_t array_layer = 0;
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
};

enum class TextureReadbackStatus {
    ready,
    invalid_request,
    unsupported,
    execution_failed,
};

struct TextureClearReadbackResult {
    TextureReadbackStatus status = TextureReadbackStatus::unsupported;
    Diagnostic diagnostic;
    std::vector<std::byte> rgba8;

    [[nodiscard]] bool ok() const noexcept { return status == TextureReadbackStatus::ready; }
};

// A bounded executable triangle request. The pipeline must contain one
// vertex and one fragment shader, one RGBA8 color target, and no resource
// bindings. The supplied vertex bytes are interpreted using the pipeline's
// vertex layout. This is a real backend draw contract, not a CPU fill or a
// pixel-fidelity claim for the complete renderer.
struct TriangleDrawRequest {
    const PipelineProgram* pipeline = nullptr;
    std::span<const std::byte> vertex_data{};
    std::uint32_t vertex_count = 3U;
    std::uint32_t mip_level = 0U;
    std::uint32_t array_layer = 0U;
    std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
};

enum class TriangleDrawStatus {
    ready,
    invalid_request,
    unsupported,
    execution_failed,
};

struct TriangleDrawResult {
    TriangleDrawStatus status = TriangleDrawStatus::unsupported;
    Diagnostic diagnostic;
    std::vector<std::byte> rgba8;

    [[nodiscard]] bool ok() const noexcept { return status == TriangleDrawStatus::ready; }
};

// Indexed static-mesh execution consumes persistent backend-neutral buffers.
// Offsets in DrawPacket are interpreted as element offsets: vertex_offset is
// measured in pipeline-stride vertices and index_offset in uint16 indices.
// This baseline deliberately has no material/resource binding. Transform
// execution uses the explicit PipelineTransformContract::draw_matrices ABI.
enum class StaticMeshIndexType : std::uint8_t { uint16 };

// Production draw packets remain neutral until their material shaders are
// linked. A caller that supplies backend-executable shader modules can grant
// that authority to one request without mutating or relabeling the packet.
enum class IndexedShaderAuthority : std::uint8_t {
    packet_contract,
    explicit_pipeline,
    explicit_stock_ks_per_pixel_native,
    // Structural Vulkan transport probe for the recovered native ABI. This
    // does not claim exact installed shader behavior or translated DXBC.
    explicit_stock_ks_per_pixel_vulkan_abi_probe,
};

// Material resources remain staged unless a caller supplies the complete
// portable diffuse-resource ABI for one synchronous request. This authority
// does not claim that a stock KN5 or CSP shader contract was recovered.
enum class IndexedResourceAuthority : std::uint8_t {
    packet_contract,
    explicit_bindings,
};

struct IndexedSampledTextureBinding {
    // Non-owning. Keep both handles alive until the synchronous draw returns.
    const Texture* texture = nullptr;
    const Sampler* sampler = nullptr;
};

class StockKsPerPixelNativeDrawResources;

// Exact installed ksPerPixel resource binding. The request does not own these
// objects. Keep every handle alive until the synchronous draw returns.
struct StockKsPerPixelNativeDrawBinding {
    const StockKsPerPixelNativeDrawResources* resources = nullptr;
    const Texture* diffuse_texture = nullptr;
    std::array<const DepthAttachment*, 3U> shadow_maps{};
};

enum class StockKsPerPixelVulkanAbiUniformSlot : std::uint8_t {
    camera,
    object,
    lighting,
    shadow_maps,
    material,
    count,
};

// Non-owning descriptor views for the recovered Vulkan ABI probe. Every view
// is one bounded 256-byte uniform record; the actual recovered records occupy
// only the first 224/64/160/208/32 bytes respectively.
struct StockKsPerPixelVulkanAbiUniformBufferView {
    const Buffer* buffer = nullptr;
    std::uint64_t offset_bytes = 0U;
    std::uint32_t range_bytes = 0U;
};

struct StockKsPerPixelVulkanAbiProbeDrawBinding {
    std::array<StockKsPerPixelVulkanAbiUniformBufferView,
               static_cast<std::size_t>(
                   StockKsPerPixelVulkanAbiUniformSlot::count)>
        uniform_buffers{};
    const Texture* diffuse_texture = nullptr;
    std::array<const DepthAttachment*, 3U> shadow_maps{};
    const Sampler* linear_sampler = nullptr;
    const Sampler* shadow_sampler = nullptr;
};

// Recovered stock ksShadowGenAT cbMaterial packing. The installed pixel
// shader reads ksAlphaRef from byte offset 28 of a 32-byte b4 record.
struct StockShadowCasterMaterialConstants {
    // ksAmbient, ksDiffuse, ksSpecular, ksSpecularEXP.
    std::array<float, 4> lighting{};
    // ksEmissive RGB and ksAlphaRef.
    std::array<float, 4> emissive_and_alpha_ref{};
};

static_assert(sizeof(StockShadowCasterMaterialConstants) == 32U);
static_assert(offsetof(StockShadowCasterMaterialConstants,
                       emissive_and_alpha_ref) == 16U);
static_assert(std::is_trivially_copyable_v<StockShadowCasterMaterialConstants>);

inline constexpr std::uint32_t stock_shadow_caster_material_bytes = 32U;
inline constexpr std::uint32_t stock_shadow_caster_buffer_alignment = 256U;

struct StockShadowCasterMaterialBinding {
    // Non-owning. Keep this buffer alive until the synchronous draw returns.
    // The explicit alpha-tested pipeline declares set 0/binding 4. D3D12
    // maps that binding to b4, which matches the recovered stock shader.
    const Buffer* buffer = nullptr;
    std::uint64_t offset_bytes = 0U;
    std::uint32_t range_bytes = 0U;
};

inline constexpr std::size_t indexed_directional_shadow_cascade_count = 3U;

// Portable receiver packing. This is source-evidenced behavior carried by a
// new cross-backend ABI; it is not a recovered native register layout.
struct DirectionalShadowReceiverConstants {
    std::array<apex::scene::Matrix4, indexed_directional_shadow_cascade_count>
        shadow_matrices{};
    std::array<float, 4> split_distances = {2.0F, 12.0F, 50.0F, 0.0F};
    std::array<float, 4> depth_biases = {0.000002F, 0.000015F, 0.0003F, 0.0F};
    std::array<float, 4> camera_position{};
    std::array<float, 4> camera_forward = {0.0F, 0.0F, -1.0F, 0.0F};
};

static_assert(sizeof(DirectionalShadowReceiverConstants) == 256U);
static_assert(std::is_trivially_copyable_v<DirectionalShadowReceiverConstants>);

inline constexpr std::uint32_t portable_directional_shadow_buffer_view_bytes = 256U;

// The receiver layout is an explicit caller choice because shader modules
// own the ABI. Portable remains the default; stock_ks_shadow_maps selects the
// recovered D3D11 cbShadowMaps byte layout below.
enum class DirectionalShadowReceiverConstantsLayout : std::uint8_t {
    portable,
    stock_ks_shadow_maps,
};

struct IndexedDirectionalShadowBinding {
    // Non-owning. All three maps and the sampler must remain alive until the
    // synchronous draw returns. The sampler is nearest/clamp-to-edge with
    // comparison disabled because the receiver performs explicit 3x3 PCF.
    std::array<const DepthAttachment*, indexed_directional_shadow_cascade_count>
        maps{};
    const Sampler* sampler = nullptr;
    const Buffer* constants = nullptr;
    std::uint64_t constants_offset_bytes = 0U;
    std::uint32_t constants_range_bytes = 0U;
};

// Values and semantic order follow the current production WebGL ksPerPixel
// binder. The std140/HLSL-compatible packing is this portable test ABI.
// Shader execution remains a separate, explicitly authorized contract.
struct KsPerPixelMaterialConstants {
    // ksAmbient, ksDiffuse, ksSpecular, ksSpecularEXP.
    std::array<float, 4> lighting = {0.35F, 0.80F, 0.20F, 30.0F};
    // fresnelC, fresnelEXP, fresnelMaxLevel, opaque-main-pass alphaRef.
    std::array<float, 4> fresnel = {0.0F, 5.0F, 0.05F, 0.0F};
    // RGB emissive color and reserved padding.
    std::array<float, 4> emissive = {0.0F, 0.0F, 0.0F, 0.0F};
    // useDetail, detailUVMultiplier, detailNormalBlend, reserved. These
    // values preserve the public/app.js generic detail-stack controls.
    std::array<float, 4> detail = {0.0F, 1.0F, 1.0F, 0.0F};
    // CSP damageZones.x/y/z/w. The source-evidenced bounded damage stage
    // evaluates saturate(txDamage.a * dot(txDamageMask, damage_zones)).
    std::array<float, 4> damage_zones = {0.0F, 0.0F, 0.0F, 0.0F};
};

static_assert(sizeof(KsPerPixelMaterialConstants) == 80U);
static_assert(std::is_trivially_copyable_v<KsPerPixelMaterialConstants>);

// Per-frame lighting values follow the production WebGL ksPerPixel binder.
// Static-scene execution derives camera_position from its CameraFrame so
// view-dependent lighting cannot use stale duplicate state. The first 64
// bytes retain the original portable directional-lighting ABI.
struct KsPerPixelFrameConstants {
    std::array<float, 4> sun_direction = {0.0F, 1.0F, 0.0F, 0.0F};
    std::array<float, 4> sun_color = {1.0F, 1.0F, 1.0F, 0.0F};
    std::array<float, 4> ambient_color = {0.2F, 0.2F, 0.2F, 0.0F};
    std::array<float, 4> camera_position = {0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> horizon_color = {0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> sky_color = {0.0F, 0.0F, 0.0F, 0.0F};
    std::array<float, 4> fog_color = {0.0F, 0.0F, 0.0F, 0.0F};
    // fogDistance, fogBlend, enabled, reserved.
    std::array<float, 4> fog = {1.0F, 1.0F, 0.0F, 0.0F};
};

static_assert(sizeof(KsPerPixelFrameConstants) == 128U);
static_assert(std::is_trivially_copyable_v<KsPerPixelFrameConstants>);

inline constexpr std::uint32_t portable_material_constant_bytes = 80U;
inline constexpr std::uint32_t portable_material_buffer_view_bytes = 256U;
inline constexpr std::uint32_t portable_frame_constant_bytes = 128U;
inline constexpr std::uint32_t portable_frame_buffer_view_bytes = 256U;

struct IndexedMaterialBufferBinding {
    // Non-owning. Keep this buffer alive until the synchronous draw returns.
    const Buffer* buffer = nullptr;
    std::uint64_t offset_bytes = 0U;
    // The cross-backend view is one D3D12-aligned 256-byte record. The typed
    // ksPerPixel values occupy its first 80 bytes.
    std::uint32_t range_bytes = 0U;
};

struct IndexedFrameBufferBinding {
    // Non-owning. Keep this buffer alive until the synchronous draw returns.
    // The executable pipeline must declare a uniform buffer at set 0/binding
    // 3. The view is one D3D12/Vulkan-aligned 256-byte record.
    const Buffer* buffer = nullptr;
    std::uint64_t offset_bytes = 0U;
    std::uint32_t range_bytes = 0U;
};

enum class IndexedPortableResourceLayout : std::uint8_t {
    resource_free,
    diffuse,
    diffuse_with_constants,
    diffuse_with_frame,
    diffuse_with_constants_and_frame,
    // Exact bounded ksPerPixelNM ABI: diffuse at bindings 0/1, material and
    // frame uniforms at 2/3, and tangent-space normal texture at bindings 4/5.
    diffuse_normal_with_constants_and_frame,
    // Exact bounded txMaps extension: the ksPerPixelNM bindings above plus
    // the linear maps texture and sampler at bindings 6/7.
    diffuse_normal_maps_with_constants_and_frame,
    // Exact generic ksPerPixelMultiMap_NMDetail stack: txDetail at 8/9 and
    // txNormalDetail at 10/11, with the 80-byte material record.
    diffuse_normal_maps_detail_stack_with_constants_and_frame,
    // Exact dirt-zero ksPerPixelMultiMap_damage_dirt branch: txDamage at
    // 12/13 and txDamageMask at 14/15. Bindings 8-11 stay reserved for the
    // mutually exclusive generic detail stack.
    diffuse_normal_maps_damage_with_constants_and_frame,
    // Dirt-zero damage branch with the stock txDust alpha input at bindings
    // 8/9. This is a damage-only layout; generic detail remains exclusive.
    diffuse_normal_maps_damage_dust_with_constants_and_frame,
    unsupported,
};

// Classify the only portable resource layouts that the indexed executor can
// run. Binding names do not affect the ABI. Sets, bindings, and kinds do.
[[nodiscard]] IndexedPortableResourceLayout classify_indexed_portable_resource_layout(
    const PipelineProgram& pipeline) noexcept;

// A complete receiver extension is three sampled D32 images at bindings
// 16-18, one sampler at 19, and one uniform buffer at 20. The extension is
// orthogonal to the classified material layout.
[[nodiscard]] bool pipeline_declares_directional_shadow_receiver(
    const PipelineProgram& pipeline) noexcept;

inline constexpr std::uint32_t max_indexed_static_mesh_vertices = 10'000'000U;
inline constexpr std::uint32_t max_indexed_static_mesh_indices = 20'000'000U;

// Both matrices use the column-major layout used by SceneNode::transform and
// public/app.js. The shader evaluates view_projection * world * position.
struct DrawMatrices {
    apex::scene::Matrix4 world = apex::scene::identity_matrix;
    apex::scene::Matrix4 view_projection = apex::scene::identity_matrix;
};

static_assert(sizeof(DrawMatrices) == 32U * sizeof(float));
static_assert(std::is_trivially_copyable_v<DrawMatrices>);

// World-space colored line geometry used by editor overlays. The fixed ABI
// keeps overlay pipelines backend-neutral and leaves material descriptors out
// of the authoring pass.
struct OverlayLineVertex {
    std::array<float, 3> position{};
    std::array<float, 3> color{};
};

static_assert(sizeof(OverlayLineVertex) == 6U * sizeof(float));
static_assert(std::is_trivially_copyable_v<OverlayLineVertex>);

struct OverlayLineDrawRequest {
    const PipelineProgram* pipeline = nullptr;
    const Buffer* vertex_buffer = nullptr;
    std::uint64_t vertex_offset_bytes = 0U;
    std::uint32_t vertex_count = 0U;
    DrawMatrices matrices{};
    // Insert before this element of IndexedStaticMeshBatchDescription::draws.
    // max() appends the line draw after all indexed scene draws.
    std::uint32_t scene_position = std::numeric_limits<std::uint32_t>::max();
};

inline constexpr std::size_t max_overlay_line_draws = 16U;
inline constexpr std::uint32_t max_overlay_line_vertices = 4096U;
inline constexpr std::uint32_t max_overlay_line_total_vertices = 65'536U;

struct IndexedStaticMeshDrawRequest {
    const DrawPacket* packet = nullptr;
    const PipelineProgram* pipeline = nullptr;
    const Buffer* vertex_buffer = nullptr;
    const Buffer* index_buffer = nullptr;
    StaticMeshIndexType index_type = StaticMeshIndexType::uint16;
    std::uint32_t mip_level = 0U;
    std::uint32_t array_layer = 0U;
    std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    // The request owns this copy. The clip-space convention must match the
    // target backend before command recording begins.
    std::optional<CameraFrame> camera_frame;
    // Non-owning. Keep the attachment alive across all indexed draws in a
    // frame so depth contents persist between requests.
    DepthAttachment* depth_attachment = nullptr;
    // The default preserves the existing one-draw behavior. Set load_color
    // for subsequent draws that must retain the color attachment contents.
    bool load_color = false;
    // Clear depth on the first draw of a frame. Later draws load the existing
    // attachment contents. The clear value is the normalized D32 depth.
    bool clear_depth = false;
    float depth_clear_value = 1.0F;
    IndexedShaderAuthority shader_authority = IndexedShaderAuthority::packet_contract;
    IndexedResourceAuthority resource_authority = IndexedResourceAuthority::packet_contract;
    // Diffuse sampled image and sampler at set 0/bindings 0 and 1.
    IndexedSampledTextureBinding sampled_binding{};
    // Optional normal sampled image and sampler at set 0/bindings 4 and 5.
    IndexedSampledTextureBinding normal_binding{};
    // Optional txMaps sampled image and sampler at set 0/bindings 6 and 7.
    IndexedSampledTextureBinding maps_binding{};
    // Optional txDetail or mutually exclusive txDust sampled image and sampler
    // at set 0/bindings 8 and 9. The portable resource layout selects the ABI.
    IndexedSampledTextureBinding detail_binding{};
    // Optional txNormalDetail/txDetailNM sampled image and sampler at set 0/bindings 10 and 11.
    IndexedSampledTextureBinding normal_detail_binding{};
    // Optional txDamage sampled image and sampler at set 0/bindings 12 and 13.
    IndexedSampledTextureBinding damage_binding{};
    // Optional txDamageMask sampled image and sampler at set 0/bindings 14 and 15.
    IndexedSampledTextureBinding damage_mask_binding{};
    IndexedMaterialBufferBinding material_binding{};
    IndexedFrameBufferBinding frame_binding{};
    IndexedDirectionalShadowBinding directional_shadow_binding{};
    const StockKsPerPixelNativeDrawBinding* stock_ks_per_pixel_native = nullptr;
    const StockKsPerPixelVulkanAbiProbeDrawBinding*
        stock_ks_per_pixel_vulkan_abi_probe = nullptr;
};

// A recovered selected-mesh draw reuses one static 44-byte-stride mesh. The
// draw is inserted at an explicit position in the ordered scene packet span.
// Its flat color is a fragment-stage constant, not a material resource.
struct SelectedMeshDrawRequest {
    // Insert before this element of IndexedStaticMeshBatchDescription::draws.
    // max() is the append sentinel and normalizes to draws.size().
    std::uint32_t scene_position = std::numeric_limits<std::uint32_t>::max();
    const DrawPacket* packet = nullptr;
    const PipelineProgram* pipeline = nullptr;
    const Buffer* vertex_buffer = nullptr;
    const Buffer* index_buffer = nullptr;
    StaticMeshIndexType index_type = StaticMeshIndexType::uint16;
    const Buffer* color_buffer = nullptr;
    std::uint64_t color_offset_bytes = 0U;
    std::uint32_t color_range_bytes = 0U;
    DrawMatrices matrices{};
};

inline constexpr std::size_t max_selected_mesh_draws = 1U;

enum class IndexedStaticMeshDrawStatus {
    ready,
    invalid_request,
    unsupported,
    execution_failed,
};

struct IndexedStaticMeshDrawResult {
    IndexedStaticMeshDrawStatus status = IndexedStaticMeshDrawStatus::unsupported;
    Diagnostic diagnostic;
    std::vector<std::byte> rgba8;

    [[nodiscard]] bool ok() const noexcept { return status == IndexedStaticMeshDrawStatus::ready; }
};

// A bounded ordered group of indexed draws. The batch owns the initial color
// load/clear and optional depth clear; individual draw requests must leave
// their attachment and load/clear fields at their defaults. The first draw
// uses load_color/clear_depth, and later draws load both persistent targets.
inline constexpr std::size_t max_indexed_static_mesh_batch_draws = 4096U;
// The first native D3D12 stock batch allocates two shader-visible sampler
// descriptors per draw. The D3D12 sampler-heap limit therefore bounds this
// native-only subset to 1,024 draws.
inline constexpr std::size_t max_stock_ks_per_pixel_native_batch_draws = 1024U;

struct IndexedStaticMeshBatchDescription {
    std::span<const IndexedStaticMeshDrawRequest> draws{};
    DepthAttachment* depth_attachment = nullptr;
    bool load_color = false;
    std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    bool clear_depth = false;
    float depth_clear_value = 1.0F;
    // Optional single-sample destination for a four-sample color target.
    // The pointer is non-owning and remains valid through this synchronous
    // call. A backend resolves it in the same submission as the batch.
    Texture* resolve_target = nullptr;
    // Disable CPU readback when the caller only needs the retained GPU image.
    // A four-sample batch then requires resolve_target.
    bool capture_rgba8 = true;
    // Resource-free line draws merge into the indexed scene sequence by each
    // request's scene_position. A selected draw at the same position executes
    // first. All line draws execute before the optional MSAA resolve.
    std::span<const OverlayLineDrawRequest> overlay_draws{};
    // Selected draws merge into the indexed scene sequence by scene_position.
    // A selected draw executes before line draws at the same position.
    std::span<const SelectedMeshDrawRequest> selected_mesh_draws{};
};

enum class IndexedStaticMeshBatchStatus {
    ready,
    invalid_request,
    unsupported,
    execution_failed,
};

struct IndexedStaticMeshBatchResult {
    IndexedStaticMeshBatchStatus status = IndexedStaticMeshBatchStatus::unsupported;
    Diagnostic diagnostic;
    // One pass produces one final color readback. Draw order is observable in
    // this image, rather than represented by synthetic per-draw results.
    std::vector<std::byte> rgba8;

    [[nodiscard]] bool ok() const noexcept {
        return status == IndexedStaticMeshBatchStatus::ready;
    }
};

// A depth-only indexed draw is the backend-neutral directional-shadow seam.
// It writes only the supplied persistent D32 attachment. Vertex/index ranges
// come from the validated packet and remain bounded before backend execution.
struct DepthOnlyIndexedStaticMeshDrawRequest {
    const DrawPacket* packet = nullptr;
    const PipelineProgram* pipeline = nullptr;
    const Buffer* vertex_buffer = nullptr;
    const Buffer* index_buffer = nullptr;
    StaticMeshIndexType index_type = StaticMeshIndexType::uint16;
    std::optional<CameraFrame> camera_frame;
    bool clear_depth = false;
    float depth_clear_value = 1.0F;
    // The opaque mode preserves the resource-free vertex-only contract. The
    // alpha-tested mode requires caller shader authority for t0/s3/b4 and a
    // vertex-plus-fragment pipeline. It does not infer stock shader modules.
    enum class MaterialMode : std::uint8_t {
        opaque,
        stock_alpha_tested,
    } material_mode = MaterialMode::opaque;
    IndexedResourceAuthority resource_authority =
        IndexedResourceAuthority::packet_contract;
    IndexedSampledTextureBinding alpha_tested_diffuse_binding{};
    StockShadowCasterMaterialBinding alpha_tested_material_binding{};
};

enum class DepthOnlyIndexedStaticMeshDrawStatus : std::uint8_t {
    ready,
    invalid_request,
    unsupported,
    execution_failed,
};

// Validate the executable shader, resource, target, and vertex-stream shape
// before a depth-only request owns concrete geometry or backend objects.
enum class DepthOnlyIndexedPipelineRole : std::uint8_t {
    opaque_static,
    stock_alpha_tested_static,
    skinned,
};

struct DepthOnlyIndexedStaticMeshDrawResult {
    DepthOnlyIndexedStaticMeshDrawStatus status =
        DepthOnlyIndexedStaticMeshDrawStatus::unsupported;
    Diagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return status == DepthOnlyIndexedStaticMeshDrawStatus::ready;
    }
};

struct DepthOnlyIndexedStaticMeshBatchDescription {
    std::span<const DepthOnlyIndexedStaticMeshDrawRequest> draws{};
    DepthAttachment* depth_attachment = nullptr;
    bool clear_depth = false;
    float depth_clear_value = 1.0F;
};

enum class DepthOnlyIndexedStaticMeshBatchStatus : std::uint8_t {
    ready,
    invalid_request,
    unsupported,
    execution_failed,
};

struct DepthOnlyIndexedStaticMeshBatchResult {
    DepthOnlyIndexedStaticMeshBatchStatus status =
        DepthOnlyIndexedStaticMeshBatchStatus::unsupported;
    Diagnostic diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return status == DepthOnlyIndexedStaticMeshBatchStatus::ready;
    }
};

inline constexpr std::uint64_t max_texture_readback_bytes = 256ULL * 1024ULL * 1024ULL;

enum class SamplerFilter : std::uint8_t {
    nearest,
    linear,
    anisotropic,
};

enum class SamplerAddressMode : std::uint8_t {
    repeat,
    mirrored_repeat,
    clamp_to_edge,
    clamp_to_border,
};

enum class SamplerCompare : std::uint8_t {
    disabled,
    less,
    less_equal,
    greater,
    greater_equal,
    equal,
    not_equal,
    always,
    never,
};

struct SamplerDescription {
    SamplerFilter min_filter = SamplerFilter::linear;
    SamplerFilter mag_filter = SamplerFilter::linear;
    SamplerFilter mip_filter = SamplerFilter::linear;
    SamplerAddressMode address_u = SamplerAddressMode::repeat;
    SamplerAddressMode address_v = SamplerAddressMode::repeat;
    SamplerAddressMode address_w = SamplerAddressMode::repeat;
    SamplerCompare compare = SamplerCompare::disabled;
    float mip_lod_bias = 0.0F;
    float max_anisotropy = 1.0F;
    float min_lod = 0.0F;
    float max_lod = 1000.0F;
};

enum class SamplerStatus {
    ready,
    invalid_description,
    unsupported,
    allocation_failed,
};

struct SamplerInfo {
    SamplerDescription description{};
};

class Sampler {
public:
    virtual ~Sampler() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const SamplerInfo& info() const noexcept = 0;
};

struct SamplerResult {
    SamplerStatus status = SamplerStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<Sampler> sampler;

    [[nodiscard]] bool ok() const noexcept {
        return status == SamplerStatus::ready && sampler != nullptr;
    }
};

enum class ShaderStage : std::uint8_t {
    vertex,
    fragment,
    compute,
};

enum class ShaderBytecodeFormat : std::uint8_t {
    spirv,
    // Both Direct3D formats use a DXBC container. SHDR/SHEX carries legacy
    // DXBC programs, while a DXIL chunk carries LLVM-based DXIL programs.
    dxbc,
    dxil,
};

struct ShaderModuleDescription {
    ShaderStage stage = ShaderStage::vertex;
    // Serialized shader/container words must be little-endian and four-byte
    // aligned. Backends perform bounded structural validation before use.
    std::span<const std::byte> bytecode{};
};

struct ShaderModuleInfo {
    ShaderStage stage = ShaderStage::vertex;
    ShaderBytecodeFormat format = ShaderBytecodeFormat::spirv;
    std::size_t size_bytes = 0;
};

enum class ShaderModuleStatus {
    ready,
    invalid_description,
    unsupported,
    allocation_failed,
};

class ShaderModule {
public:
    virtual ~ShaderModule() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const ShaderModuleInfo& info() const noexcept = 0;
};

struct ShaderModuleResult {
    ShaderModuleStatus status = ShaderModuleStatus::unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<ShaderModule> shader_module;

    [[nodiscard]] bool ok() const noexcept {
        return status == ShaderModuleStatus::ready && shader_module != nullptr;
    }
};

enum class StockKsPerPixelNativeShaderStatus : std::uint8_t {
    ready,
    invalid_program,
    backend_unsupported,
    vertex_shader_failed,
    pixel_shader_failed,
    invalid_shader_module,
    allocation_failed,
};

struct StockKsPerPixelNativeShaderResult;

class StockKsPerPixelNativeShaderProgram {
public:
    StockKsPerPixelNativeShaderProgram(
        StockKsPerPixelNativeShaderProgram&&) noexcept = default;
    StockKsPerPixelNativeShaderProgram& operator=(
        StockKsPerPixelNativeShaderProgram&&) noexcept = default;
    StockKsPerPixelNativeShaderProgram(
        const StockKsPerPixelNativeShaderProgram&) = delete;
    StockKsPerPixelNativeShaderProgram& operator=(
        const StockKsPerPixelNativeShaderProgram&) = delete;

    [[nodiscard]] const ValidatedStockKsPerPixelNativeProgram& source()
        const noexcept {
        return source_;
    }
    [[nodiscard]] const Device* device() const noexcept { return device_; }
    [[nodiscard]] const ShaderModule& vertex_shader() const noexcept {
        return *vertex_shader_;
    }
    [[nodiscard]] const ShaderModule& pixel_shader() const noexcept {
        return *pixel_shader_;
    }
    [[nodiscard]] bool ready() const noexcept {
        return vertex_shader_ != nullptr && pixel_shader_ != nullptr;
    }

private:
    friend struct StockKsPerPixelNativeShaderResult;
    friend StockKsPerPixelNativeShaderResult
    allocate_stock_ks_per_pixel_native_shaders(
        Device& device,
        ValidatedStockKsPerPixelNativeProgram&& program);

    StockKsPerPixelNativeShaderProgram(
        Device& device,
        ValidatedStockKsPerPixelNativeProgram source,
        std::unique_ptr<ShaderModule> vertex_shader,
        std::unique_ptr<ShaderModule> pixel_shader) noexcept
        : device_(&device),
          source_(std::move(source)),
          vertex_shader_(std::move(vertex_shader)),
          pixel_shader_(std::move(pixel_shader)) {}

    const Device* device_ = nullptr;
    ValidatedStockKsPerPixelNativeProgram source_;
    std::unique_ptr<ShaderModule> vertex_shader_;
    std::unique_ptr<ShaderModule> pixel_shader_;
};

struct StockKsPerPixelNativeShaderResult {
    StockKsPerPixelNativeShaderStatus status =
        StockKsPerPixelNativeShaderStatus::backend_unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<StockKsPerPixelNativeShaderProgram> program;

    [[nodiscard]] bool ok() const noexcept {
        return status == StockKsPerPixelNativeShaderStatus::ready &&
               program != nullptr;
    }
};

inline constexpr std::uint32_t
    stock_ks_per_pixel_native_constant_buffer_view_bytes = 256U;

enum class StockKsPerPixelNativeConstantSlot : std::uint8_t {
    camera,
    object,
    lighting,
    shadow_maps,
    material,
    count,
};

struct StockKsPerPixelNativeConstantData {
    StockKsPerPixelCameraConstants camera{};
    StockKsPerPixelObjectConstants object{};
    StockKsPerPixelLightingConstants lighting{};
    StockDirectionalShadowReceiverConstants shadow_maps{};
    StockKsPerPixelMaterialConstants material{};
};

enum class StockKsPerPixelNativeConstantBufferStatus : std::uint8_t {
    ready,
    invalid_constants,
    backend_unsupported,
    buffer_failed,
    invalid_buffer,
    allocation_failed,
};

struct StockKsPerPixelNativeConstantBufferResult;

class StockKsPerPixelNativeConstantBuffers {
public:
    StockKsPerPixelNativeConstantBuffers(
        StockKsPerPixelNativeConstantBuffers&&) noexcept = default;
    StockKsPerPixelNativeConstantBuffers& operator=(
        StockKsPerPixelNativeConstantBuffers&&) noexcept = default;
    StockKsPerPixelNativeConstantBuffers(
        const StockKsPerPixelNativeConstantBuffers&) = delete;
    StockKsPerPixelNativeConstantBuffers& operator=(
        const StockKsPerPixelNativeConstantBuffers&) = delete;

    [[nodiscard]] Buffer* buffer(
        StockKsPerPixelNativeConstantSlot slot) noexcept {
        const std::size_t index = static_cast<std::size_t>(slot);
        return index < buffers_.size() ? buffers_[index].get() : nullptr;
    }
    [[nodiscard]] const Buffer* buffer(
        StockKsPerPixelNativeConstantSlot slot) const noexcept {
        const std::size_t index = static_cast<std::size_t>(slot);
        return index < buffers_.size() ? buffers_[index].get() : nullptr;
    }
    [[nodiscard]] bool ready() const noexcept {
        for (const auto& buffer : buffers_)
            if (buffer == nullptr) return false;
        return true;
    }
    [[nodiscard]] const Device* device() const noexcept { return device_; }

private:
    friend StockKsPerPixelNativeConstantBufferResult
    allocate_stock_ks_per_pixel_native_constant_buffers(
        Device& device,
        const StockKsPerPixelNativeConstantData& constants);

    explicit StockKsPerPixelNativeConstantBuffers(
        Device& device,
        std::array<std::unique_ptr<Buffer>,
                   static_cast<std::size_t>(
                       StockKsPerPixelNativeConstantSlot::count)> buffers)
        noexcept
        : device_(&device), buffers_(std::move(buffers)) {}

    const Device* device_ = nullptr;
    std::array<std::unique_ptr<Buffer>,
               static_cast<std::size_t>(
                   StockKsPerPixelNativeConstantSlot::count)> buffers_;
};

struct StockKsPerPixelNativeConstantBufferResult {
    StockKsPerPixelNativeConstantBufferStatus status =
        StockKsPerPixelNativeConstantBufferStatus::backend_unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<StockKsPerPixelNativeConstantBuffers> buffers;

    [[nodiscard]] bool ok() const noexcept {
        return status == StockKsPerPixelNativeConstantBufferStatus::ready &&
               buffers != nullptr;
    }
};

struct StockKsPerPixelNativeSamplerSettings {
    float max_anisotropy = 16.0F;
    float mip_lod_bias = 0.0F;
};

enum class StockKsPerPixelNativeSamplerSlot : std::uint8_t {
    linear,
    shadow,
    count,
};

enum class StockKsPerPixelNativeSamplerStatus : std::uint8_t {
    ready,
    invalid_settings,
    backend_unsupported,
    linear_sampler_failed,
    shadow_sampler_failed,
    invalid_sampler,
    allocation_failed,
};

struct StockKsPerPixelNativeSamplerResult;

class StockKsPerPixelNativeSamplers {
public:
    StockKsPerPixelNativeSamplers(
        StockKsPerPixelNativeSamplers&&) noexcept = default;
    StockKsPerPixelNativeSamplers& operator=(
        StockKsPerPixelNativeSamplers&&) noexcept = default;
    StockKsPerPixelNativeSamplers(
        const StockKsPerPixelNativeSamplers&) = delete;
    StockKsPerPixelNativeSamplers& operator=(
        const StockKsPerPixelNativeSamplers&) = delete;

    [[nodiscard]] const Sampler* sampler(
        StockKsPerPixelNativeSamplerSlot slot) const noexcept {
        const std::size_t index = static_cast<std::size_t>(slot);
        return index < samplers_.size() ? samplers_[index].get() : nullptr;
    }
    [[nodiscard]] bool ready() const noexcept {
        for (const auto& sampler : samplers_)
            if (sampler == nullptr) return false;
        return true;
    }
    [[nodiscard]] const Device* device() const noexcept { return device_; }

private:
    friend StockKsPerPixelNativeSamplerResult
    allocate_stock_ks_per_pixel_native_samplers(
        Device& device,
        const StockKsPerPixelNativeSamplerSettings& settings);

    explicit StockKsPerPixelNativeSamplers(
        Device& device,
        std::array<std::unique_ptr<Sampler>,
                   static_cast<std::size_t>(
                       StockKsPerPixelNativeSamplerSlot::count)> samplers)
        noexcept
        : device_(&device), samplers_(std::move(samplers)) {}

    const Device* device_ = nullptr;
    std::array<std::unique_ptr<Sampler>,
               static_cast<std::size_t>(
                   StockKsPerPixelNativeSamplerSlot::count)> samplers_;
};

struct StockKsPerPixelNativeSamplerResult {
    StockKsPerPixelNativeSamplerStatus status =
        StockKsPerPixelNativeSamplerStatus::invalid_settings;
    Diagnostic diagnostic;
    std::unique_ptr<StockKsPerPixelNativeSamplers> samplers;

    [[nodiscard]] bool ok() const noexcept {
        return status == StockKsPerPixelNativeSamplerStatus::ready &&
               samplers != nullptr;
    }
};

enum class StockKsPerPixelNativeDrawResourcesStatus : std::uint8_t {
    ready,
    shader_missing,
    constants_missing,
    samplers_missing,
    shader_invalid,
    constants_invalid,
    samplers_invalid,
    device_mismatch,
    backend_unsupported,
    allocation_failed,
};

struct StockKsPerPixelNativeDrawResourcesResult;

// Owns the native shader, constant-buffer, and sampler groups used by one or
// more synchronous stock ksPerPixel draws. Diffuse and shadow handles remain
// borrowed through StockKsPerPixelNativeDrawBinding.
class StockKsPerPixelNativeDrawResources {
public:
    StockKsPerPixelNativeDrawResources(
        StockKsPerPixelNativeDrawResources&&) noexcept = delete;
    StockKsPerPixelNativeDrawResources& operator=(
        StockKsPerPixelNativeDrawResources&&) noexcept = delete;
    StockKsPerPixelNativeDrawResources(
        const StockKsPerPixelNativeDrawResources&) = delete;
    StockKsPerPixelNativeDrawResources& operator=(
        const StockKsPerPixelNativeDrawResources&) = delete;
    ~StockKsPerPixelNativeDrawResources() = default;

    [[nodiscard]] bool ready() const noexcept {
        return shader_program_ != nullptr && shader_program_->ready() &&
               constant_buffers_ != nullptr && constant_buffers_->ready() &&
               samplers_ != nullptr && samplers_->ready();
    }
    [[nodiscard]] Backend backend() const noexcept { return Backend::D3D12; }
    [[nodiscard]] const Device* device() const noexcept { return device_; }
    [[nodiscard]] const StockKsPerPixelNativeShaderProgram& shader_program()
        const noexcept {
        return *shader_program_;
    }
    [[nodiscard]] const StockKsPerPixelNativeConstantBuffers& constant_buffers()
        const noexcept {
        return *constant_buffers_;
    }
    [[nodiscard]] const StockKsPerPixelNativeSamplers& samplers() const noexcept {
        return *samplers_;
    }
    [[nodiscard]] StockKsPerPixelNativeDrawBinding bind(
        const Texture& diffuse,
        std::array<const DepthAttachment*, 3U> shadow_maps) const noexcept {
        return {this, &diffuse, shadow_maps};
    }

private:
    friend struct StockKsPerPixelNativeDrawResourcesResult;
    friend StockKsPerPixelNativeDrawResourcesResult
    make_stock_ks_per_pixel_native_draw_resources(
        Device&,
        std::unique_ptr<StockKsPerPixelNativeShaderProgram>,
        std::unique_ptr<StockKsPerPixelNativeConstantBuffers>,
        std::unique_ptr<StockKsPerPixelNativeSamplers>);

    StockKsPerPixelNativeDrawResources(
        Device& device,
        std::unique_ptr<StockKsPerPixelNativeShaderProgram> shader_program,
        std::unique_ptr<StockKsPerPixelNativeConstantBuffers> constant_buffers,
        std::unique_ptr<StockKsPerPixelNativeSamplers> samplers) noexcept
        : device_(&device),
          shader_program_(std::move(shader_program)),
          constant_buffers_(std::move(constant_buffers)),
          samplers_(std::move(samplers)) {}

    const Device* device_ = nullptr;
    std::unique_ptr<StockKsPerPixelNativeShaderProgram> shader_program_;
    std::unique_ptr<StockKsPerPixelNativeConstantBuffers> constant_buffers_;
    std::unique_ptr<StockKsPerPixelNativeSamplers> samplers_;
};

struct StockKsPerPixelNativeDrawResourcesResult {
    StockKsPerPixelNativeDrawResourcesStatus status =
        StockKsPerPixelNativeDrawResourcesStatus::backend_unsupported;
    Diagnostic diagnostic;
    std::unique_ptr<StockKsPerPixelNativeDrawResources> resources;

    [[nodiscard]] bool ok() const noexcept {
        return status == StockKsPerPixelNativeDrawResourcesStatus::ready &&
               resources != nullptr;
    }
};

inline constexpr std::size_t max_shader_module_bytes = 16U * 1024U * 1024U;

[[nodiscard]] BufferStatus validate_buffer_description(
    const BufferDescription& description,
    std::size_t initial_data_size,
    Diagnostic& diagnostic);

[[nodiscard]] BufferStatus validate_buffer_update(
    const Buffer& buffer,
    std::uint64_t offset,
    std::size_t data_size,
    Diagnostic& diagnostic);

[[nodiscard]] TextureStatus validate_texture_description(
    const TextureDescription& description,
    const TextureUploadPlan& initial_uploads,
    Diagnostic& diagnostic);

[[nodiscard]] TextureStatus validate_texture_upload_plan(
    const TextureDescription& description,
    const TextureUploadPlan& uploads,
    Diagnostic& diagnostic);

[[nodiscard]] TextureStatus validate_texture_update(
    const Texture& texture,
    const TextureUploadPlan& uploads,
    Diagnostic& diagnostic);

[[nodiscard]] DepthAttachmentStatus validate_depth_attachment_description(
    const DepthAttachmentDescription& description,
    Diagnostic& diagnostic);

[[nodiscard]] DepthAttachmentReadbackStatus validate_depth_attachment_readback(
    const DepthAttachment& attachment,
    const DepthAttachmentReadbackRequest& request,
    Diagnostic& diagnostic);

[[nodiscard]] TextureReadbackStatus validate_texture_clear_readback(
    const Texture& texture,
    const TextureClearReadbackRequest& request,
    Diagnostic& diagnostic);

[[nodiscard]] TriangleDrawStatus validate_triangle_draw_request(
    const Texture& texture, const TriangleDrawRequest& request, Diagnostic& diagnostic);

[[nodiscard]] IndexedStaticMeshDrawStatus validate_indexed_static_mesh_draw_request(
    const Texture& texture, const IndexedStaticMeshDrawRequest& request, Diagnostic& diagnostic);

[[nodiscard]] IndexedStaticMeshBatchStatus validate_overlay_line_draw_request(
    const Texture& texture, const OverlayLineDrawRequest& request,
    Diagnostic& diagnostic);

[[nodiscard]] IndexedStaticMeshBatchStatus validate_selected_mesh_draw_request(
    const Texture& texture, const SelectedMeshDrawRequest& request,
    Diagnostic& diagnostic);

// Preflight an ordered batch without changing any caller-owned request. The
// function applies the batch attachment/load/clear state to local copies and
// validates every draw before a backend is allowed to execute any of them.
[[nodiscard]] IndexedStaticMeshBatchStatus validate_indexed_static_mesh_batch_description(
    const Texture& texture, const IndexedStaticMeshBatchDescription& description,
    Diagnostic& diagnostic);

[[nodiscard]] DepthOnlyIndexedStaticMeshDrawStatus
validate_depth_only_indexed_static_mesh_draw_request(
    const DepthAttachment& depth,
    const DepthOnlyIndexedStaticMeshDrawRequest& request,
    Diagnostic& diagnostic);

[[nodiscard]] bool validate_depth_only_indexed_pipeline_contract(
    const PipelineProgram& pipeline, DepthOnlyIndexedPipelineRole role,
    Diagnostic& diagnostic);

[[nodiscard]] DepthOnlyIndexedStaticMeshBatchStatus
validate_depth_only_indexed_static_mesh_batch_description(
    const DepthOnlyIndexedStaticMeshBatchDescription& description,
    Diagnostic& diagnostic);

[[nodiscard]] SamplerStatus validate_sampler_description(
    const SamplerDescription& description,
    Diagnostic& diagnostic);

[[nodiscard]] ShaderModuleStatus validate_shader_module_description(
    const ShaderModuleDescription& description,
    Diagnostic& diagnostic);

[[nodiscard]] bool shader_bytecode_format(
    std::span<const std::byte> bytecode,
    ShaderBytecodeFormat& format) noexcept;

[[nodiscard]] PresentationTargetStatus validate_presentation_target_description(
    const PresentationTargetDescription& description,
    Diagnostic& diagnostic);

struct AdapterResult {
    DeviceStatus status = DeviceStatus::unavailable;
    Diagnostic diagnostic;
    std::vector<DeviceInfo> adapters;

    [[nodiscard]] bool ok() const noexcept { return status == DeviceStatus::ready; }
};

class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual const DeviceInfo& info() const noexcept = 0;

    // The default preserves discovery-only and fake devices. Backends may
    // report prerequisites without exposing or creating native handles.
    [[nodiscard]] virtual PresentationCapabilities presentation_capabilities()
        const noexcept {
        return {};
    }

    // The target and its native synchronization objects remain private to
    // each backend. The default preserves source compatibility for D3D12,
    // fake devices, and discovery-only devices until they opt in.
    [[nodiscard]] virtual PresentationTargetResult create_presentation_target(
        const PresentationTargetDescription& description) {
        Diagnostic diagnostic;
        const PresentationTargetStatus validation =
            validate_presentation_target_description(description, diagnostic);
        if (validation != PresentationTargetStatus::ready)
            return {validation, std::move(diagnostic), nullptr};
        return {PresentationTargetStatus::unsupported,
                {"presentation_target_unsupported",
                 "This backend has not enabled presentation targets"},
                nullptr};
    }

    [[nodiscard]] virtual PresentationFrameResult clear_and_present(
        PresentationTarget&, const std::array<float, 4>&) {
        return {PresentationFrameStatus::unsupported,
                {"presentation_execution_unsupported",
                 "This backend has not enabled presentation execution"}};
    }

    // Presents a completed, same-format offscreen color attachment without
    // exposing backend image or swapchain handles. Backends that implement
    // this seam retain ownership and synchronization validation.
    [[nodiscard]] virtual PresentationFrameResult present_texture(
        PresentationTarget&, Texture&) {
        return {PresentationFrameStatus::unsupported,
                {"presentation_texture_unsupported",
                 "This backend has not enabled rendered-texture presentation"}};
    }

    // Implementations copy initial_data before this call returns. Buffer
    // lifetimes may extend beyond this Device object: backend handles
    // retain their private shared context. Public callers only see this
    // backend-neutral RAII contract, never Vk* or ID3D12* types.
    [[nodiscard]] virtual BufferResult create_buffer(
        const BufferDescription& description,
        std::span<const std::byte> initial_data = {}) = 0;

    [[nodiscard]] virtual BufferUpdateResult update_buffer(
        Buffer& buffer,
        std::uint64_t offset,
        std::span<const std::byte> data) = 0;

    [[nodiscard]] virtual TextureResult create_texture(
        const TextureDescription& description,
        const TextureUploadPlan& initial_uploads = {}) = 0;

    [[nodiscard]] virtual TextureUpdateResult update_texture(
        Texture& texture,
        const TextureUploadPlan& uploads) = 0;

    // Backends opt into persistent D32 attachments independently. Keeping a
    // default implementation preserves discovery-only and fake devices.
    [[nodiscard]] virtual DepthAttachmentResult create_depth_attachment(
        const DepthAttachmentDescription&) {
        return {DepthAttachmentStatus::unsupported,
                {"depth_attachment_execution_unsupported",
                 "This backend has not enabled persistent depth attachments"},
                nullptr};
    }

    [[nodiscard]] virtual DepthAttachmentReadbackResult read_depth_attachment(
        DepthAttachment& attachment,
        const DepthAttachmentReadbackRequest& request) {
        Diagnostic diagnostic;
        const DepthAttachmentReadbackStatus validation =
            validate_depth_attachment_readback(attachment, request, diagnostic);
        if (validation != DepthAttachmentReadbackStatus::ready)
            return {validation, std::move(diagnostic), {}};
        return {DepthAttachmentReadbackStatus::unsupported,
                {"depth_attachment_readback_unsupported",
                 "This backend has not enabled D32 depth readback"},
                {}};
    }

    [[nodiscard]] virtual TextureClearReadbackResult clear_texture_and_readback(
        Texture& texture,
        const TextureClearReadbackRequest& request) = 0;

    [[nodiscard]] virtual TriangleDrawResult draw_triangle_and_readback(
        Texture& texture, const TriangleDrawRequest& request) = 0;

    // Backends opt into indexed execution independently. Keeping a default
    // implementation preserves headless and discovery-only devices while the
    // neutral contract is adopted by each backend.
    [[nodiscard]] virtual IndexedStaticMeshDrawResult draw_indexed_static_mesh_and_readback(
        Texture&, const IndexedStaticMeshDrawRequest&) {
        return {IndexedStaticMeshDrawStatus::unsupported,
                {"indexed_static_mesh_execution_unsupported",
                 "This backend has not enabled indexed static-mesh execution"},
                {}};
    }

    // Backends may execute the validated ordered batch as one pass. The
    // default validates first, so an invalid later draw cannot partially
    // execute on a discovery-only or neutral device.
    [[nodiscard]] virtual IndexedStaticMeshBatchResult
    draw_indexed_static_mesh_batch_and_readback(
        Texture& texture, const IndexedStaticMeshBatchDescription& description) {
        Diagnostic diagnostic;
        const IndexedStaticMeshBatchStatus validation =
            validate_indexed_static_mesh_batch_description(texture, description, diagnostic);
        if (validation != IndexedStaticMeshBatchStatus::ready)
            return {validation, std::move(diagnostic), {}};
        return {IndexedStaticMeshBatchStatus::unsupported,
                {"indexed_static_mesh_batch_execution_unsupported",
                 "This backend has not enabled ordered indexed static-mesh batch execution"},
                {}};
    }

    // Backends opt into depth-only indexed execution independently. The
    // neutral default validates the complete request and performs no work.
    [[nodiscard]] virtual DepthOnlyIndexedStaticMeshDrawResult
    draw_depth_only_indexed_static_mesh(
        DepthAttachment& depth, const DepthOnlyIndexedStaticMeshDrawRequest& request) {
        Diagnostic diagnostic;
        const DepthOnlyIndexedStaticMeshDrawStatus validation =
            validate_depth_only_indexed_static_mesh_draw_request(depth, request, diagnostic);
        if (validation != DepthOnlyIndexedStaticMeshDrawStatus::ready)
            return {validation, std::move(diagnostic)};
        return {DepthOnlyIndexedStaticMeshDrawStatus::unsupported,
                {"depth_only_indexed_static_mesh_execution_unsupported",
                 "This backend has not enabled depth-only indexed static-mesh execution"}};
    }

    [[nodiscard]] virtual DepthOnlyIndexedStaticMeshBatchResult
    draw_depth_only_indexed_static_mesh_batch(
        const DepthOnlyIndexedStaticMeshBatchDescription& description) {
        Diagnostic diagnostic;
        const DepthOnlyIndexedStaticMeshBatchStatus validation =
            validate_depth_only_indexed_static_mesh_batch_description(description, diagnostic);
        if (validation != DepthOnlyIndexedStaticMeshBatchStatus::ready)
            return {validation, std::move(diagnostic)};
        return {DepthOnlyIndexedStaticMeshBatchStatus::unsupported,
                {"depth_only_indexed_static_mesh_batch_execution_unsupported",
                 "This backend has not enabled depth-only indexed static-mesh batch execution"}};
    }

    [[nodiscard]] virtual SamplerResult create_sampler(
        const SamplerDescription& description) = 0;

    [[nodiscard]] virtual ShaderModuleResult create_shader_module(
        const ShaderModuleDescription& description) = 0;

    virtual void wait_idle() noexcept = 0;
};

// Allocate the two validated native DXBC stages on D3D12. Vulkan rejects this
// request before a device call because Vulkan requires source-equivalent
// SPIR-V. A successful result owns both the source package and shader objects.
[[nodiscard]] StockKsPerPixelNativeShaderResult
allocate_stock_ks_per_pixel_native_shaders(
    Device& device,
    ValidatedStockKsPerPixelNativeProgram&& program);

// Create five separate mutable views for native b0 through b4. Each view is
// 256 bytes for D3D12 CBV alignment. Bytes after each reflected record are
// zero. A failed allocation returns no partial owner.
[[nodiscard]] StockKsPerPixelNativeConstantBufferResult
allocate_stock_ks_per_pixel_native_constant_buffers(
    Device& device,
    const StockKsPerPixelNativeConstantData& constants);

// Create the recovered s0 anisotropic sampler and s1 comparison sampler.
// The two runtime values match the native editor controls. A failed
// allocation returns no partial owner.
[[nodiscard]] StockKsPerPixelNativeSamplerResult
allocate_stock_ks_per_pixel_native_samplers(
    Device& device,
    const StockKsPerPixelNativeSamplerSettings& settings = {});

// Adopt three already allocated native owners into one stable bundle. The
// factory consumes the supplied owners on every return (releasing them on
// failure) and rejects incomplete or non-D3D12 metadata before any draw can
// observe the bundle.
[[nodiscard]] StockKsPerPixelNativeDrawResourcesResult
make_stock_ks_per_pixel_native_draw_resources(
    Device& device,
    std::unique_ptr<StockKsPerPixelNativeShaderProgram> shader_program,
    std::unique_ptr<StockKsPerPixelNativeConstantBuffers> constant_buffers,
    std::unique_ptr<StockKsPerPixelNativeSamplers> samplers);

struct DeviceResult {
    DeviceStatus status = DeviceStatus::unavailable;
    Diagnostic diagnostic;
    std::unique_ptr<Device> device;

    [[nodiscard]] bool ok() const noexcept {
        return status == DeviceStatus::ready && device != nullptr;
    }
};

[[nodiscard]] const char* backend_name(Backend backend) noexcept;
[[nodiscard]] const char* device_status_name(DeviceStatus status) noexcept;
[[nodiscard]] const char* buffer_status_name(BufferStatus status) noexcept;
[[nodiscard]] const char* texture_status_name(TextureStatus status) noexcept;
[[nodiscard]] const char* depth_attachment_status_name(DepthAttachmentStatus status) noexcept;
[[nodiscard]] const char* depth_attachment_readback_status_name(
    DepthAttachmentReadbackStatus status) noexcept;
[[nodiscard]] const char* texture_readback_status_name(TextureReadbackStatus status) noexcept;
[[nodiscard]] const char* triangle_draw_status_name(TriangleDrawStatus status) noexcept;
[[nodiscard]] const char* indexed_static_mesh_draw_status_name(
    IndexedStaticMeshDrawStatus status) noexcept;
[[nodiscard]] const char* indexed_static_mesh_batch_status_name(
    IndexedStaticMeshBatchStatus status) noexcept;
[[nodiscard]] const char* depth_only_indexed_static_mesh_draw_status_name(
    DepthOnlyIndexedStaticMeshDrawStatus status) noexcept;
[[nodiscard]] const char* depth_only_indexed_static_mesh_batch_status_name(
    DepthOnlyIndexedStaticMeshBatchStatus status) noexcept;
[[nodiscard]] const char* sampler_status_name(SamplerStatus status) noexcept;
[[nodiscard]] const char* shader_module_status_name(ShaderModuleStatus status) noexcept;
[[nodiscard]] const char* stock_ks_per_pixel_native_shader_status_name(
    StockKsPerPixelNativeShaderStatus status) noexcept;
[[nodiscard]] const char* stock_ks_per_pixel_native_constant_buffer_status_name(
    StockKsPerPixelNativeConstantBufferStatus status) noexcept;
[[nodiscard]] const char* stock_ks_per_pixel_native_sampler_status_name(
    StockKsPerPixelNativeSamplerStatus status) noexcept;
[[nodiscard]] const char* stock_ks_per_pixel_native_draw_resources_status_name(
    StockKsPerPixelNativeDrawResourcesStatus status) noexcept;
[[nodiscard]] const char* presentation_target_status_name(
    PresentationTargetStatus status) noexcept;
[[nodiscard]] const char* presentation_frame_status_name(
    PresentationFrameStatus status) noexcept;

[[nodiscard]] AdapterResult enumerate_adapters(Backend backend,
                                                const DeviceOptions& options = {});

[[nodiscard]] DeviceResult create_device(Backend backend,
                                         const DeviceOptions& options = {});

} // namespace apex::render
