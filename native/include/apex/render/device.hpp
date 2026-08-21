#pragma once

#include "apex/render/camera.hpp"
#include "apex/render/pipeline.hpp"
#include "apex/render/texture_format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace apex::render {

struct DrawPacket;

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
    // A native renderer device is intentionally headless. No window-system
    // extensions or swapchain are requested by either backend.
    bool headless = true;
    bool enable_validation = false;
    bool allow_software = true;
    // Prefer a software adapter when one is available. D3D12 resolves WARP
    // explicitly; Vulkan prioritizes CPU physical devices.
    bool prefer_software = false;
    std::uint32_t adapter_index = 0;
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
};

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
};

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

struct IndexedStaticMeshBatchDescription {
    std::span<const IndexedStaticMeshDrawRequest> draws{};
    DepthAttachment* depth_attachment = nullptr;
    bool load_color = false;
    std::array<float, 4> clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
    bool clear_depth = false;
    float depth_clear_value = 1.0F;
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
    // Direct3D shader bytecode is a DXBC container; DXIL is a supported
    // chunk inside that container, not a standalone top-level signature.
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

[[nodiscard]] TextureReadbackStatus validate_texture_clear_readback(
    const Texture& texture,
    const TextureClearReadbackRequest& request,
    Diagnostic& diagnostic);

[[nodiscard]] TriangleDrawStatus validate_triangle_draw_request(
    const Texture& texture, const TriangleDrawRequest& request, Diagnostic& diagnostic);

[[nodiscard]] IndexedStaticMeshDrawStatus validate_indexed_static_mesh_draw_request(
    const Texture& texture, const IndexedStaticMeshDrawRequest& request, Diagnostic& diagnostic);

// Preflight an ordered batch without changing any caller-owned request. The
// function applies the batch attachment/load/clear state to local copies and
// validates every draw before a backend is allowed to execute any of them.
[[nodiscard]] IndexedStaticMeshBatchStatus validate_indexed_static_mesh_batch_description(
    const Texture& texture, const IndexedStaticMeshBatchDescription& description,
    Diagnostic& diagnostic);

[[nodiscard]] SamplerStatus validate_sampler_description(
    const SamplerDescription& description,
    Diagnostic& diagnostic);

[[nodiscard]] ShaderModuleStatus validate_shader_module_description(
    const ShaderModuleDescription& description,
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

    // Buffer lifetimes may extend beyond this Device object: backend handles
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

    [[nodiscard]] virtual SamplerResult create_sampler(
        const SamplerDescription& description) = 0;

    [[nodiscard]] virtual ShaderModuleResult create_shader_module(
        const ShaderModuleDescription& description) = 0;

    virtual void wait_idle() noexcept = 0;
};

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
[[nodiscard]] const char* texture_readback_status_name(TextureReadbackStatus status) noexcept;
[[nodiscard]] const char* triangle_draw_status_name(TriangleDrawStatus status) noexcept;
[[nodiscard]] const char* indexed_static_mesh_draw_status_name(
    IndexedStaticMeshDrawStatus status) noexcept;
[[nodiscard]] const char* indexed_static_mesh_batch_status_name(
    IndexedStaticMeshBatchStatus status) noexcept;
[[nodiscard]] const char* sampler_status_name(SamplerStatus status) noexcept;
[[nodiscard]] const char* shader_module_status_name(ShaderModuleStatus status) noexcept;

[[nodiscard]] AdapterResult enumerate_adapters(Backend backend,
                                                const DeviceOptions& options = {});

[[nodiscard]] DeviceResult create_device(Backend backend,
                                         const DeviceOptions& options = {});

} // namespace apex::render
