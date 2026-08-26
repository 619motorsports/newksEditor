#include "apex/render/device.hpp"
#include "apex/render/draw_packet.hpp"
#include "apex/render/static_scene.hpp"
#include "apex/render/stock_material_execution.hpp"
#include "../src/render/d3d12_stock_ks_per_pixel_status.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace apex::render;

static_assert(!std::is_copy_constructible_v<
              ValidatedStockKsPerPixelNativeProgram>);
static_assert(!std::is_copy_assignable_v<
              ValidatedStockKsPerPixelNativeProgram>);
static_assert(std::is_nothrow_move_constructible_v<
              ValidatedStockKsPerPixelNativeProgram>);
static_assert(std::is_nothrow_move_assignable_v<
              ValidatedStockKsPerPixelNativeProgram>);
static_assert(!std::is_copy_constructible_v<
              StockKsPerPixelNativeSamplers>);
static_assert(!std::is_copy_assignable_v<
              StockKsPerPixelNativeSamplers>);
static_assert(std::is_nothrow_move_constructible_v<
              StockKsPerPixelNativeSamplers>);
static_assert(std::is_nothrow_move_assignable_v<
              StockKsPerPixelNativeSamplers>);
static_assert(!std::is_copy_constructible_v<
              StockKsPerPixelNativeDrawResources>);
static_assert(!std::is_copy_assignable_v<
              StockKsPerPixelNativeDrawResources>);
static_assert(!std::is_move_constructible_v<
              StockKsPerPixelNativeDrawResources>);
static_assert(!std::is_move_assignable_v<
              StockKsPerPixelNativeDrawResources>);

class FakeConstantBuffer final : public Buffer {
public:
    FakeConstantBuffer(Backend backend, BufferDescription description)
        : backend_(backend), info_{description} {
        ++live_count;
    }
    ~FakeConstantBuffer() override { --live_count; }

    [[nodiscard]] Backend backend() const noexcept override {
        return backend_;
    }
    [[nodiscard]] const BufferInfo& info() const noexcept override {
        return info_;
    }

    static inline std::size_t live_count = 0U;

private:
    Backend backend_ = Backend::D3D12;
    BufferInfo info_{};
};

class FakeShaderModule final : public ShaderModule {
public:
    FakeShaderModule(Backend backend, ShaderModuleInfo info)
        : backend_(backend), info_(info) {
        ++live_count;
    }
    ~FakeShaderModule() override { --live_count; }

    [[nodiscard]] Backend backend() const noexcept override {
        return backend_;
    }
    [[nodiscard]] const ShaderModuleInfo& info() const noexcept override {
        return info_;
    }

    static inline std::size_t live_count = 0U;

private:
    Backend backend_ = Backend::D3D12;
    ShaderModuleInfo info_{};
};

class FakeNativeSampler final : public Sampler {
public:
    FakeNativeSampler(Backend backend, SamplerDescription description)
        : backend_(backend), info_{description} {
        ++live_count;
    }
    ~FakeNativeSampler() override { --live_count; }

    [[nodiscard]] Backend backend() const noexcept override {
        return backend_;
    }
    [[nodiscard]] const SamplerInfo& info() const noexcept override {
        return info_;
    }

    static inline std::size_t live_count = 0U;

private:
    Backend backend_ = Backend::D3D12;
    SamplerInfo info_{};
};

class FakeNativeTexture final : public Texture {
public:
    FakeNativeTexture(Backend backend, TextureDescription description,
                      const Device* owner = nullptr)
        : backend_(backend), info_{description}, owner_(owner) {}
    [[nodiscard]] Backend backend() const noexcept override { return backend_; }
    [[nodiscard]] const TextureInfo& info() const noexcept override { return info_; }
    [[nodiscard]] const Device* owner() const noexcept { return owner_; }
private:
    Backend backend_ = Backend::D3D12;
    TextureInfo info_{};
    const Device* owner_ = nullptr;
};

class FakeNativeDepth final : public DepthAttachment {
public:
    FakeNativeDepth(Backend backend, DepthAttachmentDescription description,
                    const Device* owner = nullptr)
        : backend_(backend), info_{description}, owner_(owner) {}
    [[nodiscard]] Backend backend() const noexcept override { return backend_; }
    [[nodiscard]] const DepthAttachmentInfo& info() const noexcept override { return info_; }
    [[nodiscard]] const Device* owner() const noexcept { return owner_; }
private:
    Backend backend_ = Backend::D3D12;
    DepthAttachmentInfo info_{};
    const Device* owner_ = nullptr;
};

class FakeShaderDevice final : public Device {
public:
    explicit FakeShaderDevice(Backend backend)
        : info_{backend, "fake", "unit", 1U, 0U, 0U, 0U, 0U, true} {}

    [[nodiscard]] const DeviceInfo& info() const noexcept override {
        return info_;
    }
    [[nodiscard]] bool owns_resource(
        const Texture& texture) const noexcept override {
        const auto* native = dynamic_cast<const FakeNativeTexture*>(&texture);
        return native != nullptr && native->owner() == this;
    }
    [[nodiscard]] bool owns_resource(
        const DepthAttachment& attachment) const noexcept override {
        const auto* native =
            dynamic_cast<const FakeNativeDepth*>(&attachment);
        return native != nullptr && native->owner() == this;
    }
    [[nodiscard]] BufferResult create_buffer(
        const BufferDescription& description,
        std::span<const std::byte> initial_data) override {
        ++buffer_calls;
        buffer_descriptions.push_back(description);
        initial_buffers.emplace_back(initial_data.begin(), initial_data.end());
        if (buffer_fail_call != 0U && buffer_calls == buffer_fail_call)
            return {BufferStatus::allocation_failed,
                    {"fake_buffer_failure", "fake buffer allocation failed"},
                    nullptr};
        BufferDescription returned_description = description;
        if (wrong_buffer) returned_description.size_bytes = 128U;
        const Backend returned_backend =
            wrong_buffer ? Backend::Vulkan : info_.backend;
        return {BufferStatus::ready, {},
                std::make_unique<FakeConstantBuffer>(
                    returned_backend, returned_description)};
    }
    [[nodiscard]] BufferUpdateResult update_buffer(
        Buffer&, std::uint64_t,
        std::span<const std::byte> data) override {
        ++buffer_update_calls;
        updated_buffers.emplace_back(data.begin(), data.end());
        return {BufferStatus::ready, {}};
    }
    [[nodiscard]] TextureResult create_texture(
        const TextureDescription& description,
        const TextureUploadPlan&) override {
        return {TextureStatus::ready, {},
                std::make_unique<FakeNativeTexture>(info_.backend,
                                                    description, this)};
    }
    [[nodiscard]] TextureUpdateResult update_texture(
        Texture&, const TextureUploadPlan&) override {
        return {TextureStatus::unsupported, {"unused", "unused"}};
    }
    [[nodiscard]] TextureClearReadbackResult clear_texture_and_readback(
        Texture&, const TextureClearReadbackRequest&) override {
        return {TextureReadbackStatus::unsupported, {"unused", "unused"}, {}};
    }
    [[nodiscard]] TriangleDrawResult draw_triangle_and_readback(
        Texture&, const TriangleDrawRequest&) override {
        return {TriangleDrawStatus::unsupported, {"unused", "unused"}, {}};
    }
    [[nodiscard]] SamplerResult create_sampler(
        const SamplerDescription& description) override {
        ++sampler_calls;
        sampler_descriptions.push_back(description);
        if (sampler_fail_call != 0U && sampler_calls == sampler_fail_call)
            return {SamplerStatus::allocation_failed,
                    {"fake_sampler_failure", "fake sampler allocation failed"},
                    nullptr};
        SamplerDescription returned_description = description;
        if (wrong_sampler) returned_description.compare = SamplerCompare::always;
        const Backend returned_backend =
            wrong_sampler ? Backend::Vulkan : info_.backend;
        return {SamplerStatus::ready, {},
                std::make_unique<FakeNativeSampler>(
                    returned_backend, returned_description)};
    }
    [[nodiscard]] ShaderModuleResult create_shader_module(
        const ShaderModuleDescription& description) override {
        ++shader_calls;
        stages.push_back(description.stage);
        sizes.push_back(description.bytecode.size());
        if (fail_call != 0U && shader_calls == fail_call)
            return {ShaderModuleStatus::allocation_failed,
                    {"fake_shader_failure", "fake shader allocation failed"},
                    nullptr};
        ShaderModuleInfo module_info{
            wrong_stage ? ShaderStage::compute : description.stage,
            wrong_format ? ShaderBytecodeFormat::spirv
                         : ShaderBytecodeFormat::dxbc,
            description.bytecode.size()};
        const Backend module_backend =
            wrong_backend ? Backend::Vulkan : info_.backend;
        return {ShaderModuleStatus::ready, {},
                std::make_unique<FakeShaderModule>(module_backend,
                                                   module_info)};
    }
    [[nodiscard]] IndexedStaticMeshBatchResult
    draw_indexed_static_mesh_batch_and_readback(
        Texture& target,
        const IndexedStaticMeshBatchDescription& batch) override {
        Diagnostic diagnostic;
        const IndexedStaticMeshBatchStatus status =
            validate_indexed_static_mesh_batch_description(target, batch,
                                                           diagnostic);
        if (status != IndexedStaticMeshBatchStatus::ready)
            return {status, std::move(diagnostic), {}};
        ++batch_calls;
        native_draws = 0U;
        native_resource_owners.clear();
        for (const IndexedStaticMeshDrawRequest& draw : batch.draws) {
            if (draw.shader_authority !=
                IndexedShaderAuthority::explicit_stock_ks_per_pixel_native)
                continue;
            ++native_draws;
            native_resource_owners.push_back(
                draw.stock_ks_per_pixel_native->resources);
        }
        return {IndexedStaticMeshBatchStatus::ready, {}, {std::byte{0x7f}}};
    }
    void wait_idle() noexcept override {}

    std::size_t shader_calls = 0U;
    std::size_t fail_call = 0U;
    std::size_t buffer_calls = 0U;
    std::size_t buffer_fail_call = 0U;
    std::size_t sampler_calls = 0U;
    std::size_t sampler_fail_call = 0U;
    std::size_t buffer_update_calls = 0U;
    std::size_t batch_calls = 0U;
    std::size_t native_draws = 0U;
    bool wrong_stage = false;
    bool wrong_format = false;
    bool wrong_backend = false;
    bool wrong_buffer = false;
    bool wrong_sampler = false;
    std::vector<ShaderStage> stages;
    std::vector<std::size_t> sizes;
    std::vector<BufferDescription> buffer_descriptions;
    std::vector<std::vector<std::byte>> initial_buffers;
    std::vector<std::vector<std::byte>> updated_buffers;
    std::vector<SamplerDescription> sampler_descriptions;
    std::vector<const StockKsPerPixelNativeDrawResources*>
        native_resource_owners;

private:
    DeviceInfo info_{};
};

StockKsPerPixelLightingConstants lighting_fixture();

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset,
             std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte)
        bytes[offset + byte] =
            static_cast<std::uint8_t>(value >> (byte * 8U));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    put_u32(bytes, offset, value);
}

std::uint32_t get_u32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

struct RdefResourceFixture {
    const char* name;
    std::uint32_t type;
    std::uint32_t return_type;
    std::uint32_t dimension;
    std::uint32_t samples;
    std::uint32_t bind;
    std::uint32_t flags;
};

struct RdefConstantBufferFixture {
    const char* name;
    std::uint32_t bytes;
};

std::vector<std::uint8_t> rdef_stage_fixture(bool vertex) {
    static constexpr std::array vertex_resources = {
        RdefResourceFixture{"cbCamera", 0U, 0U, 0U, 0U, 0U, 1U},
        RdefResourceFixture{"cbPerObject", 0U, 0U, 0U, 0U, 1U, 1U},
        RdefResourceFixture{"cbLighting", 0U, 0U, 0U, 0U, 2U, 1U},
        RdefResourceFixture{"cbShadowMaps", 0U, 0U, 0U, 0U, 3U, 1U},
    };
    static constexpr std::array pixel_resources = {
        RdefResourceFixture{"samLinear", 3U, 0U, 0U, 0U, 0U, 1U},
        RdefResourceFixture{"samShadow", 3U, 0U, 0U, 0U, 1U, 3U},
        RdefResourceFixture{"txDiffuse", 2U, 5U, 4U, 0xffffffffU, 0U, 13U},
        RdefResourceFixture{"txShadow0", 2U, 5U, 4U, 0xffffffffU, 6U, 1U},
        RdefResourceFixture{"txShadow1", 2U, 5U, 4U, 0xffffffffU, 7U, 1U},
        RdefResourceFixture{"txShadow2", 2U, 5U, 4U, 0xffffffffU, 8U, 1U},
        RdefResourceFixture{"cbLighting", 0U, 0U, 0U, 0U, 2U, 1U},
        RdefResourceFixture{"cbShadowMaps", 0U, 0U, 0U, 0U, 3U, 1U},
        RdefResourceFixture{"cbMaterial", 0U, 0U, 0U, 0U, 4U, 1U},
    };
    static constexpr std::array vertex_buffers = {
        RdefConstantBufferFixture{"cbCamera", 224U},
        RdefConstantBufferFixture{"cbPerObject", 64U},
        RdefConstantBufferFixture{"cbLighting", 160U},
        RdefConstantBufferFixture{"cbShadowMaps", 208U},
    };
    static constexpr std::array pixel_buffers = {
        RdefConstantBufferFixture{"cbLighting", 160U},
        RdefConstantBufferFixture{"cbMaterial", 32U},
        RdefConstantBufferFixture{"cbShadowMaps", 208U},
    };
    const std::span<const RdefResourceFixture> resources =
        vertex ? std::span<const RdefResourceFixture>(vertex_resources)
               : std::span<const RdefResourceFixture>(pixel_resources);
    const std::span<const RdefConstantBufferFixture> buffers =
        vertex ? std::span<const RdefConstantBufferFixture>(vertex_buffers)
               : std::span<const RdefConstantBufferFixture>(pixel_buffers);

    constexpr std::size_t rdef_header_bytes = 28U;
    constexpr std::size_t resource_stride = 32U;
    constexpr std::size_t buffer_stride = 24U;
    const std::size_t resource_offset = rdef_header_bytes;
    const std::size_t buffer_offset =
        resource_offset + resources.size() * resource_stride;
    const std::size_t string_offset =
        buffer_offset + buffers.size() * buffer_stride;
    std::vector<std::uint8_t> rdef(string_offset, 0U);
    put_u32(rdef, 0U, static_cast<std::uint32_t>(buffers.size()));
    put_u32(rdef, 4U, static_cast<std::uint32_t>(buffer_offset));
    put_u32(rdef, 8U, static_cast<std::uint32_t>(resources.size()));
    put_u32(rdef, 12U, static_cast<std::uint32_t>(resource_offset));
    put_u32(rdef, 16U, vertex ? 0xfffe0400U : 0xffff0400U);
    put_u32(rdef, 20U, 0x100U);
    const auto add_name = [&](const char* text) {
        const auto offset = static_cast<std::uint32_t>(rdef.size());
        while (*text != '\0') {
            rdef.push_back(static_cast<std::uint8_t>(*text));
            ++text;
        }
        rdef.push_back(0U);
        return offset;
    };
    for (std::size_t index = 0U; index < resources.size(); ++index) {
        const auto& resource = resources[index];
        const std::size_t offset = resource_offset + index * resource_stride;
        put_u32(rdef, offset, add_name(resource.name));
        put_u32(rdef, offset + 4U, resource.type);
        put_u32(rdef, offset + 8U, resource.return_type);
        put_u32(rdef, offset + 12U, resource.dimension);
        put_u32(rdef, offset + 16U, resource.samples);
        put_u32(rdef, offset + 20U, resource.bind);
        put_u32(rdef, offset + 24U, 1U);
        put_u32(rdef, offset + 28U, resource.flags);
    }
    for (std::size_t index = 0U; index < buffers.size(); ++index) {
        const std::size_t offset = buffer_offset + index * buffer_stride;
        put_u32(rdef, offset, add_name(buffers[index].name));
        put_u32(rdef, offset + 12U, buffers[index].bytes);
    }
    put_u32(rdef, 24U, add_name("synthetic-ksPerPixel"));

    constexpr std::size_t dxbc_table_end = 40U;
    const std::size_t program_offset = dxbc_table_end + 8U + rdef.size();
    const std::size_t total_bytes = program_offset + 12U;
    std::vector<std::uint8_t> stage(total_bytes, 0U);
    stage[0U] = 'D';
    stage[1U] = 'X';
    stage[2U] = 'B';
    stage[3U] = 'C';
    put_u32(stage, 20U, 1U);
    put_u32(stage, 24U, static_cast<std::uint32_t>(stage.size()));
    put_u32(stage, 28U, 2U);
    put_u32(stage, 32U, static_cast<std::uint32_t>(dxbc_table_end));
    put_u32(stage, 36U, static_cast<std::uint32_t>(program_offset));
    put_u32(stage, dxbc_table_end, 0x46454452U);
    put_u32(stage, dxbc_table_end + 4U,
            static_cast<std::uint32_t>(rdef.size()));
    std::copy(rdef.begin(), rdef.end(), stage.begin() + 48U);
    put_u32(stage, program_offset, 0x52444853U);
    put_u32(stage, program_offset + 4U, 4U);
    put_u32(stage, program_offset + 8U,
            vertex ? 0x00010040U : 0x00000040U);
    return stage;
}

StockShaderContainer reflected_container_fixture() {
    StockShaderContainer container;
    container.header = {2U, false, "mesh", 0U, 0U, 0U};
    container.vertex_metadata = {4U, 0U, 2U};
    container.pixel_metadata = {4U, 0U, 2U};
    container.vertex_shader = rdef_stage_fixture(true);
    container.pixel_shader = rdef_stage_fixture(false);
    container.header.vertex_bytes =
        static_cast<std::uint32_t>(container.vertex_shader.size());
    container.header.pixel_bytes =
        static_cast<std::uint32_t>(container.pixel_shader.size());
    return container;
}

std::vector<std::uint8_t> signature_payload_fixture(
    std::span<const StockShaderSignatureParameter> parameters) {
    constexpr std::size_t header_bytes = 8U;
    constexpr std::size_t parameter_stride = 24U;
    const std::size_t table_end =
        header_bytes + parameters.size() * parameter_stride;
    std::vector<std::uint8_t> payload(table_end, 0U);
    put_u32(payload, 0U, static_cast<std::uint32_t>(parameters.size()));
    put_u32(payload, 4U, static_cast<std::uint32_t>(header_bytes));
    for (std::size_t index = 0U; index < parameters.size(); ++index) {
        const StockShaderSignatureParameter& parameter = parameters[index];
        const std::size_t offset = header_bytes + index * parameter_stride;
        const std::uint32_t name_offset =
            static_cast<std::uint32_t>(payload.size());
        payload.insert(payload.end(), parameter.semantic.begin(),
                       parameter.semantic.end());
        payload.push_back(0U);
        put_u32(payload, offset, name_offset);
        put_u32(payload, offset + 4U, parameter.semantic_index);
        put_u32(payload, offset + 8U,
                static_cast<std::uint32_t>(parameter.system_value));
        put_u32(payload, offset + 12U,
                static_cast<std::uint32_t>(parameter.component_type));
        put_u32(payload, offset + 16U, parameter.shader_register);
        payload[offset + 20U] = parameter.component_mask;
        payload[offset + 21U] = parameter.read_write_mask;
    }
    return payload;
}

std::vector<std::uint8_t> signature_stage_fixture(bool vertex) {
    const std::span<const StockShaderSignatureParameter> input =
        vertex ? std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_vertex_input_signature)
               : std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_fragment_input_signature);
    const std::span<const StockShaderSignatureParameter> output =
        vertex ? std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_vertex_output_signature)
               : std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_fragment_output_signature);
    const std::vector<std::uint8_t> input_payload =
        signature_payload_fixture(input);
    const std::vector<std::uint8_t> output_payload =
        signature_payload_fixture(output);
    const std::array<std::uint8_t, 4U> program = {
        0x40U, 0U, static_cast<std::uint8_t>(vertex ? 1U : 0U), 0U};

    constexpr std::size_t table_end = 44U;
    std::vector<std::uint8_t> stage(table_end, 0U);
    stage[0U] = 'D';
    stage[1U] = 'X';
    stage[2U] = 'B';
    stage[3U] = 'C';
    put_u32(stage, 20U, 1U);
    put_u32(stage, 28U, 3U);
    const auto add_chunk = [&](std::size_t table_index, std::uint32_t tag,
                               std::span<const std::uint8_t> payload) {
        const std::size_t offset = stage.size();
        put_u32(stage, 32U + table_index * 4U,
                static_cast<std::uint32_t>(offset));
        stage.resize(offset + 8U + payload.size());
        put_u32(stage, offset, tag);
        put_u32(stage, offset + 4U,
                static_cast<std::uint32_t>(payload.size()));
        std::copy(payload.begin(), payload.end(), stage.data() + offset + 8U);
    };
    add_chunk(0U, 0x4e475349U, input_payload);
    add_chunk(1U, 0x4e47534fU, output_payload);
    add_chunk(2U, 0x52444853U, program);
    put_u32(stage, 24U, static_cast<std::uint32_t>(stage.size()));
    return stage;
}

StockShaderContainer signature_container_fixture() {
    StockShaderContainer container;
    container.header = {2U, false, "mesh", 0U, 0U, 0U};
    container.vertex_metadata = {4U, 0U, 3U};
    container.pixel_metadata = {4U, 0U, 3U};
    container.vertex_shader = signature_stage_fixture(true);
    container.pixel_shader = signature_stage_fixture(false);
    container.header.vertex_bytes =
        static_cast<std::uint32_t>(container.vertex_shader.size());
    container.header.pixel_bytes =
        static_cast<std::uint32_t>(container.pixel_shader.size());
    return container;
}

std::vector<std::uint8_t> complete_native_stage_fixture(bool vertex) {
    const std::vector<std::uint8_t> rdef_stage =
        rdef_stage_fixture(vertex);
    const std::uint32_t rdef_bytes = get_u32(rdef_stage, 44U);
    const std::span<const std::uint8_t> rdef_payload(
        rdef_stage.data() + 48U, rdef_bytes);
    const std::span<const StockShaderSignatureParameter> input =
        vertex ? std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_vertex_input_signature)
               : std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_fragment_input_signature);
    const std::span<const StockShaderSignatureParameter> output =
        vertex ? std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_vertex_output_signature)
               : std::span<const StockShaderSignatureParameter>(
                     stock_ks_per_pixel_fragment_output_signature);
    const std::vector<std::uint8_t> input_payload =
        signature_payload_fixture(input);
    const std::vector<std::uint8_t> output_payload =
        signature_payload_fixture(output);
    const std::array<std::uint8_t, 4U> program = {
        0x40U, 0U, static_cast<std::uint8_t>(vertex ? 1U : 0U), 0U};

    constexpr std::size_t table_end = 48U;
    std::vector<std::uint8_t> stage(table_end, 0U);
    stage[0U] = 'D';
    stage[1U] = 'X';
    stage[2U] = 'B';
    stage[3U] = 'C';
    put_u32(stage, 20U, 1U);
    put_u32(stage, 28U, 4U);
    const auto add_chunk = [&](std::size_t table_index, std::uint32_t tag,
                               std::span<const std::uint8_t> payload) {
        const std::size_t offset = stage.size();
        put_u32(stage, 32U + table_index * 4U,
                static_cast<std::uint32_t>(offset));
        stage.resize(offset + 8U + payload.size());
        put_u32(stage, offset, tag);
        put_u32(stage, offset + 4U,
                static_cast<std::uint32_t>(payload.size()));
        std::copy(payload.begin(), payload.end(), stage.data() + offset + 8U);
    };
    add_chunk(0U, 0x46454452U, rdef_payload);
    add_chunk(1U, 0x4e475349U, input_payload);
    add_chunk(2U, 0x4e47534fU, output_payload);
    add_chunk(3U, 0x52444853U, program);
    stage.resize((stage.size() + 3U) & ~std::size_t{3U}, 0U);
    put_u32(stage, 24U, static_cast<std::uint32_t>(stage.size()));
    return stage;
}

StockShaderContainer complete_native_container_fixture(
    StockKsPerPixelVariant variant = StockKsPerPixelVariant::base) {
    StockShaderContainer container;
    container.header = {
        2U, variant == StockKsPerPixelVariant::alpha_to_coverage,
        "mesh", 0U, 0U, 0U};
    container.vertex_metadata = {4U, 0U, 4U};
    container.pixel_metadata = {4U, 0U, 4U};
    container.vertex_shader = complete_native_stage_fixture(true);
    container.pixel_shader = complete_native_stage_fixture(false);
    container.header.vertex_bytes =
        static_cast<std::uint32_t>(container.vertex_shader.size());
    container.header.pixel_bytes =
        static_cast<std::uint32_t>(container.pixel_shader.size());
    return container;
}

std::vector<std::uint8_t> complete_native_container_file_fixture(
    StockKsPerPixelVariant variant) {
    const StockShaderContainer container =
        complete_native_container_fixture(variant);
    std::vector<std::uint8_t> bytes = {
        container.header.version,
        static_cast<std::uint8_t>(container.header.alpha_tested)};
    append_u32(bytes, 0U);
    append_u32(bytes,
               static_cast<std::uint32_t>(container.vertex_shader.size()));
    bytes.insert(bytes.end(), container.vertex_shader.begin(),
                 container.vertex_shader.end());
    append_u32(bytes,
               static_cast<std::uint32_t>(container.pixel_shader.size()));
    bytes.insert(bytes.end(), container.pixel_shader.begin(),
                 container.pixel_shader.end());
    append_u32(bytes, 0U);
    return bytes;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_near(float actual, float expected, const char* message) {
    if (std::abs(actual - expected) > 1.0e-5F)
        throw std::runtime_error(std::string(message) + ": " +
                                 std::to_string(actual));
}

void exposes_exact_native_register_contract() {
    require(stock_ks_per_pixel_register_contract.size() == 11U,
            "ksPerPixel register count");
    const auto& camera = stock_ks_per_pixel_register_contract[0U];
    const auto& material = stock_ks_per_pixel_register_contract[4U];
    const auto& diffuse = stock_ks_per_pixel_register_contract[5U];
    const auto& shadow_sampler = stock_ks_per_pixel_register_contract[10U];
    require(camera.name == "cbCamera" && camera.register_index == 0U &&
                camera.constant_bytes == 224U &&
                camera.stages == StockShaderStageMask::vertex,
            "native camera register contract");
    require(material.name == "cbMaterial" && material.register_index == 4U &&
                material.constant_bytes == 32U &&
                material.stages == StockShaderStageMask::fragment,
            "native material register contract");
    require(diffuse.name == "txDiffuse" && diffuse.register_index == 0U &&
                diffuse.register_class ==
                    StockShaderRegisterClass::sampled_texture,
            "native diffuse register contract");
    require(shadow_sampler.name == "samShadow" &&
                shadow_sampler.register_index == 1U &&
                shadow_sampler.comparison_sampler,
            "native comparison sampler contract");
}

void projects_native_registers_without_namespace_collisions() {
    require(validate_stock_ks_per_pixel_backend_contract(
                stock_ks_per_pixel_backend_contract) ==
                StockKsPerPixelBindingStatus::ready,
            "canonical native backend projection accepted");
    auto reordered = stock_ks_per_pixel_backend_contract;
    std::swap(reordered[0U], reordered[10U]);
    require(validate_stock_ks_per_pixel_backend_contract(reordered) ==
                StockKsPerPixelBindingStatus::ready,
            "reflected binding table order does not affect validation");
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(
                stock_ks_per_pixel_d3d12_root_ranges) ==
                StockKsPerPixelBindingStatus::ready,
            "canonical D3D12 root ranges accepted");
    for (std::size_t index = 0U;
         index < stock_ks_per_pixel_register_contract.size(); ++index) {
        const auto& native = stock_ks_per_pixel_register_contract[index];
        const auto& backend = stock_ks_per_pixel_backend_contract[index];
        require(native.name == backend.name &&
                    native.register_class == backend.register_class &&
                    native.register_index == backend.register_index &&
                    native.stages == backend.stages &&
                    native.constant_bytes == backend.constant_bytes &&
                    native.comparison_sampler == backend.comparison_sampler,
                "backend projection matches canonical native register entry");
    }

    const auto& cb0 = stock_ks_per_pixel_backend_contract[0U];
    const auto& t0 = stock_ks_per_pixel_backend_contract[5U];
    const auto& s0 = stock_ks_per_pixel_backend_contract[9U];
    require(cb0.register_index == 0U && t0.register_index == 0U &&
                s0.register_index == 0U &&
                cb0.register_class != t0.register_class &&
                t0.register_class != s0.register_class,
            "D3D b0, t0, and s0 remain independent native namespaces");
    require(cb0.vulkan_set == 0U && t0.vulkan_set == 1U &&
                s0.vulkan_set == 2U && cb0.vulkan_binding == 0U &&
                t0.vulkan_binding == 0U && s0.vulkan_binding == 0U,
            "Vulkan separates b0, t0, and s0 into three descriptor sets");

    for (std::size_t left = 0U;
         left < stock_ks_per_pixel_backend_contract.size(); ++left) {
        for (std::size_t right = left + 1U;
             right < stock_ks_per_pixel_backend_contract.size(); ++right) {
            require(stock_ks_per_pixel_backend_contract[left].vulkan_set !=
                            stock_ks_per_pixel_backend_contract[right].vulkan_set ||
                        stock_ks_per_pixel_backend_contract[left].vulkan_binding !=
                            stock_ks_per_pixel_backend_contract[right].vulkan_binding,
                    "Vulkan stock descriptor locations are unique");
        }
    }

    const auto& vertex_cbvs = stock_ks_per_pixel_d3d12_root_ranges[0U];
    const auto& fragment_cbvs = stock_ks_per_pixel_d3d12_root_ranges[1U];
    require(vertex_cbvs.register_class ==
                    StockShaderRegisterClass::constant_buffer &&
                vertex_cbvs.base_register == 0U &&
                vertex_cbvs.descriptor_count == 4U &&
                vertex_cbvs.stages == StockShaderStageMask::vertex &&
                fragment_cbvs.base_register == 2U &&
                fragment_cbvs.descriptor_count == 3U &&
                fragment_cbvs.stages == StockShaderStageMask::fragment,
            "D3D12 root ranges preserve exact stage visibility");
}

void exposes_recovered_vertex_and_sampler_contracts() {
    require(stock_ks_per_pixel_vertex_stride_bytes == 44U &&
                stock_ks_per_pixel_index_bits == 16U &&
                stock_ks_per_pixel_vertex_contract.size() == 4U,
            "native mesh stream width and index format");
    require(stock_ks_per_pixel_vertex_contract[0U].semantic ==
                    StockKsPerPixelVertexSemantic::position &&
                stock_ks_per_pixel_vertex_contract[0U].offset_bytes == 0U &&
                stock_ks_per_pixel_vertex_contract[0U].component_count == 3U &&
                stock_ks_per_pixel_vertex_contract[1U].semantic ==
                    StockKsPerPixelVertexSemantic::normal &&
                stock_ks_per_pixel_vertex_contract[1U].offset_bytes == 12U &&
                stock_ks_per_pixel_vertex_contract[2U].semantic ==
                    StockKsPerPixelVertexSemantic::texcoord0 &&
                stock_ks_per_pixel_vertex_contract[2U].offset_bytes == 24U &&
                stock_ks_per_pixel_vertex_contract[2U].component_count == 2U &&
                stock_ks_per_pixel_vertex_contract[3U].semantic ==
                    StockKsPerPixelVertexSemantic::tangent &&
                stock_ks_per_pixel_vertex_contract[3U].offset_bytes == 32U,
            "native mesh input elements and offsets");

    const auto& linear = stock_ks_per_pixel_sampler_contract[0U];
    const auto& shadow = stock_ks_per_pixel_sampler_contract[1U];
    require(linear.name == "samLinear" && linear.register_index == 0U &&
                linear.filter == StockKsPerPixelSamplerFilter::anisotropic &&
                linear.address == StockKsPerPixelSamplerAddress::wrap &&
                linear.compare == StockKsPerPixelSamplerCompare::disabled &&
                linear.runtime_max_anisotropy &&
                linear.runtime_mip_lod_bias &&
                linear.maximum_lod == std::numeric_limits<float>::max(),
            "native anisotropic diffuse sampler contract");
    require(shadow.name == "samShadow" && shadow.register_index == 1U &&
                shadow.filter ==
                    StockKsPerPixelSamplerFilter::
                        comparison_min_mag_linear_mip_point &&
                shadow.address == StockKsPerPixelSamplerAddress::clamp &&
                shadow.compare == StockKsPerPixelSamplerCompare::less &&
                !shadow.runtime_max_anisotropy &&
                !shadow.runtime_mip_lod_bias &&
                shadow.maximum_lod == std::numeric_limits<float>::max(),
            "native comparison shadow sampler contract");
}

void rejects_malformed_backend_binding_layouts() {
    require(validate_stock_ks_per_pixel_backend_contract(
                std::span<const StockKsPerPixelBackendBinding>(
                    stock_ks_per_pixel_backend_contract.data(),
                    stock_ks_per_pixel_backend_contract.size() - 1U)) ==
                StockKsPerPixelBindingStatus::count_mismatch,
            "incomplete backend binding table rejected");

    auto bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].register_class =
        static_cast<StockShaderRegisterClass>(255U);
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_register_class,
            "unknown register class rejected before projection");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].descriptor_kind =
        static_cast<StockShaderDescriptorKind>(255U);
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_kind,
            "unknown descriptor kind rejected before projection");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].descriptor_kind = StockShaderDescriptorKind::sampled_image;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "constant buffer cannot map to a sampled image");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[5U].descriptor_kind = StockShaderDescriptorKind::sampler;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "sampled texture cannot map to a sampler");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[9U].descriptor_kind = StockShaderDescriptorKind::uniform_buffer;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "sampler cannot map to a uniform buffer");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].stages = static_cast<StockShaderStageMask>(0U);
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_stage_mask,
            "zero stage mask rejected before projection");
    bindings[0U].stages = static_cast<StockShaderStageMask>(4U);
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_stage_mask,
            "unknown stage bits rejected before projection");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].stages = StockShaderStageMask::fragment;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "camera buffer cannot become fragment-only");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[4U].stages = StockShaderStageMask::vertex;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "material buffer cannot become vertex-visible");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].name = {};
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_name,
            "empty reflected binding name rejected");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].name =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::invalid_name,
            "oversized reflected binding name rejected");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].name = "cbUnknown";
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "unknown reflected binding name rejected");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].d3d12_register_space = 1U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::d3d12_space_mismatch,
            "unrecovered D3D12 register space rejected");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[1U].register_index = bindings[0U].register_index;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::duplicate_native_binding,
            "duplicate D3D constant-buffer register rejected");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[1U].register_index = 5U;
    bindings[1U].vulkan_binding = 5U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "wrong but unique D3D register rejected");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[5U].vulkan_set = bindings[0U].vulkan_set;
    bindings[5U].vulkan_binding = bindings[0U].vulkan_binding;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::duplicate_vulkan_binding,
            "D3D register-class overlap cannot collide in Vulkan");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[5U].vulkan_binding = 5U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::vulkan_namespace_mismatch,
            "compacted Vulkan texture binding rejected");

    bindings = stock_ks_per_pixel_backend_contract;
    bindings[4U].constant_bytes = 80U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "portable material size cannot replace native b4 size");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[0U].constant_bytes = 256U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "aligned CBV allocation size cannot replace logical camera size");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[5U].constant_bytes = 4U;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "sampled texture cannot declare constant bytes");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[9U].comparison_sampler = true;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "linear sampler cannot become a comparison sampler");
    bindings = stock_ks_per_pixel_backend_contract;
    bindings[10U].comparison_sampler = false;
    require(validate_stock_ks_per_pixel_backend_contract(bindings) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "shadow sampler must retain comparison sampling");

    require(validate_stock_ks_per_pixel_d3d12_root_ranges(
                std::span<const StockKsPerPixelD3D12DescriptorRange>(
                    stock_ks_per_pixel_d3d12_root_ranges.data(), 4U)) ==
                StockKsPerPixelBindingStatus::count_mismatch,
            "incomplete D3D12 root range table rejected");
    auto ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].descriptor_count = 0U;
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "empty D3D12 descriptor range rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].register_class =
        static_cast<StockShaderRegisterClass>(255U);
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "unknown D3D12 descriptor range class rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].stages = static_cast<StockShaderStageMask>(0U);
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "empty D3D12 descriptor range stage mask rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].base_register =
        std::numeric_limits<std::uint32_t>::max();
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "overflowing D3D12 descriptor range rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].descriptor_count =
        std::numeric_limits<std::uint32_t>::max();
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::invalid_descriptor_range,
            "oversized D3D12 descriptor range rejected before allocation");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[1U] = ranges[0U];
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::duplicate_descriptor_range,
            "duplicate D3D12 root range rejected");
    ranges = stock_ks_per_pixel_d3d12_root_ranges;
    ranges[0U].base_register = 1U;
    ranges[0U].descriptor_count = 3U;
    require(validate_stock_ks_per_pixel_d3d12_root_ranges(ranges) ==
                StockKsPerPixelBindingStatus::contract_mismatch,
            "valid but incorrect D3D12 root range rejected");
}

StockShaderContainer container_fixture(StockKsPerPixelVariant variant) {
    StockShaderContainer container;
    container.header = {
        2U, variant == StockKsPerPixelVariant::alpha_to_coverage,
        "mesh", 48U, 48U, 0U};
    container.vertex_metadata = {4U, 0U, 1U};
    container.pixel_metadata = {4U, 0U, 1U};
    container.vertex_shader.resize(48U);
    container.pixel_shader.resize(48U);
    return container;
}

void validates_owned_container_shape_before_backend_use() {
    auto base = container_fixture(StockKsPerPixelVariant::base);
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::ready,
            "base container shape accepted");
    auto at = container_fixture(StockKsPerPixelVariant::alpha_to_coverage);
    require(validate_stock_ks_per_pixel_container_shape(
                at, StockKsPerPixelVariant::alpha_to_coverage) ==
                StockKsPerPixelContainerStatus::ready,
            "AT container shape accepted");
    require(validate_stock_ks_per_pixel_container_shape(
                at, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::alpha_flag_mismatch,
            "AT package cannot enter the base pipeline state");
    require(validate_stock_ks_per_pixel_container_shape(
                base, static_cast<StockKsPerPixelVariant>(255U)) ==
                StockKsPerPixelContainerStatus::invalid_variant,
            "unknown stock variant rejected");

    base.header.version = 1U;
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::unsupported_version,
            "unexpected stock package version rejected");
    base = container_fixture(StockKsPerPixelVariant::base);
    base.header.vertex_layout = "skinned";
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::unsupported_layout,
            "unexpected stock vertex layout rejected");

    base = container_fixture(StockKsPerPixelVariant::base);
    base.vertex_shader.pop_back();
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::stage_size_mismatch,
            "truncated owned vertex stage rejected");
    base = container_fixture(StockKsPerPixelVariant::base);
    base.vertex_metadata.shader_model_major = 5U;
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::unsupported_shader_model,
            "unexpected shader model rejected");
    base = container_fixture(StockKsPerPixelVariant::base);
    base.header.geometry_bytes = 48U;
    base.geometry_shader.resize(48U);
    base.geometry_metadata = StockShaderStageMetadata{4U, 0U, 1U};
    require(validate_stock_ks_per_pixel_container_shape(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelContainerStatus::unsupported_geometry,
            "unexpected geometry stage rejected");
}

void validates_bounded_rdef_resource_contract() {
    auto container = reflected_container_fixture();
    require(validate_stock_ks_per_pixel_reflection(container) ==
                StockKsPerPixelReflectionStatus::ready,
            "synthetic reflected stock contract accepted");

    const std::uint32_t vertex_rdef_bytes =
        get_u32(container.vertex_shader, 44U);
    for (std::uint32_t prefix = 0U; prefix < vertex_rdef_bytes; ++prefix) {
        auto truncated = container;
        put_u32(truncated.vertex_shader, 44U, prefix);
        require(validate_stock_ks_per_pixel_reflection(truncated) !=
                    StockKsPerPixelReflectionStatus::ready,
                "every truncated RDEF payload is rejected");
    }

    auto malformed = container;
    put_u32(malformed.vertex_shader, 40U, 0x54415453U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::missing_rdef,
            "missing RDEF chunk rejected");

    malformed = container;
    const std::size_t vertex_program = get_u32(malformed.vertex_shader, 36U);
    put_u32(malformed.vertex_shader, vertex_program, 0x46454452U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::duplicate_rdef,
            "duplicate RDEF chunk rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, 44U, 27U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::truncated_rdef_header,
            "truncated RDEF header rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, 48U + 16U, 0xffff0400U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::target_mismatch,
            "RDEF target-stage mismatch rejected");

    malformed = container;
    put_u32(malformed.pixel_shader, 48U + 8U,
            std::numeric_limits<std::uint32_t>::max());
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::count_mismatch,
            "oversized RDEF resource count rejected before multiplication");

    malformed = container;
    put_u32(malformed.pixel_shader, 48U + 12U,
            std::numeric_limits<std::uint32_t>::max());
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::table_out_of_bounds,
            "out-of-range RDEF resource table rejected");

    malformed = container;
    const std::uint32_t pixel_rdef_bytes =
        get_u32(malformed.pixel_shader, 44U);
    put_u32(malformed.pixel_shader, 48U + 24U, pixel_rdef_bytes - 1U);
    malformed.pixel_shader[48U + pixel_rdef_bytes - 1U] = 'X';
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::invalid_name,
            "unterminated RDEF creator name rejected");

    malformed = container;
    const std::size_t pixel_resource_offset =
        48U + get_u32(malformed.pixel_shader, 48U + 12U);
    put_u32(malformed.pixel_shader,
            pixel_resource_offset + 2U * 32U + 20U, 1U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::resource_mismatch,
            "wrong reflected texture register rejected");

    malformed = container;
    put_u32(malformed.pixel_shader, pixel_resource_offset + 32U,
            get_u32(malformed.pixel_shader, pixel_resource_offset));
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::resource_mismatch,
            "duplicate reflected resource name rejected");

    malformed = container;
    const std::size_t pixel_buffer_offset =
        48U + get_u32(malformed.pixel_shader, 48U + 4U);
    put_u32(malformed.pixel_shader, pixel_buffer_offset + 12U, 159U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::constant_buffer_mismatch,
            "wrong reflected constant-buffer size rejected");

    malformed = container;
    malformed.vertex_shader.pop_back();
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::invalid_stage_container,
            "truncated DXBC stage is rejected before RDEF access");
}

void validates_exact_bounded_stage_signatures() {
    require(stock_ks_per_pixel_stage_interface_is_consistent,
            "vertex output and fragment input linkage is consistent");
    require(stock_ks_per_pixel_vertex_input_signature.size() == 4U &&
                stock_ks_per_pixel_vertex_output_signature.size() == 8U &&
                stock_ks_per_pixel_fragment_input_signature.size() == 8U &&
                stock_ks_per_pixel_fragment_output_signature.size() == 1U,
            "exact stock signature counts are public");
    require(stock_ks_per_pixel_vertex_input_signature[3U].semantic ==
                    "TANGENT" &&
                stock_ks_per_pixel_vertex_input_signature[3U]
                        .read_write_mask == 0U &&
                stock_ks_per_pixel_fragment_output_signature[0U].semantic ==
                    "SV_TARGET" &&
                stock_ks_per_pixel_fragment_output_signature[0U]
                        .system_value ==
                    StockShaderSignatureSystemValue::none,
            "native unused tangent and raw SV_TARGET metadata are retained");

    const auto container = signature_container_fixture();
    require(validate_stock_ks_per_pixel_signatures(container) ==
                StockKsPerPixelSignatureStatus::ready,
            "synthetic exact stock signatures accepted");

    const std::uint32_t input_bytes =
        get_u32(container.vertex_shader, 48U);
    for (std::uint32_t prefix = 0U; prefix < input_bytes; ++prefix) {
        auto truncated = container;
        put_u32(truncated.vertex_shader, 48U, prefix);
        require(validate_stock_ks_per_pixel_signatures(truncated) !=
                    StockKsPerPixelSignatureStatus::ready,
                "every truncated signature payload is rejected");
    }

    auto malformed = container;
    put_u32(malformed.vertex_shader, 44U, 0x54415453U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::missing_signature,
            "missing ISGN chunk rejected");

    malformed = container;
    const std::size_t output_chunk =
        get_u32(malformed.vertex_shader, 36U);
    put_u32(malformed.vertex_shader, output_chunk, 0x4e475349U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::duplicate_signature,
            "duplicate ISGN chunk rejected");

    constexpr std::size_t input_payload = 52U;
    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 4U, 4U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::table_out_of_bounds,
            "signature table before its header rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 4U, 16U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::parameter_mismatch,
            "non-native signature record offset rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 4U, input_bytes + 1U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::table_out_of_bounds,
            "signature record offset past the payload rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload,
            std::numeric_limits<std::uint32_t>::max());
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::count_mismatch,
            "overflow-sized signature count rejected before multiplication");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 8U, 0U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::invalid_name,
            "signature name inside the record table rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 8U, input_bytes);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::invalid_name,
            "signature name at the payload end rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 8U, input_bytes - 1U);
    malformed.vertex_shader[input_payload + input_bytes - 1U] = 'X';
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::invalid_name,
            "unterminated signature name rejected");

    malformed = container;
    malformed.vertex_shader[input_payload + 8U + 22U] = 1U;
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::parameter_mismatch,
            "nonzero legacy signature reserved field rejected");

    malformed = container;
    malformed.vertex_shader[input_payload + 8U + 20U] = 0x10U;
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::parameter_mismatch,
            "invalid signature component mask rejected");

    malformed = container;
    put_u32(malformed.vertex_shader, input_payload + 8U + 4U, 1U);
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::parameter_mismatch,
            "semantic-index drift rejected");

    malformed = container;
    malformed.pixel_shader.pop_back();
    require(validate_stock_ks_per_pixel_signatures(malformed) ==
                StockKsPerPixelSignatureStatus::invalid_stage_container,
            "truncated signature stage rejected before record access");
}

void gates_the_complete_native_program_before_backend_allocation() {
    auto base = complete_native_container_fixture();
    require(validate_stock_ks_per_pixel_native_program(
                base, StockKsPerPixelVariant::base) ==
                StockKsPerPixelNativeProgramStatus::ready,
            "complete base native program contract accepted");
    auto alpha = complete_native_container_fixture(
        StockKsPerPixelVariant::alpha_to_coverage);
    require(validate_stock_ks_per_pixel_native_program(
                alpha, StockKsPerPixelVariant::alpha_to_coverage) ==
                StockKsPerPixelNativeProgramStatus::ready,
            "complete alpha-to-coverage native program contract accepted");
    require(validate_stock_ks_per_pixel_native_program(
                base, static_cast<StockKsPerPixelVariant>(255U)) ==
                StockKsPerPixelNativeProgramStatus::invalid_variant,
            "unknown native program variant rejected");

    auto malformed = base;
    malformed.header.version = 1U;
    require(validate_stock_ks_per_pixel_native_program(
                malformed, StockKsPerPixelVariant::base) ==
                StockKsPerPixelNativeProgramStatus::container_shape_mismatch,
            "native program shape failure stops the allocation gate");

    malformed = base;
    constexpr std::size_t rdef_payload = 56U;
    put_u32(malformed.vertex_shader, rdef_payload + 16U, 0xffff0400U);
    require(validate_stock_ks_per_pixel_native_program(
                malformed, StockKsPerPixelVariant::base) ==
                StockKsPerPixelNativeProgramStatus::reflection_mismatch,
            "native program reflection failure stops the allocation gate");

    malformed = base;
    const std::size_t vertex_input_chunk =
        get_u32(malformed.vertex_shader, 36U);
    put_u32(malformed.vertex_shader,
            vertex_input_chunk + 8U + 8U + 4U, 1U);
    require(validate_stock_ks_per_pixel_native_program(
                malformed, StockKsPerPixelVariant::base) ==
                StockKsPerPixelNativeProgramStatus::signature_mismatch,
            "native program signature failure stops the allocation gate");

    malformed = base;
    const std::size_t vertex_output_chunk =
        get_u32(malformed.vertex_shader, 40U);
    put_u32(malformed.vertex_shader, vertex_output_chunk, 0x52444853U);
    require(validate_stock_ks_per_pixel_reflection(malformed) ==
                StockKsPerPixelReflectionStatus::invalid_stage_container,
            "duplicate executable chunks reject a direct reflection request");
}

void owns_the_validated_native_program_after_the_gate() {
    auto source = complete_native_container_fixture();
    const std::size_t vertex_bytes = source.vertex_shader.size();
    const std::size_t pixel_bytes = source.pixel_shader.size();
    const std::uint8_t vertex_tail = source.vertex_shader.back();
    const std::uint8_t pixel_tail = source.pixel_shader.back();

    auto created = create_validated_stock_ks_per_pixel_native_program(
        std::move(source), StockKsPerPixelVariant::base);
    require(created.ok() && created.program.has_value(),
            "complete package creates an owned validated native program");
    source.vertex_shader.assign(32U, 0xffU);
    source.pixel_shader.assign(32U, 0xffU);
    require(created.program->variant() == StockKsPerPixelVariant::base &&
                created.program->vertex_shader().size() == vertex_bytes &&
                created.program->pixel_shader().size() == pixel_bytes &&
                created.program->vertex_shader().back() == vertex_tail &&
                created.program->pixel_shader().back() == pixel_tail,
            "caller mutation after move cannot change validated stage bytes");

    ValidatedStockKsPerPixelNativeProgram moved =
        std::move(*created.program);
    created.program.reset();
    require(moved.vertex_shader().size() == vertex_bytes &&
                moved.pixel_shader().size() == pixel_bytes,
            "validated stage ownership survives program moves");

    auto alpha_source = complete_native_container_fixture(
        StockKsPerPixelVariant::alpha_to_coverage);
    auto alpha = create_validated_stock_ks_per_pixel_native_program(
        std::move(alpha_source),
        StockKsPerPixelVariant::alpha_to_coverage);
    require(alpha.ok() && alpha.program->variant() ==
                              StockKsPerPixelVariant::alpha_to_coverage,
            "alpha package retains its explicit native variant");

    auto wrong_variant = complete_native_container_fixture();
    auto rejected_variant = create_validated_stock_ks_per_pixel_native_program(
        std::move(wrong_variant),
        StockKsPerPixelVariant::alpha_to_coverage);
    require(!rejected_variant.ok() && !rejected_variant.program.has_value() &&
                rejected_variant.status ==
                    StockKsPerPixelNativeProgramStatus::container_shape_mismatch,
            "variant mismatch produces no validated native object");

    auto malformed = complete_native_container_fixture();
    const std::size_t input_chunk = get_u32(malformed.vertex_shader, 36U);
    put_u32(malformed.vertex_shader, input_chunk + 8U + 8U + 4U, 1U);
    auto rejected_signature =
        create_validated_stock_ks_per_pixel_native_program(
            std::move(malformed), StockKsPerPixelVariant::base);
    require(!rejected_signature.ok() &&
                !rejected_signature.program.has_value() &&
                rejected_signature.status ==
                    StockKsPerPixelNativeProgramStatus::signature_mismatch,
            "malformed signature produces no validated native object");
}

ValidatedStockKsPerPixelNativeProgram validated_program_fixture(
    StockKsPerPixelVariant variant = StockKsPerPixelVariant::base) {
    auto result = create_validated_stock_ks_per_pixel_native_program(
        complete_native_container_fixture(variant), variant);
    require(result.ok(), "native shader allocation fixture passes the gate");
    return std::move(*result.program);
}

void clones_validated_native_programs_without_aliasing_mutable_state() {
    const auto original = validated_program_fixture();
    auto cloned = clone_validated_stock_ks_per_pixel_native_program(original);
    require(cloned.ok() && cloned.program.has_value(),
            "validated native program clone passes the complete gate");
    require(cloned.program->variant() == original.variant() &&
                cloned.program->vertex_shader().size() ==
                    original.vertex_shader().size() &&
                cloned.program->pixel_shader().size() ==
                    original.pixel_shader().size() &&
                std::equal(cloned.program->vertex_shader().begin(),
                           cloned.program->vertex_shader().end(),
                           original.vertex_shader().begin()) &&
                std::equal(cloned.program->pixel_shader().begin(),
                           cloned.program->pixel_shader().end(),
                           original.pixel_shader().begin()),
            "native clone preserves the exact validated DXBC payloads");
}

void loads_only_exact_validated_native_program_files() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "apex-ks-per-pixel-native-program-file-test.shader";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};
    const auto write_bytes = [&](std::span<const std::uint8_t> bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output),
                "native program file fixture opens");
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(output),
                "native program file fixture write completes");
    };

    const std::vector<std::uint8_t> base_bytes =
        complete_native_container_file_fixture(
            StockKsPerPixelVariant::base);
    write_bytes(base_bytes);
    auto loaded = load_validated_stock_ks_per_pixel_native_program_file(
        path, StockKsPerPixelVariant::base);
    require(loaded.ok() && loaded.program->variant() ==
                               StockKsPerPixelVariant::base,
            "exact base native program file passes the complete gate");

    auto wrong_variant =
        load_validated_stock_ks_per_pixel_native_program_file(
            path, StockKsPerPixelVariant::alpha_to_coverage);
    require(!wrong_variant.ok() &&
                wrong_variant.file_status ==
                    StockShaderContainerFileStatus::ready &&
                wrong_variant.program_status ==
                    StockKsPerPixelNativeProgramStatus::
                        container_shape_mismatch &&
                !wrong_variant.program.has_value(),
            "loaded base package cannot enter the alpha native variant");

    const std::vector<std::uint8_t> alpha_bytes =
        complete_native_container_file_fixture(
            StockKsPerPixelVariant::alpha_to_coverage);
    write_bytes(alpha_bytes);
    loaded = load_validated_stock_ks_per_pixel_native_program_file(
        path, StockKsPerPixelVariant::alpha_to_coverage);
    require(loaded.ok() && loaded.program->variant() ==
                               StockKsPerPixelVariant::alpha_to_coverage,
            "exact alpha native program file passes the complete gate");

    write_bytes(std::span<const std::uint8_t>(alpha_bytes.data(),
                                              alpha_bytes.size() - 1U));
    const auto truncated_alpha =
        load_validated_stock_ks_per_pixel_native_program_file(
            path, StockKsPerPixelVariant::alpha_to_coverage);
    require(!truncated_alpha.ok() &&
                truncated_alpha.file_status ==
                    StockShaderContainerFileStatus::malformed_package &&
                !truncated_alpha.program.has_value(),
            "truncated alpha native program file returns no validated owner");

    write_bytes(std::span<const std::uint8_t>(base_bytes.data(),
                                              base_bytes.size() - 1U));
    const auto truncated =
        load_validated_stock_ks_per_pixel_native_program_file(
            path, StockKsPerPixelVariant::base);
    require(!truncated.ok() &&
                truncated.file_status ==
                    StockShaderContainerFileStatus::malformed_package &&
                !truncated.program.has_value(),
            "truncated native program file returns no validated owner");
}

void executes_a_validated_native_program_through_the_static_scene_batch() {
    FakeShaderDevice device(Backend::D3D12);

    apex::formats::Kn5File model;
    model.materials.resize(1U);
    model.materials[0].name = "body";
    model.materials[0].shader = "ksPerPixel";
    model.materials[0].properties.push_back(
        {"fresnelMaxLevel", 0.0F, {}, {}, {}});
    model.materials[0].resources.push_back(
        {"txDiffuse", 0U, "body.dds"});
    model.textures.push_back(
        {true, "body.dds", 4U, {}, std::nullopt});
    model.textures.push_back(
        {true, "unused.dds", 4U, {}, std::nullopt});
    model.root.type = 1U;
    model.root.kind = "node";
    model.root.name = "ROOT";
    apex::formats::Kn5Node mesh;
    mesh.type = 2U;
    mesh.kind = "mesh";
    mesh.name = "BODY";
    mesh.materialId = 0U;
    mesh.vertexStride = 11U;
    mesh.vertices.resize(33U, 0.0F);
    mesh.vertices[0U] = -0.5F;
    mesh.vertices[11U] = 0.5F;
    mesh.vertices[23U] = 0.5F;
    mesh.indices = {0U, 1U, 2U};
    apex::formats::Kn5Node second_mesh = mesh;
    second_mesh.name = "BODY_SECOND";
    model.root.children.push_back(std::move(mesh));
    model.root.children.push_back(std::move(second_mesh));

    apex::scene::SceneSnapshot scene;
    (void)scene.add_material(
        {"body", "ksPerPixel", apex::scene::BlendMode::opaque});
    apex::scene::SceneNode root;
    root.name = "ROOT";
    const apex::scene::NodeId root_id = scene.add_node(std::move(root));
    apex::scene::SceneNode node;
    node.name = "BODY";
    node.kind = apex::scene::NodeKind::mesh;
    node.material = 0U;
    const apex::scene::NodeId node_id =
        scene.add_node(std::move(node), root_id);
    apex::scene::SceneNode second_node;
    second_node.name = "BODY_SECOND";
    second_node.kind = apex::scene::NodeKind::mesh;
    second_node.material = 0U;
    const apex::scene::NodeId second_node_id =
        scene.add_node(std::move(second_node), root_id);

    DrawPacket packet;
    packet.node = node_id;
    packet.material = 0U;
    packet.primitive = DrawPrimitiveKind::static_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 11U;
    packet.material_profile.shader = "ksPerPixel";
    packet.resources.push_back({"txDiffuse", 0U, 0U, "body.dds"});
    DrawPacket second_packet = packet;
    second_packet.node = second_node_id;
    second_packet.world_matrix[12U] = 2.0F;
    const std::array<DrawPacket, 2U> packets = {packet, second_packet};

    auto native_owner =
        std::make_shared<const ValidatedStockKsPerPixelNativeProgram>(
            validated_program_fixture());
    const std::array<StockMaterialD3D12NativeProgram, 1U> native_programs = {
        StockMaterialD3D12NativeProgram{"ksPerPixel", native_owner}};
    StockMaterialExecutionRequest request;
    request.model = &model;
    request.scene = &scene;
    request.packets = packets;
    request.builtin_d3d12_native =
        BuiltinD3D12StockNativeSelector::ks_per_pixel_base;
    request.builtin_d3d12_native_programs = native_programs;
    request.targets.colors.push_back(
        {PipelineRenderTargetFormat::rgba8_unorm, 1U});
    request.targets.has_depth = true;
    request.targets.depth =
        {PipelineRenderTargetFormat::depth32_float, 1U};

    auto alpha_model = model;
    alpha_model.materials[0U].shader = "ksPerPixelAT";
    alpha_model.materials[0U].serializedBlendMode = 2U;
    auto alpha_scene = scene;
    alpha_scene.materials[0U].shader = "ksPerPixelAT";
    alpha_scene.materials[0U].blend_mode =
        apex::scene::BlendMode::alpha_to_coverage;
    auto alpha_packets = packets;
    for (DrawPacket& alpha_packet : alpha_packets) {
        alpha_packet.material_profile.shader = "ksPerPixelAT";
        alpha_packet.flags.alpha_to_coverage = true;
    }
    auto alpha_owner =
        std::make_shared<const ValidatedStockKsPerPixelNativeProgram>(
            validated_program_fixture(
                StockKsPerPixelVariant::alpha_to_coverage));
    const std::array<StockMaterialD3D12NativeProgram, 1U>
        alpha_native_programs = {
            StockMaterialD3D12NativeProgram{"ksPerPixelAT", alpha_owner}};
    auto alpha_request = request;
    alpha_request.model = &alpha_model;
    alpha_request.scene = &alpha_scene;
    alpha_request.packets = alpha_packets;
    alpha_request.builtin_d3d12_native =
        BuiltinD3D12StockNativeSelector::ks_per_pixel_alpha_to_coverage;
    alpha_request.builtin_d3d12_native_programs = alpha_native_programs;
    alpha_request.targets.colors.front().samples = 4U;
    alpha_request.targets.depth.samples = 4U;
    FakeShaderDevice alpha_device(Backend::D3D12);
    const auto alpha_prepared = prepare_stock_material_execution(
        alpha_device, alpha_request);
    require(alpha_prepared.ok() &&
                alpha_prepared.resources
                        ->stock_d3d12_native_program_count() == 1U &&
                alpha_prepared.resources
                        ->requires_stock_d3d12_native_frame() &&
                alpha_device.shader_calls == 4U &&
                alpha_device.buffer_calls == 15U &&
                alpha_device.sampler_calls == 4U,
            alpha_prepared.diagnostic.code.empty()
                ? "native D3D12 ksPerPixelAT scene preparation"
                : alpha_prepared.diagnostic.code.c_str());

    auto alpha_one_sample_request = alpha_request;
    alpha_one_sample_request.targets.colors.front().samples = 1U;
    alpha_one_sample_request.targets.depth.samples = 1U;
    FakeShaderDevice alpha_one_sample_device(Backend::D3D12);
    const auto alpha_one_sample = prepare_stock_material_execution(
        alpha_one_sample_device, alpha_one_sample_request);
    require(!alpha_one_sample.ok() &&
                alpha_one_sample.diagnostic.code ==
                    "stock_material_d3d12_native_target_unsupported" &&
                alpha_one_sample_device.shader_calls == 0U &&
                alpha_one_sample_device.buffer_calls == 0U &&
                alpha_one_sample_device.sampler_calls == 0U,
            "native ksPerPixelAT rejects one-sample targets before allocation");

    const std::array<StockMaterialD3D12NativeProgram, 1U>
        wrong_alpha_programs = {
            StockMaterialD3D12NativeProgram{"ksPerPixelAT", native_owner}};
    auto wrong_alpha_request = alpha_request;
    wrong_alpha_request.builtin_d3d12_native_programs = wrong_alpha_programs;
    FakeShaderDevice wrong_alpha_device(Backend::D3D12);
    const auto wrong_alpha = prepare_stock_material_execution(
        wrong_alpha_device, wrong_alpha_request);
    require(!wrong_alpha.ok() &&
                wrong_alpha.diagnostic.code ==
                    "stock_material_d3d12_native_program_invalid" &&
                wrong_alpha_device.shader_calls == 0U &&
                wrong_alpha_device.buffer_calls == 0U &&
                wrong_alpha_device.sampler_calls == 0U,
            "native ksPerPixelAT rejects a base owner before allocation");

    constexpr std::uint64_t per_draw_native_constant_bytes =
        static_cast<std::uint64_t>(
            stock_ks_per_pixel_native_constant_buffer_view_bytes) *
        static_cast<std::uint64_t>(
            StockKsPerPixelNativeConstantSlot::count);
    FakeShaderDevice constant_limit_device(Backend::D3D12);
    auto constant_limit_request = request;
    constant_limit_request.limits.scene
        .max_total_stock_d3d12_native_constant_bytes =
        per_draw_native_constant_bytes;
    const auto constant_limited = prepare_stock_material_execution(
        constant_limit_device, constant_limit_request);
    require(!constant_limited.ok() &&
                constant_limited.diagnostic.code ==
                    "static_scene_stock_d3d12_native_constant_aggregate_limit" &&
                constant_limit_device.shader_calls == 0U &&
                constant_limit_device.buffer_calls == 0U &&
                constant_limit_device.sampler_calls == 0U,
            "native constant-byte limits reject multiple draws before allocation");
    FakeShaderDevice update_limit_device(Backend::D3D12);
    auto update_limit_request = request;
    update_limit_request.limits.scene.max_total_update_bytes =
        static_cast<std::uint64_t>(
            stock_ks_per_pixel_native_constant_buffer_view_bytes) *
        4U;
    const auto update_limited = prepare_stock_material_execution(
        update_limit_device, update_limit_request);
    require(!update_limited.ok() &&
                update_limited.diagnostic.code ==
                    "static_scene_stock_d3d12_native_update_aggregate_limit" &&
                update_limit_device.shader_calls == 0U &&
                update_limit_device.buffer_calls == 0U &&
                update_limit_device.sampler_calls == 0U,
            "native update-byte limits reject multiple draws before allocation");
    FakeShaderDevice draw_limit_device(Backend::D3D12);
    auto draw_limit_request = request;
    draw_limit_request.limits.scene.max_draws = 1U;
    const auto draw_limited = prepare_stock_material_execution(
        draw_limit_device, draw_limit_request);
    require(!draw_limited.ok() &&
                draw_limited.diagnostic.code ==
                    "stock_material_packet_count_invalid" &&
                draw_limit_device.shader_calls == 0U &&
                draw_limit_device.buffer_calls == 0U &&
                draw_limit_device.sampler_calls == 0U,
            "native draw-count limits reject before backend allocation");

    constexpr std::array<PipelineRenderTargetFormat, 4U>
        supported_native_formats = {
            PipelineRenderTargetFormat::rgba8_unorm,
            PipelineRenderTargetFormat::rgba8_srgb,
            PipelineRenderTargetFormat::bgra8_unorm,
            PipelineRenderTargetFormat::bgra8_srgb};
    for (const PipelineRenderTargetFormat format :
         supported_native_formats) {
        FakeShaderDevice format_device(Backend::D3D12);
        auto format_request = request;
        format_request.targets.colors.front().format = format;
        const auto accepted = prepare_stock_material_execution(
            format_device, format_request);
        require(accepted.ok() && format_device.shader_calls == 4U,
                "native static scene accepts one-sample RGBA8 and BGRA8 targets");
    }
    FakeShaderDevice unsupported_format_device(Backend::D3D12);
    auto unsupported_format_request = request;
    unsupported_format_request.targets.colors.front().format =
        PipelineRenderTargetFormat::rgba16_float;
    const auto unsupported_format = prepare_stock_material_execution(
        unsupported_format_device, unsupported_format_request);
    require(!unsupported_format.ok() &&
                unsupported_format.diagnostic.code ==
                    "static_scene_material_pipeline_unsupported" &&
                unsupported_format_device.shader_calls == 0U &&
                unsupported_format_device.buffer_calls == 0U &&
                unsupported_format_device.sampler_calls == 0U,
            "native unsupported color formats reject before backend allocation");
    FakeShaderDevice unsupported_depth_device(Backend::D3D12);
    auto unsupported_depth_request = request;
    unsupported_depth_request.targets.depth.format =
        PipelineRenderTargetFormat::depth24_stencil8;
    const auto unsupported_depth = prepare_stock_material_execution(
        unsupported_depth_device, unsupported_depth_request);
    require(!unsupported_depth.ok() &&
                unsupported_depth.diagnostic.code ==
                    "static_scene_material_pipeline_unsupported" &&
                unsupported_depth_device.shader_calls == 0U &&
                unsupported_depth_device.buffer_calls == 0U &&
                unsupported_depth_device.sampler_calls == 0U,
            "native unsupported depth formats reject before backend allocation");
    FakeShaderDevice multisample_device(Backend::D3D12);
    auto multisample_request = request;
    multisample_request.targets.colors.front().samples = 4U;
    multisample_request.targets.depth.samples = 4U;
    const auto multisample = prepare_stock_material_execution(
        multisample_device, multisample_request);
    require(!multisample.ok() &&
                multisample.diagnostic.code ==
                    "stock_material_d3d12_native_target_unsupported" &&
                multisample_device.shader_calls == 0U &&
                multisample_device.buffer_calls == 0U &&
                multisample_device.sampler_calls == 0U,
            "native multisampling rejects before backend allocation");

    auto prepared = prepare_stock_material_execution(device, request);
    require(prepared.ok() && prepared.resources->draw_count() == 2U &&
                prepared.resources->stock_d3d12_native_program_count() == 1U &&
                prepared.resources->requires_stock_d3d12_native_frame(),
            prepared.diagnostic.code.empty()
                ? "native D3D12 stock scene preparation"
                : prepared.diagnostic.code.c_str());
    require(device.shader_calls == 4U && device.buffer_calls == 14U &&
                device.sampler_calls == 4U,
            "scene preparation allocates one private native resource bundle per draw");

    CameraFrameRequest camera_request;
    camera_request.eye = {0.0F, 0.0F, 2.0F};
    camera_request.target = {0.0F, 0.0F, 0.0F};
    camera_request.aspect = 1.0F;
    camera_request.near_plane = 0.1F;
    camera_request.far_plane = 100.0F;
    camera_request.clip_space = CameraClipSpace::d3d12;
    const CameraFrameResult camera = build_camera_frame(camera_request);
    require(camera.ok(), "native scene camera frame");

    const TextureDescription sampled_description{
        1U, 1U, 1U, 1U, TextureFormat::rgba8_unorm,
        TextureUsage::sampled, TextureMemory::device_local,
        TextureMutability::immutable};
    FakeNativeTexture diffuse(Backend::D3D12, sampled_description, &device);
    const std::array<const Texture*, 2U> textures = {&diffuse, nullptr};
    std::array<FakeNativeDepth, 3U> shadow_maps = {
        FakeNativeDepth(Backend::D3D12,
                        {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true},
                        &device),
        FakeNativeDepth(Backend::D3D12,
                        {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true},
                        &device),
        FakeNativeDepth(Backend::D3D12,
                        {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true},
                        &device)};
    FakeNativeDepth depth(
        Backend::D3D12,
        {16U, 16U, 1U, DepthAttachmentFormat::d32_float, false}, &device);
    FakeNativeTexture target(
        Backend::D3D12,
        {16U, 16U, 1U, 1U, TextureFormat::rgba8_unorm,
         TextureUsage::color_attachment | TextureUsage::transfer_source,
         TextureMemory::device_local, TextureMutability::mutable_data},
        &device);

    StaticSceneFrameDescription::StockNativeFrame native_frame;
    native_frame.camera = make_stock_ks_per_pixel_camera_constants(
        camera.frame->view, camera.frame->projection,
        apex::scene::identity_matrix, {0.0F, 0.0F, 2.0F}, 0.1F,
        100.0F, camera.frame->fov_radians);
    native_frame.lighting.light_direction = {0.0F, -1.0F, 0.0F};
    native_frame.shadow_constants =
        make_stock_directional_shadow_receiver_constants(
            {apex::scene::identity_matrix, apex::scene::identity_matrix,
             apex::scene::identity_matrix},
            {0.0F, 0.0F, 0.0F}, 32U);
    native_frame.shadow_maps = {
        &shadow_maps[0], &shadow_maps[1], &shadow_maps[2]};
    StaticSceneFrameDescription frame;
    frame.camera = *camera.frame;
    frame.depth_attachment = &depth;
    frame.clear_depth = true;
    frame.textures_by_global_index = textures;
    frame.stock_d3d12_native_frame = native_frame;

    const IndexedStaticMeshBatchResult drawn =
        prepared.resources->draw_and_readback(device, target, frame);
    require(drawn.ok() && device.batch_calls == 1U &&
                device.native_draws == 2U &&
                device.native_resource_owners.size() == 2U &&
                device.native_resource_owners[0U] != nullptr &&
                device.native_resource_owners[0U]->ready() &&
                device.native_resource_owners[1U] != nullptr &&
                device.native_resource_owners[1U]->ready() &&
                device.native_resource_owners[0U] !=
                    device.native_resource_owners[1U] &&
                device.buffer_update_calls == 8U,
            drawn.diagnostic.code.empty()
                ? "native static scene submits two independent D3D12 authority draws"
                : drawn.diagnostic.code.c_str());
    StockKsPerPixelObjectConstants first_object;
    StockKsPerPixelObjectConstants second_object;
    require(device.updated_buffers.size() == 8U &&
                device.updated_buffers[1U].size() >= sizeof(first_object) &&
                device.updated_buffers[5U].size() >= sizeof(second_object),
            "each native draw updates its private object record");
    std::memcpy(&first_object, device.updated_buffers[1U].data(),
                sizeof(first_object));
    std::memcpy(&second_object, device.updated_buffers[5U].data(),
                sizeof(second_object));
    require(first_object.world == stock_ks_per_pixel_transpose_matrix(
                                      packet.world_matrix) &&
                second_object.world == stock_ks_per_pixel_transpose_matrix(
                                       second_packet.world_matrix) &&
                first_object.world != second_object.world,
            "separate native draws retain independent object transforms");

    FakeShaderDevice foreign_device(Backend::D3D12);
    FakeNativeTexture foreign_target(
        Backend::D3D12, target.info().description, &foreign_device);
    const std::size_t updates_before_rejection = device.buffer_update_calls;
    const std::size_t batches_before_rejection = device.batch_calls;
    auto rejected = prepared.resources->draw_and_readback(
        device, foreign_target, frame);
    require(!rejected.ok() &&
                rejected.diagnostic.code ==
                    "static_scene_target_device_mismatch" &&
                device.buffer_update_calls == updates_before_rejection &&
                device.batch_calls == batches_before_rejection,
            "foreign same-backend targets reject before native updates");

    frame.resolve_target = &foreign_target;
    rejected = prepared.resources->draw_and_readback(device, target, frame);
    require(!rejected.ok() &&
                rejected.diagnostic.code ==
                    "static_scene_resolve_target_device_mismatch" &&
                device.buffer_update_calls == updates_before_rejection &&
                device.batch_calls == batches_before_rejection,
            "foreign same-backend resolve targets reject before native updates");
    frame.resolve_target = nullptr;

    FakeNativeDepth foreign_depth(
        Backend::D3D12, depth.info().description, &foreign_device);
    frame.depth_attachment = &foreign_depth;
    rejected = prepared.resources->draw_and_readback(device, target, frame);
    require(!rejected.ok() &&
                rejected.diagnostic.code ==
                    "static_scene_depth_attachment_device_mismatch" &&
                device.buffer_update_calls == updates_before_rejection &&
                device.batch_calls == batches_before_rejection,
            "foreign same-backend depth rejects before native updates");
    frame.depth_attachment = &depth;

    FakeNativeTexture foreign_diffuse(
        Backend::D3D12, diffuse.info().description, &foreign_device);
    const std::array<const Texture*, 2U> foreign_textures = {
        &foreign_diffuse, nullptr};
    frame.textures_by_global_index = foreign_textures;
    rejected = prepared.resources->draw_and_readback(device, target, frame);
    require(!rejected.ok() &&
                rejected.diagnostic.code ==
                    "static_scene_texture_device_mismatch" &&
                device.buffer_update_calls == updates_before_rejection &&
                device.batch_calls == batches_before_rejection,
            "foreign same-backend diffuse rejects before native updates");
    frame.textures_by_global_index = textures;

    FakeNativeDepth foreign_shadow(
        Backend::D3D12, shadow_maps[0U].info().description,
        &foreign_device);
    frame.stock_d3d12_native_frame->shadow_maps[0U] = &foreign_shadow;
    rejected = prepared.resources->draw_and_readback(device, target, frame);
    require(!rejected.ok() &&
                rejected.diagnostic.code ==
                    "static_scene_stock_d3d12_native_shadow_map_device_mismatch" &&
                device.buffer_update_calls == updates_before_rejection &&
                device.batch_calls == batches_before_rejection,
            "foreign same-backend native shadow maps reject before updates");

    frame.stock_d3d12_native_frame->shadow_maps[0U] = &shadow_maps[0U];
    const std::array<const Texture*, 2U> unused_foreign_texture = {
        &diffuse, &foreign_diffuse};
    frame.textures_by_global_index = unused_foreign_texture;
    const auto unused_foreign = prepared.resources->draw_and_readback(
        device, target, frame);
    require(unused_foreign.ok() &&
                device.buffer_update_calls == updates_before_rejection + 8U &&
                device.batch_calls == batches_before_rejection + 1U,
            "unused foreign caller-table entries do not reject a native draw");
}

void allocates_only_validated_native_d3d12_shader_objects() {
    require(FakeShaderModule::live_count == 0U,
            "native shader allocation test starts without live objects");

    FakeShaderDevice d3d12(Backend::D3D12);
    auto validated = validated_program_fixture();
    const std::size_t vertex_bytes = validated.vertex_shader().size();
    const std::size_t pixel_bytes = validated.pixel_shader().size();
    auto allocated = allocate_stock_ks_per_pixel_native_shaders(
        d3d12, std::move(validated));
    require(allocated.ok() && d3d12.shader_calls == 2U &&
                d3d12.stages ==
                    std::vector<ShaderStage>{ShaderStage::vertex,
                                             ShaderStage::fragment} &&
                d3d12.sizes ==
                    std::vector<std::size_t>{vertex_bytes, pixel_bytes} &&
                allocated.program->source().variant() ==
                    StockKsPerPixelVariant::base &&
                allocated.program->vertex_shader().info().format ==
                    ShaderBytecodeFormat::dxbc &&
                allocated.program->pixel_shader().info().format ==
                    ShaderBytecodeFormat::dxbc &&
                FakeShaderModule::live_count == 2U,
            "D3D12 allocates and owns the exact validated shader pair");
    allocated.program.reset();
    require(FakeShaderModule::live_count == 0U,
            "native shader program releases both shader objects");

    FakeShaderDevice moved_from_device(Backend::D3D12);
    const auto rejected_moved_from =
        allocate_stock_ks_per_pixel_native_shaders(
            moved_from_device, std::move(validated));
    require(!rejected_moved_from.ok() &&
                rejected_moved_from.status ==
                    StockKsPerPixelNativeShaderStatus::invalid_program &&
                moved_from_device.shader_calls == 0U,
            "moved-from native proof rejects before a device call");

    FakeShaderDevice vulkan(Backend::Vulkan);
    auto vulkan_source = validated_program_fixture();
    const std::size_t retained_vertex_bytes =
        vulkan_source.vertex_shader().size();
    const auto rejected_vulkan =
        allocate_stock_ks_per_pixel_native_shaders(
            vulkan, std::move(vulkan_source));
    require(!rejected_vulkan.ok() &&
                rejected_vulkan.status ==
                    StockKsPerPixelNativeShaderStatus::backend_unsupported &&
                rejected_vulkan.diagnostic.code ==
                    "stock_native_shader_backend_unsupported" &&
                vulkan.shader_calls == 0U &&
                vulkan_source.vertex_shader().size() == retained_vertex_bytes,
            "Vulkan rejects native DXBC before allocation and retains the source");

    FakeShaderDevice vertex_failure(Backend::D3D12);
    vertex_failure.fail_call = 1U;
    auto vertex_source = validated_program_fixture();
    const auto rejected_vertex =
        allocate_stock_ks_per_pixel_native_shaders(
            vertex_failure, std::move(vertex_source));
    require(!rejected_vertex.ok() &&
                rejected_vertex.status ==
                    StockKsPerPixelNativeShaderStatus::vertex_shader_failed &&
                vertex_failure.shader_calls == 1U &&
                FakeShaderModule::live_count == 0U,
            "vertex allocation failure stops before the pixel shader");

    FakeShaderDevice pixel_failure(Backend::D3D12);
    pixel_failure.fail_call = 2U;
    auto pixel_source = validated_program_fixture();
    const auto rejected_pixel =
        allocate_stock_ks_per_pixel_native_shaders(
            pixel_failure, std::move(pixel_source));
    require(!rejected_pixel.ok() &&
                rejected_pixel.status ==
                    StockKsPerPixelNativeShaderStatus::pixel_shader_failed &&
                pixel_failure.shader_calls == 2U &&
                FakeShaderModule::live_count == 0U,
            "pixel allocation failure releases the vertex shader object");

    FakeShaderDevice wrong_module(Backend::D3D12);
    wrong_module.wrong_format = true;
    auto wrong_source = validated_program_fixture();
    const auto rejected_module =
        allocate_stock_ks_per_pixel_native_shaders(
            wrong_module, std::move(wrong_source));
    require(!rejected_module.ok() &&
                rejected_module.status ==
                    StockKsPerPixelNativeShaderStatus::invalid_shader_module &&
                wrong_module.shader_calls == 1U &&
                FakeShaderModule::live_count == 0U,
            "unexpected backend shader metadata is rejected and released");

    require(std::string_view(
                stock_ks_per_pixel_native_shader_status_name(
                    StockKsPerPixelNativeShaderStatus::pixel_shader_failed)) ==
                "pixel_shader_failed" &&
                std::string_view(
                    stock_ks_per_pixel_native_shader_status_name(
                        static_cast<StockKsPerPixelNativeShaderStatus>(255U))) ==
                    "unknown",
            "native shader allocation statuses have stable names");
}

StockKsPerPixelNativeConstantData native_constant_data_fixture() {
    StockKsPerPixelNativeConstantData constants;
    constants.camera.view = apex::scene::identity_matrix;
    constants.camera.projection = apex::scene::identity_matrix;
    constants.camera.mvp_inverse = apex::scene::identity_matrix;
    constants.camera.near_plane = 0.1F;
    constants.camera.far_plane = 1'000.0F;
    constants.camera.field_of_view = 1.0F;
    constants.object.world = apex::scene::identity_matrix;
    constants.lighting.light_direction = {0.0F, -1.0F, 0.0F};
    constants.lighting.ambient_color = {0.2F, 0.2F, 0.2F};
    constants.lighting.light_color = {1.0F, 1.0F, 1.0F};
    constants.lighting.exposure = 1.0F;
    constants.shadow_maps.shadow_matrices.fill(apex::scene::identity_matrix);
    constants.shadow_maps.biases = {0.001F, 0.002F, 0.003F};
    constants.shadow_maps.texture_size = 1.0F / 1'024.0F;
    constants.material.ambient = 0.35F;
    constants.material.diffuse = 0.8F;
    constants.material.specular = 0.2F;
    constants.material.specular_exponent = 30.0F;
    return constants;
}

template <typename Record>
bool padded_record_matches(const std::vector<std::byte>& bytes,
                           const Record& record) {
    if (bytes.size() !=
            stock_ks_per_pixel_native_constant_buffer_view_bytes ||
        std::memcmp(bytes.data(), &record, sizeof(record)) != 0)
        return false;
    return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(record)),
                       bytes.end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

void allocates_exact_native_d3d12_constant_buffer_views() {
    require(FakeConstantBuffer::live_count == 0U,
            "native constant allocation starts without live buffers");
    const StockKsPerPixelNativeConstantData constants =
        native_constant_data_fixture();
    FakeShaderDevice d3d12(Backend::D3D12);
    auto allocated = allocate_stock_ks_per_pixel_native_constant_buffers(
        d3d12, constants);
    require(allocated.ok() && d3d12.buffer_calls == 5U &&
                d3d12.initial_buffers.size() == 5U &&
                std::all_of(
                    d3d12.buffer_descriptions.begin(),
                    d3d12.buffer_descriptions.end(),
                    [](const BufferDescription& description) {
                        return description.size_bytes ==
                                   stock_ks_per_pixel_native_constant_buffer_view_bytes &&
                               description.usage == BufferUsage::uniform &&
                               description.memory == BufferMemory::host_visible &&
                               description.mutability ==
                                   BufferMutability::mutable_data;
                    }) &&
                padded_record_matches(d3d12.initial_buffers[0U],
                                      constants.camera) &&
                padded_record_matches(d3d12.initial_buffers[1U],
                                      constants.object) &&
                padded_record_matches(d3d12.initial_buffers[2U],
                                      constants.lighting) &&
                padded_record_matches(d3d12.initial_buffers[3U],
                                      constants.shadow_maps) &&
                padded_record_matches(d3d12.initial_buffers[4U],
                                      constants.material) &&
                FakeConstantBuffer::live_count == 5U,
            "D3D12 creates five exact zero-padded native constant views");
    for (std::size_t index = 0U; index < 5U; ++index)
        require(allocated.buffers->buffer(
                    static_cast<StockKsPerPixelNativeConstantSlot>(index)) !=
                    nullptr,
                "native constant owner exposes each validated slot");
    require(allocated.buffers->buffer(
                StockKsPerPixelNativeConstantSlot::count) == nullptr,
            "native constant owner rejects the count sentinel");
    allocated.buffers.reset();
    require(FakeConstantBuffer::live_count == 0U,
            "native constant owner releases all five buffers");

    auto invalid_constants = constants;
    invalid_constants.shadow_maps.texture_size = 0.0F;
    FakeShaderDevice invalid_device(Backend::D3D12);
    const auto invalid = allocate_stock_ks_per_pixel_native_constant_buffers(
        invalid_device, invalid_constants);
    require(!invalid.ok() &&
                invalid.status ==
                    StockKsPerPixelNativeConstantBufferStatus::invalid_constants &&
                invalid_device.buffer_calls == 0U,
            "invalid native constants reject before buffer allocation");

    FakeShaderDevice vulkan(Backend::Vulkan);
    auto allocated_vulkan =
        allocate_stock_ks_per_pixel_native_constant_buffers(vulkan, constants);
    require(allocated_vulkan.ok() && vulkan.buffer_calls == 5U &&
                allocated_vulkan.buffers->device() == &vulkan,
            "Vulkan allocates the recovered native-ABI constant records");
    allocated_vulkan.buffers.reset();

    FakeShaderDevice partial_failure(Backend::D3D12);
    partial_failure.buffer_fail_call = 3U;
    const auto rejected_partial =
        allocate_stock_ks_per_pixel_native_constant_buffers(partial_failure,
                                                            constants);
    require(!rejected_partial.ok() &&
                rejected_partial.status ==
                    StockKsPerPixelNativeConstantBufferStatus::buffer_failed &&
                partial_failure.buffer_calls == 3U &&
                FakeConstantBuffer::live_count == 0U,
            "partial native constant failure releases earlier buffers");

    FakeShaderDevice wrong_buffer(Backend::D3D12);
    wrong_buffer.wrong_buffer = true;
    const auto rejected_buffer =
        allocate_stock_ks_per_pixel_native_constant_buffers(wrong_buffer,
                                                            constants);
    require(!rejected_buffer.ok() &&
                rejected_buffer.status ==
                    StockKsPerPixelNativeConstantBufferStatus::invalid_buffer &&
                wrong_buffer.buffer_calls == 1U &&
                FakeConstantBuffer::live_count == 0U,
            "invalid backend buffer metadata is rejected and released");

    require(std::string_view(
                stock_ks_per_pixel_native_constant_buffer_status_name(
                    StockKsPerPixelNativeConstantBufferStatus::buffer_failed)) ==
                "buffer_failed" &&
                std::string_view(
                    stock_ks_per_pixel_native_constant_buffer_status_name(
                        static_cast<
                            StockKsPerPixelNativeConstantBufferStatus>(255U))) ==
                    "unknown",
            "native constant-buffer statuses have stable names");
}

void allocates_exact_native_stock_samplers() {
    require(FakeNativeSampler::live_count == 0U,
            "native sampler allocation starts without live samplers");
    FakeShaderDevice device(Backend::D3D12);
    const StockKsPerPixelNativeSamplerSettings settings{8.0F, -0.75F};
    auto allocated = allocate_stock_ks_per_pixel_native_samplers(
        device, settings);
    require(allocated.ok() && device.sampler_calls == 2U &&
                device.sampler_descriptions.size() == 2U &&
                FakeNativeSampler::live_count == 2U,
            "native stock sampler owner retains both sampler objects");

    const SamplerDescription& linear = device.sampler_descriptions[0U];
    require(linear.min_filter == SamplerFilter::anisotropic &&
                linear.mag_filter == SamplerFilter::anisotropic &&
                linear.mip_filter == SamplerFilter::anisotropic &&
                linear.address_u == SamplerAddressMode::repeat &&
                linear.address_v == SamplerAddressMode::repeat &&
                linear.address_w == SamplerAddressMode::repeat &&
                linear.compare == SamplerCompare::disabled &&
                linear.mip_lod_bias == settings.mip_lod_bias &&
                linear.max_anisotropy == settings.max_anisotropy &&
                linear.min_lod == 0.0F &&
                linear.max_lod == std::numeric_limits<float>::max(),
            "s0 preserves the recovered runtime anisotropic sampler");

    const SamplerDescription& shadow = device.sampler_descriptions[1U];
    require(shadow.min_filter == SamplerFilter::linear &&
                shadow.mag_filter == SamplerFilter::linear &&
                shadow.mip_filter == SamplerFilter::nearest &&
                shadow.address_u == SamplerAddressMode::clamp_to_edge &&
                shadow.address_v == SamplerAddressMode::clamp_to_edge &&
                shadow.address_w == SamplerAddressMode::clamp_to_edge &&
                shadow.compare == SamplerCompare::less &&
                shadow.mip_lod_bias == 0.0F &&
                shadow.max_anisotropy == 1.0F &&
                shadow.min_lod == 0.0F &&
                shadow.max_lod == std::numeric_limits<float>::max(),
            "s1 preserves the recovered comparison sampler");
    require(allocated.samplers->sampler(
                StockKsPerPixelNativeSamplerSlot::linear) != nullptr &&
                allocated.samplers->sampler(
                    StockKsPerPixelNativeSamplerSlot::shadow) != nullptr &&
                allocated.samplers->sampler(
                    StockKsPerPixelNativeSamplerSlot::count) == nullptr,
            "native sampler owner exposes only exact validated slots");
    allocated.samplers.reset();
    require(FakeNativeSampler::live_count == 0U,
            "native sampler owner releases both objects");

    FakeShaderDevice invalid_device(Backend::D3D12);
    auto invalid_settings = settings;
    invalid_settings.max_anisotropy = 1.0F;
    const auto invalid = allocate_stock_ks_per_pixel_native_samplers(
        invalid_device, invalid_settings);
    require(!invalid.ok() &&
                invalid.status ==
                    StockKsPerPixelNativeSamplerStatus::invalid_settings &&
                invalid_device.sampler_calls == 0U,
            "invalid runtime sampler settings reject before device calls");

    FakeShaderDevice vulkan(Backend::Vulkan);
    const auto rejected_vulkan =
        allocate_stock_ks_per_pixel_native_samplers(vulkan, settings);
    require(!rejected_vulkan.ok() &&
                rejected_vulkan.status ==
                    StockKsPerPixelNativeSamplerStatus::backend_unsupported &&
                rejected_vulkan.diagnostic.code ==
                    "stock_native_sampler_backend_unsupported" &&
                vulkan.sampler_calls == 0U,
            "Vulkan rejects the native D3D12 sampler path before allocation");

    FakeShaderDevice partial_failure(Backend::D3D12);
    partial_failure.sampler_fail_call = 2U;
    const auto rejected_partial =
        allocate_stock_ks_per_pixel_native_samplers(partial_failure, settings);
    require(!rejected_partial.ok() &&
                rejected_partial.status ==
                    StockKsPerPixelNativeSamplerStatus::shadow_sampler_failed &&
                partial_failure.sampler_calls == 2U &&
                FakeNativeSampler::live_count == 0U,
            "shadow sampler failure releases the linear sampler");

    FakeShaderDevice wrong_sampler(Backend::D3D12);
    wrong_sampler.wrong_sampler = true;
    const auto rejected_sampler =
        allocate_stock_ks_per_pixel_native_samplers(wrong_sampler, settings);
    require(!rejected_sampler.ok() &&
                rejected_sampler.status ==
                    StockKsPerPixelNativeSamplerStatus::invalid_sampler &&
                wrong_sampler.sampler_calls == 1U &&
                FakeNativeSampler::live_count == 0U,
            "invalid backend sampler metadata is rejected and released");

    Diagnostic sampler_diagnostic;
    SamplerDescription exact_lod;
    exact_lod.max_lod = std::numeric_limits<float>::max();
    exact_lod.mip_lod_bias = -16.0F;
    require(validate_sampler_description(exact_lod, sampler_diagnostic) ==
                SamplerStatus::ready,
            "generic sampler contract accepts recovered native LOD values");
    exact_lod.mip_lod_bias = std::numeric_limits<float>::infinity();
    require(validate_sampler_description(exact_lod, sampler_diagnostic) ==
                SamplerStatus::invalid_description &&
                sampler_diagnostic.code == "sampler_lod_bias_invalid",
            "sampler validation rejects nonfinite LOD bias");

    require(std::string_view(
                stock_ks_per_pixel_native_sampler_status_name(
                    StockKsPerPixelNativeSamplerStatus::shadow_sampler_failed)) ==
                "shadow_sampler_failed" &&
                std::string_view(
                    stock_ks_per_pixel_native_sampler_status_name(
                        StockKsPerPixelNativeSamplerStatus::backend_unsupported)) ==
                    "backend_unsupported" &&
                std::string_view(
                    stock_ks_per_pixel_native_sampler_status_name(
                        static_cast<StockKsPerPixelNativeSamplerStatus>(255U))) ==
                    "unknown",
            "native sampler statuses have stable names");
}

void validates_complete_native_draw_bindings_before_execution() {
    FakeShaderDevice device(Backend::D3D12);
    auto source = validated_program_fixture();
    auto shaders = allocate_stock_ks_per_pixel_native_shaders(device, std::move(source));
    auto constants = allocate_stock_ks_per_pixel_native_constant_buffers(
        device, native_constant_data_fixture());
    auto samplers = allocate_stock_ks_per_pixel_native_samplers(device);
    require(shaders.ok() && constants.ok() && samplers.ok(),
            "native draw fixture owns its validated resources");
    auto resources_result = make_stock_ks_per_pixel_native_draw_resources(
        device, std::move(shaders.program), std::move(constants.buffers),
        std::move(samplers.samplers));
    require(resources_result.ok() && resources_result.resources->ready(),
            "native draw fixture adopts one validated resource owner");
    auto resources = std::move(resources_result.resources);

    const TextureDescription target_description{
        16U, 16U, 1U, 1U, TextureFormat::rgba8_unorm,
        TextureUsage::color_attachment | TextureUsage::transfer_source,
        TextureMemory::device_local, TextureMutability::mutable_data};
    FakeNativeTexture target(Backend::D3D12, target_description);
    FakeNativeTexture diffuse(
        Backend::D3D12,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
         TextureMemory::device_local, TextureMutability::immutable});
    const DepthAttachmentDescription shadow_description{
        32U, 32U, 1U, DepthAttachmentFormat::d32_float, true};
    FakeNativeDepth shadow0(Backend::D3D12, shadow_description);
    FakeNativeDepth shadow1(Backend::D3D12, shadow_description);
    FakeNativeDepth shadow2(Backend::D3D12, shadow_description);
    FakeConstantBuffer vertices(
        Backend::D3D12,
        {3U * stock_ks_per_pixel_vertex_stride_bytes, BufferUsage::vertex,
         BufferMemory::device_local, BufferMutability::immutable});
    FakeConstantBuffer indices(
        Backend::D3D12,
        {3U * sizeof(std::uint16_t), BufferUsage::index,
         BufferMemory::device_local, BufferMutability::immutable});

    PipelineProgram pipeline;
    pipeline.name = "native-ks-per-pixel-state";
    pipeline.targets.colors.push_back({PipelineRenderTargetFormat::rgba8_unorm, 1U});
    pipeline.raster.cull = PipelineCullMode::none;
    pipeline.depth.test_enabled = false;
    pipeline.depth.write_enabled = false;
    pipeline.depth.compare = PipelineCompareOperation::less;
    pipeline.vertex_layout.stride = stock_ks_per_pixel_vertex_stride_bytes;
    pipeline.vertex_layout.attributes = {
        {PipelineVertexSemantic::position, PipelineVertexAttributeFormat::float32x3, 0U, 0U},
        {PipelineVertexSemantic::normal, PipelineVertexAttributeFormat::float32x3, 1U, 12U},
        {PipelineVertexSemantic::texcoord0, PipelineVertexAttributeFormat::float32x2, 2U, 24U},
        {PipelineVertexSemantic::tangent, PipelineVertexAttributeFormat::float32x3, 3U, 32U},
    };
    DrawPacket packet;
    packet.primitive = DrawPrimitiveKind::static_mesh;
    packet.vertex_count = 3U;
    packet.index_count = 3U;
    packet.vertex_stride_floats = 11U;
    packet.flags.depth_test = false;
    packet.flags.depth_write = false;
    const StockKsPerPixelNativeDrawBinding binding = resources->bind(
        diffuse, {&shadow0, &shadow1, &shadow2});
    IndexedStaticMeshDrawRequest request;
    request.packet = &packet;
    request.pipeline = &pipeline;
    request.vertex_buffer = &vertices;
    request.index_buffer = &indices;
    request.shader_authority = IndexedShaderAuthority::explicit_stock_ks_per_pixel_native;
    request.stock_ks_per_pixel_native = &binding;

    Diagnostic diagnostic;
    const auto valid_status = validate_indexed_static_mesh_draw_request(
        target, request, diagnostic);
    require(valid_status == IndexedStaticMeshDrawStatus::ready,
            diagnostic.code.empty() ?
                "complete native binding passes before command recording" :
                diagnostic.code.c_str());
    const std::array<IndexedStaticMeshDrawRequest, 2U> native_requests = {
        request, request};
    IndexedStaticMeshBatchDescription native_batch;
    native_batch.draws = native_requests;
    require(validate_indexed_static_mesh_batch_description(
                target, native_batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            diagnostic.code.empty()
                ? "native-only base batch passes common preflight"
                : diagnostic.code.c_str());
    const std::vector<IndexedStaticMeshDrawRequest> oversized_native_requests(
        max_stock_ks_per_pixel_native_batch_draws + 1U, request);
    native_batch.draws = oversized_native_requests;
    require(validate_indexed_static_mesh_batch_description(
                target, native_batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code == "indexed_stock_native_batch_draw_limit",
            "native-only batch preserves the D3D12 sampler-heap bound");
    auto mixed_requests = native_requests;
    mixed_requests[1U].shader_authority =
        IndexedShaderAuthority::packet_contract;
    native_batch.draws = mixed_requests;
    require(validate_indexed_static_mesh_batch_description(
                target, native_batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code ==
                    "indexed_stock_native_batch_mixed_unsupported",
            "mixed native and portable batch is explicitly unsupported");
    native_batch.draws = native_requests;
    native_batch.capture_rgba8 = false;
    require(validate_indexed_static_mesh_batch_description(
                target, native_batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::ready,
            "native-only base batch accepts retained execution without CPU capture");
    DrawPacket wireframe_packet = packet;
    wireframe_packet.flags.wireframe = true;
    PipelineProgram wireframe_pipeline = pipeline;
    wireframe_pipeline.raster.fill = PipelineFillMode::wireframe;
    IndexedStaticMeshDrawRequest wireframe_request = request;
    wireframe_request.packet = &wireframe_packet;
    wireframe_request.pipeline = &wireframe_pipeline;
    const std::array<IndexedStaticMeshDrawRequest, 1U> wireframe_requests = {
        wireframe_request};
    native_batch.draws = wireframe_requests;
    native_batch.capture_rgba8 = true;
    require(validate_indexed_static_mesh_batch_description(
                target, native_batch, diagnostic) ==
                IndexedStaticMeshBatchStatus::unsupported &&
                diagnostic.code ==
                    "indexed_stock_native_batch_feature_unsupported",
            "native-only batch reports staged wireframe support explicitly");
    const auto expect_missing_binding_field =
        [&](const StockKsPerPixelNativeDrawBinding& candidate,
            const char* description) {
            auto candidate_request = request;
            candidate_request.stock_ks_per_pixel_native = &candidate;
            require(validate_indexed_static_mesh_draw_request(
                        target, candidate_request, diagnostic) ==
                        IndexedStaticMeshDrawStatus::invalid_request &&
                        diagnostic.code == "indexed_stock_native_binding_missing",
                    description);
        };
    auto missing_diffuse = binding;
    missing_diffuse.diffuse_texture = nullptr;
    expect_missing_binding_field(missing_diffuse,
                                 "missing native diffuse texture is rejected");
    for (std::size_t shadow_index = 0U;
         shadow_index < binding.shadow_maps.size(); ++shadow_index) {
        auto missing_shadow = binding;
        missing_shadow.shadow_maps[shadow_index] = nullptr;
        expect_missing_binding_field(
            missing_shadow, "missing native shadow map is rejected");
    }

    auto duplicate_shadow = binding;
    duplicate_shadow.shadow_maps[1U] = duplicate_shadow.shadow_maps[0U];
    auto duplicate_request = request;
    duplicate_request.stock_ks_per_pixel_native = &duplicate_shadow;
    require(validate_indexed_static_mesh_draw_request(
                target, duplicate_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_stock_native_shadow_map_duplicate",
            "duplicate native shadow maps are rejected");

    auto diffuse_target_alias = binding;
    diffuse_target_alias.diffuse_texture = &target;
    auto diffuse_target_request = request;
    diffuse_target_request.stock_ks_per_pixel_native = &diffuse_target_alias;
    require(validate_indexed_static_mesh_draw_request(
                target, diffuse_target_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_stock_native_diffuse_feedback_loop",
            "native diffuse and color target alias is rejected");

    FakeNativeDepth main_depth(Backend::D3D12,
                               {16U, 16U, 1U, DepthAttachmentFormat::d32_float,
                                true});
    auto shadow_depth_alias = binding;
    shadow_depth_alias.shadow_maps[0U] = &main_depth;
    auto shadow_depth_request = request;
    shadow_depth_request.depth_attachment = &main_depth;
    shadow_depth_request.stock_ks_per_pixel_native = &shadow_depth_alias;
    require(validate_indexed_static_mesh_draw_request(
                target, shadow_depth_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_stock_native_shadow_feedback_loop",
            "native shadow and main depth alias is rejected");

    FakeNativeDepth mismatched_shadow(
        Backend::D3D12,
        {16U, 16U, 1U, DepthAttachmentFormat::d32_float, true});
    auto mismatched_dimensions = binding;
    mismatched_dimensions.shadow_maps[1U] = &mismatched_shadow;
    auto mismatched_dimensions_request = request;
    mismatched_dimensions_request.stock_ks_per_pixel_native =
        &mismatched_dimensions;
    require(validate_indexed_static_mesh_draw_request(
                target, mismatched_dimensions_request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_stock_native_shadow_dimensions_mismatch",
            "native shadow dimensions must match");

    auto missing = request;
    missing.stock_ks_per_pixel_native = nullptr;
    require(validate_indexed_static_mesh_draw_request(target, missing, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_stock_native_binding_missing",
            "missing native owner rejects before pipeline work");
    FakeNativeTexture vulkan_target(Backend::Vulkan, target_description);
    require(validate_indexed_static_mesh_draw_request(vulkan_target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::unsupported &&
                diagnostic.code == "indexed_stock_native_backend_unsupported",
            "Vulkan rejects installed DXBC before execution");
    auto portable_overlap = request;
    portable_overlap.sampled_binding.texture = &diffuse;
    require(validate_indexed_static_mesh_draw_request(target, portable_overlap, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_resource_binding_unexpected",
            "native and portable material bindings cannot overlap");

    packet.flags.alpha_to_coverage = true;
    pipeline.blend.alpha_to_coverage = true;
    pipeline.targets.colors[0U].samples = 4U;
    TextureDescription multisample_description = target_description;
    multisample_description.samples = 4U;
    FakeNativeTexture multisample_target(Backend::D3D12,
                                         multisample_description);
    require(validate_indexed_static_mesh_draw_request(
                multisample_target, request, diagnostic) ==
                IndexedStaticMeshDrawStatus::invalid_request &&
                diagnostic.code == "indexed_stock_native_variant_state_mismatch",
            "base package rejects alpha-to-coverage draw state");
}

void adopts_native_draw_resources_with_one_stable_owner() {
    FakeShaderDevice d3d12(Backend::D3D12);
    auto shaders = allocate_stock_ks_per_pixel_native_shaders(
        d3d12, validated_program_fixture());
    auto constants = allocate_stock_ks_per_pixel_native_constant_buffers(
        d3d12, native_constant_data_fixture());
    auto samplers = allocate_stock_ks_per_pixel_native_samplers(d3d12);
    require(shaders.ok() && constants.ok() && samplers.ok(),
            "resource bundle fixture allocations succeed");
    auto resources_result = make_stock_ks_per_pixel_native_draw_resources(
        d3d12, std::move(shaders.program), std::move(constants.buffers),
        std::move(samplers.samplers));
    require(resources_result.ok() && resources_result.resources->ready() &&
                resources_result.resources->backend() == Backend::D3D12 &&
                resources_result.resources->device() == &d3d12,
            "resource bundle adopts complete D3D12 owners");
    auto resources = std::move(resources_result.resources);
    require(resources->shader_program().device() == &d3d12 &&
                resources->constant_buffers().device() == &d3d12 &&
                resources->samplers().device() == &d3d12,
            "resource bundle retains the supplying device identity");
    FakeNativeTexture diffuse(
        Backend::D3D12,
        {2U, 2U, 1U, 1U, TextureFormat::rgba8_unorm, TextureUsage::sampled,
         TextureMemory::device_local, TextureMutability::immutable});
    FakeNativeDepth shadow0(
        Backend::D3D12, {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true});
    FakeNativeDepth shadow1(
        Backend::D3D12, {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true});
    FakeNativeDepth shadow2(
        Backend::D3D12, {32U, 32U, 1U, DepthAttachmentFormat::d32_float, true});
    const StockKsPerPixelNativeDrawBinding binding = resources->bind(
        diffuse, {&shadow0, &shadow1, &shadow2});
    require(binding.resources == resources.get() &&
                binding.diffuse_texture == &diffuse &&
                binding.shadow_maps[2U] == &shadow2,
            "resource bundle creates a borrowed draw binding view");

    const auto missing_shader = make_stock_ks_per_pixel_native_draw_resources(
        d3d12, nullptr, nullptr, nullptr);
    require(!missing_shader.ok() &&
                missing_shader.status ==
                    StockKsPerPixelNativeDrawResourcesStatus::shader_missing &&
                missing_shader.diagnostic.code ==
                    "stock_native_draw_resources_shader_missing",
            "resource bundle rejects an incomplete shader owner deterministically");

    auto missing_constants_shader = allocate_stock_ks_per_pixel_native_shaders(
        d3d12, validated_program_fixture());
    require(missing_constants_shader.ok(),
            "missing-constants fixture shader allocation succeeds");
    const auto missing_constants =
        make_stock_ks_per_pixel_native_draw_resources(
            d3d12, std::move(missing_constants_shader.program), nullptr,
            nullptr);
    require(!missing_constants.ok() &&
                missing_constants.status ==
                    StockKsPerPixelNativeDrawResourcesStatus::constants_missing &&
                missing_constants.diagnostic.code ==
                    "stock_native_draw_resources_constants_missing",
            "resource bundle rejects missing constant owners deterministically");

    auto missing_samplers_shader = allocate_stock_ks_per_pixel_native_shaders(
        d3d12, validated_program_fixture());
    auto missing_samplers_constants =
        allocate_stock_ks_per_pixel_native_constant_buffers(
            d3d12, native_constant_data_fixture());
    require(missing_samplers_shader.ok() && missing_samplers_constants.ok(),
            "missing-samplers fixture allocations succeed");
    const auto missing_samplers =
        make_stock_ks_per_pixel_native_draw_resources(
            d3d12, std::move(missing_samplers_shader.program),
            std::move(missing_samplers_constants.buffers), nullptr);
    require(!missing_samplers.ok() &&
                missing_samplers.status ==
                    StockKsPerPixelNativeDrawResourcesStatus::samplers_missing &&
                missing_samplers.diagnostic.code ==
                    "stock_native_draw_resources_samplers_missing",
            "resource bundle rejects missing sampler owners deterministically");

    auto cross_device_shaders = allocate_stock_ks_per_pixel_native_shaders(
        d3d12, validated_program_fixture());
    auto cross_device_constants =
        allocate_stock_ks_per_pixel_native_constant_buffers(
            d3d12, native_constant_data_fixture());
    auto cross_device_samplers = allocate_stock_ks_per_pixel_native_samplers(d3d12);
    require(cross_device_shaders.ok() && cross_device_constants.ok() &&
                cross_device_samplers.ok(),
            "cross-device fixture allocations succeed");
    FakeShaderDevice other_d3d12(Backend::D3D12);
    const auto cross_device = make_stock_ks_per_pixel_native_draw_resources(
        other_d3d12, std::move(cross_device_shaders.program),
        std::move(cross_device_constants.buffers),
        std::move(cross_device_samplers.samplers));
    require(!cross_device.ok() &&
                cross_device.status ==
                    StockKsPerPixelNativeDrawResourcesStatus::device_mismatch &&
                cross_device.diagnostic.code ==
                    "stock_native_draw_resources_device_mismatch",
            "resource bundle rejects owners from another D3D12 device");

    auto wrong_shaders = allocate_stock_ks_per_pixel_native_shaders(
        d3d12, validated_program_fixture());
    auto wrong_constants = allocate_stock_ks_per_pixel_native_constant_buffers(
        d3d12, native_constant_data_fixture());
    auto wrong_samplers = allocate_stock_ks_per_pixel_native_samplers(d3d12);
    require(wrong_shaders.ok() && wrong_constants.ok() && wrong_samplers.ok(),
            "wrong-backend fixture allocations succeed");
    FakeShaderDevice vulkan(Backend::Vulkan);
    const auto wrong_backend = make_stock_ks_per_pixel_native_draw_resources(
        vulkan, std::move(wrong_shaders.program),
        std::move(wrong_constants.buffers), std::move(wrong_samplers.samplers));
    require(!wrong_backend.ok() &&
                wrong_backend.status ==
                    StockKsPerPixelNativeDrawResourcesStatus::backend_unsupported &&
                wrong_backend.diagnostic.code ==
                    "stock_native_draw_resources_backend_unsupported",
            "resource bundle rejects a non-D3D12 device deterministically");
    require(std::string_view(
                stock_ks_per_pixel_native_draw_resources_status_name(
                    StockKsPerPixelNativeDrawResourcesStatus::backend_unsupported)) ==
                "backend_unsupported" &&
                std::string_view(
                    stock_ks_per_pixel_native_draw_resources_status_name(
                        StockKsPerPixelNativeDrawResourcesStatus::device_mismatch)) ==
                    "device_mismatch" &&
                std::string_view(
                    stock_ks_per_pixel_native_draw_resources_status_name(
                        static_cast<StockKsPerPixelNativeDrawResourcesStatus>(255U))) ==
                    "unknown",
            "resource bundle statuses have stable names");
    resources.reset();
    require(FakeShaderModule::live_count == 0U &&
                FakeConstantBuffer::live_count == 0U &&
                FakeNativeSampler::live_count == 0U,
            "resource bundle releases all adopted owners together");
}

void preserves_native_constant_bytes_and_rejects_nonfinite_records() {
    StockKsPerPixelMaterialConstants material;
    material.ambient = 0.35F;
    material.diffuse = 0.80F;
    material.specular = 0.20F;
    material.specular_exponent = 30.0F;
    material.emissive = {0.1F, 0.2F, 0.3F};
    material.alpha_reference = 0.5F;
    const auto words =
        std::bit_cast<std::array<std::uint32_t, 8U>>(material);
    const std::array<std::uint32_t, 8U> expected = {
        0x3eb33333U, 0x3f4ccccdU, 0x3e4ccccdU, 0x41f00000U,
        0x3dcccccdU, 0x3e4ccccdU, 0x3e99999aU, 0x3f000000U};
    require(words == expected, "native material bytes match reflected packing");
    require(valid_stock_ks_per_pixel_material_constants(material),
            "finite native material record accepted");
    material.emissive[1U] = std::numeric_limits<float>::infinity();
    require(!valid_stock_ks_per_pixel_material_constants(material),
            "non-finite native material record rejected");

    StockKsPerPixelCameraConstants camera;
    StockKsPerPixelObjectConstants object;
    auto lighting = lighting_fixture();
    require(valid_stock_ks_per_pixel_camera_constants(camera) &&
                valid_stock_ks_per_pixel_object_constants(object) &&
                valid_stock_ks_per_pixel_lighting_constants(lighting),
            "finite native camera, object, and lighting records accepted");
    camera.projection[15U] = std::numeric_limits<float>::quiet_NaN();
    object.world[8U] = std::numeric_limits<float>::infinity();
    lighting.game_time = -std::numeric_limits<float>::infinity();
    require(!valid_stock_ks_per_pixel_camera_constants(camera) &&
                !valid_stock_ks_per_pixel_object_constants(object) &&
                !valid_stock_ks_per_pixel_lighting_constants(lighting),
            "non-finite native constant records rejected");

    std::array<apex::scene::Matrix4,
               stock_ks_per_pixel_shadow_cascade_count>
        matrices{};
    const std::array<float, stock_ks_per_pixel_shadow_cascade_count> biases = {
        0.000002F, 0.000015F, 0.0003F};
    const auto shadow = make_stock_directional_shadow_receiver_constants(
        matrices, biases, 2048U);
    require(valid_stock_directional_shadow_receiver_constants(shadow) &&
                shadow.texture_size == 1.0F / 2048.0F,
            "finite native shadow record accepted");
    const auto zero_width = make_stock_directional_shadow_receiver_constants(
        matrices, biases, 0U);
    require(!valid_stock_directional_shadow_receiver_constants(zero_width),
            "zero-width native shadow record rejected");
}

void transposes_native_host_matrices_before_upload() {
    const apex::scene::Matrix4 source = {
        1.0F, 2.0F, 3.0F, 4.0F,
        5.0F, 6.0F, 7.0F, 8.0F,
        9.0F, 10.0F, 11.0F, 12.0F,
        13.0F, 14.0F, 15.0F, 16.0F};
    const apex::scene::Matrix4 expected = {
        1.0F, 5.0F, 9.0F, 13.0F,
        2.0F, 6.0F, 10.0F, 14.0F,
        3.0F, 7.0F, 11.0F, 15.0F,
        4.0F, 8.0F, 12.0F, 16.0F};
    require(stock_ks_per_pixel_transpose_matrix(source) == expected,
            "native host matrix transpose");
    const auto camera = make_stock_ks_per_pixel_camera_constants(
        source, source, source, {17.0F, 18.0F, 19.0F}, 0.1F, 1000.0F,
        1.2F, 0.25F);
    require(camera.view == expected && camera.projection == expected &&
                camera.mvp_inverse == expected &&
                camera.camera_position ==
                    std::array<float, 3U>{17.0F, 18.0F, 19.0F} &&
                camera.near_plane == 0.1F && camera.far_plane == 1000.0F &&
                camera.field_of_view == 1.2F && camera.dof_factor == 0.25F,
            "native camera record uses transposed matrices and reflected offsets");
    const auto object = make_stock_ks_per_pixel_object_constants(source);
    require(object.world == expected,
            "native object record uses transposed world matrix");
}

StockKsPerPixelLightingConstants lighting_fixture() {
    StockKsPerPixelLightingConstants lighting;
    lighting.light_direction = {0.0F, -1.0F, 0.0F};
    lighting.ambient_color = {0.2F, 0.3F, 0.4F, 1.0F};
    lighting.light_color = {1.0F, 1.0F, 1.0F};
    lighting.horizon_color[3U] = 1.0F;
    lighting.zenith_color[3U] = 1.0F;
    lighting.fog_color = {0.4F, 0.2F, 0.1F};
    return lighting;
}

StockKsPerPixelMaterialConstants material_fixture() {
    StockKsPerPixelMaterialConstants material;
    material.ambient = 0.5F;
    material.diffuse = 0.8F;
    material.emissive = {0.1F, 0.2F, 0.3F};
    return material;
}

void evaluates_recovered_base_pixel_equation() {
    StockKsPerPixelPixelInput input;
    input.interpolated_normal = {0.0F, 1.0F, 0.0F};
    input.camera_to_surface = {0.0F, 0.0F, -1.0F};
    input.sampled_diffuse = {0.25F, 0.5F, 0.75F, 0.6F};
    input.fog_factor = 0.25F;
    const auto result = evaluate_stock_ks_per_pixel(
        input, lighting_fixture(), material_fixture(),
        StockKsPerPixelVariant::base);
    require(result.ok(), "recovered base pixel equation evaluates");
    require_near(result.rgba[0U], 0.2875F, "recovered red output");
    require_near(result.rgba[1U], 0.48125F, "recovered green output");
    require_near(result.rgba[2U], 0.75625F, "recovered blue output");
    require_near(result.rgba[3U], 0.6F, "recovered diffuse alpha output");
}

void retains_the_recovered_at_normalization_difference() {
    StockKsPerPixelPixelInput input;
    input.interpolated_normal = {0.0F, 0.5F, 0.0F};
    input.camera_to_surface = {0.0F, 0.0F, -1.0F};
    input.sampled_diffuse = {1.0F, 1.0F, 1.0F, 0.25F};
    const auto base = evaluate_stock_ks_per_pixel(
        input, lighting_fixture(), material_fixture(),
        StockKsPerPixelVariant::base);
    const auto at = evaluate_stock_ks_per_pixel(
        input, lighting_fixture(), material_fixture(),
        StockKsPerPixelVariant::alpha_to_coverage);
    require(base.ok() && at.ok(), "base and AT pixel equations evaluate");
    require(at.rgba[0U] > base.rgba[0U] &&
                at.rgba[1U] > base.rgba[1U] &&
                at.rgba[2U] > base.rgba[2U],
            "AT normal normalization changes recovered lighting");
    require_near(base.rgba[3U], 0.25F, "base diffuse alpha");
    require_near(at.rgba[3U], 0.25F, "AT diffuse alpha");
}

void rejects_unsafe_reference_inputs() {
    StockKsPerPixelPixelInput input;
    input.interpolated_normal = {0.0F, 1.0F, 0.0F};
    input.camera_to_surface = {0.0F, 0.0F, -1.0F};
    input.sampled_diffuse = {1.0F, 1.0F, 1.0F, 1.0F};
    auto lighting = lighting_fixture();
    const auto material = material_fixture();

    require(evaluate_stock_ks_per_pixel(
                input, lighting, material,
                static_cast<StockKsPerPixelVariant>(255U))
                .status == StockKsPerPixelEvaluationStatus::invalid_variant,
            "unknown pixel variant rejected");

    input.sampled_diffuse[0U] = std::numeric_limits<float>::quiet_NaN();
    require(evaluate_stock_ks_per_pixel(input, lighting, material,
                                        StockKsPerPixelVariant::base)
                .status == StockKsPerPixelEvaluationStatus::non_finite_input,
            "non-finite sampled color rejected");
    input.sampled_diffuse[0U] = 1.0F;

    input.shadow_factor = 1.01F;
    require(evaluate_stock_ks_per_pixel(input, lighting, material,
                                        StockKsPerPixelVariant::base)
                .status == StockKsPerPixelEvaluationStatus::factor_out_of_range,
            "shadow factor outside the sampled range rejected");
    input.shadow_factor = 1.0F;

    input.camera_to_surface = {};
    require(evaluate_stock_ks_per_pixel(input, lighting, material,
                                        StockKsPerPixelVariant::base)
                .status ==
                StockKsPerPixelEvaluationStatus::degenerate_view_direction,
            "degenerate view direction rejected");
    input.camera_to_surface = {0.0F, 0.0F, -1.0F};

    input.interpolated_normal = {};
    require(evaluate_stock_ks_per_pixel(
                input, lighting, material,
                StockKsPerPixelVariant::alpha_to_coverage)
                .status == StockKsPerPixelEvaluationStatus::degenerate_normal,
            "degenerate AT normal rejected");

    input.interpolated_normal = {0.0F, 1.0F, 0.0F};
    input.camera_to_surface = {0.0F, 1.0F, 0.0F};
    require(evaluate_stock_ks_per_pixel(input, lighting, material,
                                        StockKsPerPixelVariant::base)
                .status == StockKsPerPixelEvaluationStatus::degenerate_half_vector,
            "degenerate native half vector rejected");
}

void names_all_reference_statuses() {
    require(std::string_view(stock_ks_per_pixel_evaluation_status_name(
                StockKsPerPixelEvaluationStatus::ready)) == "ready",
            "ready evaluation status name");
    require(std::string_view(stock_ks_per_pixel_evaluation_status_name(
                static_cast<StockKsPerPixelEvaluationStatus>(255U))) == "unknown",
            "unknown evaluation status name");
    require(std::string_view(stock_ks_per_pixel_container_status_name(
                StockKsPerPixelContainerStatus::stage_size_mismatch)) ==
                "stage_size_mismatch",
            "container shape status name");
    require(std::string_view(stock_ks_per_pixel_container_status_name(
                static_cast<StockKsPerPixelContainerStatus>(255U))) ==
                "unknown",
            "unknown container shape status name");
    require(std::string_view(stock_ks_per_pixel_binding_status_name(
                StockKsPerPixelBindingStatus::vulkan_namespace_mismatch)) ==
                "vulkan_namespace_mismatch",
            "backend binding status name");
    require(std::string_view(stock_ks_per_pixel_binding_status_name(
                static_cast<StockKsPerPixelBindingStatus>(255U))) == "unknown",
            "unknown backend binding status name");
    require(std::string_view(stock_ks_per_pixel_reflection_status_name(
                StockKsPerPixelReflectionStatus::table_out_of_bounds)) ==
                "table_out_of_bounds",
            "RDEF reflection status name");
    require(std::string_view(stock_ks_per_pixel_reflection_status_name(
                static_cast<StockKsPerPixelReflectionStatus>(255U))) ==
                "unknown",
            "unknown RDEF reflection status name");
    require(std::string_view(stock_ks_per_pixel_signature_status_name(
                StockKsPerPixelSignatureStatus::invalid_name)) ==
                "invalid_name",
            "signature status name");
    require(std::string_view(stock_ks_per_pixel_signature_status_name(
                static_cast<StockKsPerPixelSignatureStatus>(255U))) ==
                "unknown",
            "unknown signature status name");
    require(std::string_view(stock_ks_per_pixel_native_program_status_name(
                StockKsPerPixelNativeProgramStatus::reflection_mismatch)) ==
                "reflection_mismatch",
            "native program status name");
    require(std::string_view(stock_ks_per_pixel_native_program_status_name(
                static_cast<StockKsPerPixelNativeProgramStatus>(255U))) ==
                "unknown",
            "unknown native program status name");
}

void classifies_native_bridge_failures() {
    require(
        classify_d3d12_stock_ks_per_pixel_failure(
            "d3d12_stock_native_geometry_range_invalid") ==
            D3D12StockKsPerPixelFailureKind::invalid_request,
        "native geometry preflight is an invalid request");
    require(
        classify_d3d12_stock_ks_per_pixel_failure(
            "indexed_stock_native_diffuse_uninitialized") ==
            D3D12StockKsPerPixelFailureKind::invalid_request,
        "native uninitialized texture is an invalid request");
    require(
        classify_d3d12_stock_ks_per_pixel_failure(
            "d3d12_stock_native_wireframe_unsupported") ==
            D3D12StockKsPerPixelFailureKind::unsupported,
        "native unsupported feature is not an execution failure");
    require(
        classify_d3d12_stock_ks_per_pixel_failure(
            "indexed_stock_native_context_mismatch") ==
            D3D12StockKsPerPixelFailureKind::unsupported,
        "native foreign context is unsupported");
    require(
        classify_d3d12_stock_ks_per_pixel_failure(
            "d3d12_stock_native_submit_failed") ==
            D3D12StockKsPerPixelFailureKind::execution_failed,
        "native submit failure remains an execution failure");
    require(
        classify_d3d12_stock_ks_per_pixel_failure(
            "d3d12_stock_native_batch_target_shape_invalid") ==
            D3D12StockKsPerPixelFailureKind::invalid_request,
        "native batch shape preflight is an invalid request");
    require(
        classify_d3d12_stock_ks_per_pixel_failure(
            "d3d12_stock_native_batch_pipeline_unsupported") ==
            D3D12StockKsPerPixelFailureKind::unsupported,
        "native batch unsupported pipeline is unsupported");
    require(
        classify_d3d12_stock_ks_per_pixel_failure(
            "d3d12_stock_native_batch_target_format_unsupported") ==
            D3D12StockKsPerPixelFailureKind::unsupported,
        "native batch unsupported target format is unsupported");
    require(
        classify_d3d12_stock_ks_per_pixel_failure(
            "d3d12_stock_native_batch_submit_undrained") ==
            D3D12StockKsPerPixelFailureKind::execution_failed,
        "native batch submit failure remains an execution failure");
    require(
        indexed_static_mesh_draw_status(
            D3D12StockKsPerPixelFailureKind::invalid_request) ==
            IndexedStaticMeshDrawStatus::invalid_request,
        "native draw failure maps to public invalid-request status");
    require(
        indexed_static_mesh_batch_status(
            D3D12StockKsPerPixelFailureKind::unsupported) ==
            IndexedStaticMeshBatchStatus::unsupported,
        "native batch failure maps to public unsupported status");
    require(
        classify_d3d12_stock_ks_per_pixel_failure("future_native_failure") ==
            D3D12StockKsPerPixelFailureKind::execution_failed,
        "unknown native failure stays conservatively executable failure");
}

} // namespace

int main() {
    try {
        exposes_exact_native_register_contract();
        projects_native_registers_without_namespace_collisions();
        exposes_recovered_vertex_and_sampler_contracts();
        rejects_malformed_backend_binding_layouts();
        validates_owned_container_shape_before_backend_use();
        validates_bounded_rdef_resource_contract();
        validates_exact_bounded_stage_signatures();
        gates_the_complete_native_program_before_backend_allocation();
        owns_the_validated_native_program_after_the_gate();
        clones_validated_native_programs_without_aliasing_mutable_state();
        loads_only_exact_validated_native_program_files();
        executes_a_validated_native_program_through_the_static_scene_batch();
        allocates_only_validated_native_d3d12_shader_objects();
        allocates_exact_native_d3d12_constant_buffer_views();
        allocates_exact_native_stock_samplers();
        adopts_native_draw_resources_with_one_stable_owner();
        validates_complete_native_draw_bindings_before_execution();
        preserves_native_constant_bytes_and_rejects_nonfinite_records();
        transposes_native_host_matrices_before_upload();
        evaluates_recovered_base_pixel_equation();
        retains_the_recovered_at_normalization_difference();
        rejects_unsafe_reference_inputs();
        names_all_reference_statuses();
        classifies_native_bridge_failures();
        std::cout << "stock ksPerPixel tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "stock ksPerPixel tests: " << error.what() << '\n';
        return 1;
    }
}
