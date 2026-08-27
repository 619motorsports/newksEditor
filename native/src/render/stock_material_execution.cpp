#include "apex/render/stock_material_execution.hpp"

#include "apex/render/pipeline.hpp"
#include "apex/render/stock_multimap_source.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace apex::render {
namespace {

[[nodiscard]] std::string canonical(std::string_view value) {
    std::size_t begin = 0U;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
        ++begin;
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0)
        --end;
    std::string result(value.substr(begin, end - begin));
    for (char& character : result)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

[[nodiscard]] bool supported_family(std::string_view shader) {
    const std::string key = canonical(shader);
    return key == "ksperpixel" || key == "ksperpixelat" ||
           key == "ksskinnedmesh" ||
           key == "ksperpixelnm" ||
           key == "ksperpixelmultimap" ||
           key == "ksperpixelmultimap_at" ||
           key == "ksperpixelmultimap_nmdetail" ||
           key == "ksperpixelmultimap_at_nmdetail" ||
           key == "ksperpixelmultimap_damage_dirt";
}

[[nodiscard]] bool multimap_reflection_family(std::string_view shader) {
    const std::string key = canonical(shader);
    return key == "ksperpixelmultimap" ||
           key == "ksperpixelmultimap_at" ||
           key == "ksperpixelmultimap_nmdetail" ||
           key == "ksperpixelmultimap_at_nmdetail";
}

[[nodiscard]] bool finite_material(const KsPerPixelMaterialConstants& value) {
    for (const auto* values : {&value.lighting, &value.fresnel, &value.emissive,
                               &value.detail, &value.damage_zones})
        for (const float component : *values)
            if (!std::isfinite(component)) return false;
    return true;
}

// Installed stock color and alpha-shadow programs both consume the material-
// owned 32-byte cbMaterial/b4 record. Repack the already source-evidenced
// resolver result into the color-path field names without changing bytes.
[[nodiscard]] StockKsPerPixelMaterialConstants native_material_constants(
    const StockShadowCasterMaterialConstants& value) noexcept {
    StockKsPerPixelMaterialConstants result;
    result.ambient = value.lighting[0U];
    result.diffuse = value.lighting[1U];
    result.specular = value.lighting[2U];
    result.specular_exponent = value.lighting[3U];
    result.emissive = {value.emissive_and_alpha_ref[0U],
                       value.emissive_and_alpha_ref[1U],
                       value.emissive_and_alpha_ref[2U]};
    result.alpha_reference = value.emissive_and_alpha_ref[3U];
    return result;
}

[[nodiscard]] Diagnostic diag(std::string code, std::string message) {
    return {std::move(code), std::move(message)};
}

[[nodiscard]] StockMaterialExecutionResult fail(StaticSceneResourceStatus status,
                                                 std::string code,
                                                 std::string message) {
    StockMaterialExecutionResult result;
    result.status = status;
    result.diagnostic = diag(std::move(code), std::move(message));
    return result;
}

[[nodiscard]] bool backend_shader_format(Backend backend,
                                         PipelineShaderFormat format) noexcept {
    return backend == Backend::Vulkan
               ? format == PipelineShaderFormat::spirv
               : format == PipelineShaderFormat::dxbc ||
                     format == PipelineShaderFormat::dxil;
}

[[nodiscard]] bool d3d12_native_scene_color_format(
    PipelineRenderTargetFormat format) noexcept {
    return format == PipelineRenderTargetFormat::rgba8_unorm ||
           format == PipelineRenderTargetFormat::rgba8_srgb ||
           format == PipelineRenderTargetFormat::bgra8_unorm ||
           format == PipelineRenderTargetFormat::bgra8_srgb ||
           format == PipelineRenderTargetFormat::rgba16_float;
}

[[nodiscard]] const StockMaterialShaderModules* find_modules(
    std::span<const StockMaterialShaderModules> sets, std::string_view material,
    std::string_view shader, StockMaterialShaderVariant variant,
    bool directional_shadow_receiver, bool multimap_reflection) {
    const std::string material_key = canonical(material);
    const std::string shader_key = canonical(shader);
    // A skinned packet has a distinct vertex ABI and 76-byte stride. Never
    // let a material-name override select bytecode authored for a static use
    // of the same source material.
    const bool shader_family_only = shader_key == "ksskinnedmesh";
    const StockMaterialShaderModules* family = nullptr;
    for (const StockMaterialShaderModules& set : sets) {
        if (set.variant != variant ||
            set.directional_shadow_receiver != directional_shadow_receiver ||
            set.multimap_reflection != multimap_reflection)
            continue;
        if (!shader_family_only && canonical(set.key) == material_key &&
            set.key_kind == StockMaterialShaderKeyKind::material_name)
            return &set;
        if (canonical(set.key) == shader_key &&
            set.key_kind == StockMaterialShaderKeyKind::shader_family)
            family = &set;
    }
    return family;
}

[[nodiscard]] const StockMaterialD3D12NativeProgram* find_d3d12_native_program(
    std::span<const StockMaterialD3D12NativeProgram> programs,
    std::string_view shader) {
    const std::string shader_key = canonical(shader);
    for (const StockMaterialD3D12NativeProgram& program : programs)
        if (canonical(program.key) == shader_key) return &program;
    return nullptr;
}

[[nodiscard]] bool validate_d3d12_native_programs(
    const StockMaterialExecutionRequest& request, Diagnostic& diagnostic) {
    if (request.builtin_d3d12_native ==
        BuiltinD3D12StockNativeSelector::disabled) {
        if (!request.builtin_d3d12_native_programs.empty()) {
            diagnostic = diag(
                "stock_material_d3d12_native_programs_unreferenced",
                "Installed D3D12 native programs require an enabled selector");
            return false;
        }
        return true;
    }
    const bool alpha_to_coverage_only =
        request.builtin_d3d12_native ==
        BuiltinD3D12StockNativeSelector::ks_per_pixel_alpha_to_coverage;
    const bool base_and_alpha_to_coverage =
        request.builtin_d3d12_native ==
        BuiltinD3D12StockNativeSelector::
            ks_per_pixel_base_and_alpha_to_coverage;
    if (request.builtin_d3d12_native !=
            BuiltinD3D12StockNativeSelector::ks_per_pixel_base &&
        !alpha_to_coverage_only && !base_and_alpha_to_coverage) {
        diagnostic = diag(
            "stock_material_d3d12_native_selector_invalid",
            "The installed D3D12 native selector is invalid");
        return false;
    }
    const std::size_t expected_program_count =
        base_and_alpha_to_coverage ? 2U : 1U;
    if (request.builtin_d3d12_native_programs.size() !=
            expected_program_count ||
        request.builtin_d3d12_native_programs.size() >
            request.limits.max_shader_sets) {
        diagnostic = diag(
            "stock_material_d3d12_native_program_count_invalid",
            base_and_alpha_to_coverage
                ? "The combined D3D12 native selector requires one validated base and one validated alpha-to-coverage program"
                : alpha_to_coverage_only
                ? "The alpha-to-coverage D3D12 native selector requires one bounded validated program"
                : "The base D3D12 native selector requires one bounded validated program");
        return false;
    }
    std::set<std::string> keys;
    for (const StockMaterialD3D12NativeProgram& entry :
         request.builtin_d3d12_native_programs) {
        const std::string key = canonical(entry.key);
        const bool entry_alpha_to_coverage = key == "ksperpixelat";
        const bool key_allowed =
            key == "ksperpixel" ||
            (entry_alpha_to_coverage &&
             (alpha_to_coverage_only || base_and_alpha_to_coverage));
        const bool selector_allows_key =
            base_and_alpha_to_coverage ||
            (alpha_to_coverage_only == entry_alpha_to_coverage);
        const StockKsPerPixelVariant expected_variant =
            entry_alpha_to_coverage
                ? StockKsPerPixelVariant::alpha_to_coverage
                : StockKsPerPixelVariant::base;
        if (entry.key.empty() ||
            entry.key.size() > request.limits.max_shader_key_bytes ||
            !key_allowed || !selector_allows_key ||
            !keys.insert(key).second ||
            entry.program == nullptr ||
            entry.program->validation_status() !=
                StockKsPerPixelNativeProgramStatus::ready ||
            entry.program->variant() != expected_variant) {
            diagnostic = diag(
                "stock_material_d3d12_native_program_invalid",
                base_and_alpha_to_coverage
                    ? "The combined D3D12 native selector requires exact validated ksPerPixel base and ksPerPixelAT alpha-to-coverage programs"
                    : alpha_to_coverage_only
                    ? "The installed D3D12 native selector requires one exact validated ksPerPixelAT alpha-to-coverage program"
                    : "The first D3D12 native slice accepts one exact validated base ksPerPixel program");
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::vector<PipelineResourceBinding> resources_for(
    std::string_view shader, bool include_damage_dust = false,
    bool directional_shadow_receiver = false,
    bool multimap_reflection = false) {
    const std::string key = canonical(shader);
    std::vector<PipelineResourceBinding> resources;
    const auto add = [&](PipelineResourceKind kind, std::uint32_t binding,
                         std::string name) {
        resources.push_back({kind, 0U, binding, std::move(name)});
    };
    add(PipelineResourceKind::sampled_texture, 0U, "txDiffuse");
    add(PipelineResourceKind::sampler, 1U, "txDiffuseSampler");
    add(PipelineResourceKind::uniform_buffer, 2U, "ksPerPixelMaterial");
    add(PipelineResourceKind::uniform_buffer, 3U, "ksPerPixelFrame");
    if (key == "ksperpixelnm") {
        add(PipelineResourceKind::sampled_texture, 4U, "txNormal");
        add(PipelineResourceKind::sampler, 5U, "txNormalSampler");
    } else if (key == "ksperpixelmultimap" ||
               key == "ksperpixelmultimap_at") {
        add(PipelineResourceKind::sampled_texture, 4U, "txNormal");
        add(PipelineResourceKind::sampler, 5U, "txNormalSampler");
        add(PipelineResourceKind::sampled_texture, 6U, "txMaps");
        add(PipelineResourceKind::sampler, 7U, "txMapsSampler");
    } else if (key == "ksperpixelmultimap_nmdetail" ||
               key == "ksperpixelmultimap_at_nmdetail") {
        add(PipelineResourceKind::sampled_texture, 4U, "txNormal");
        add(PipelineResourceKind::sampler, 5U, "txNormalSampler");
        add(PipelineResourceKind::sampled_texture, 6U, "txMaps");
        add(PipelineResourceKind::sampler, 7U, "txMapsSampler");
        add(PipelineResourceKind::sampled_texture, 8U, "txDetail");
        add(PipelineResourceKind::sampler, 9U, "txDetailSampler");
        add(PipelineResourceKind::sampled_texture, 10U, "txNormalDetail");
        add(PipelineResourceKind::sampler, 11U, "txNormalDetailSampler");
    } else if (key == "ksperpixelmultimap_damage_dirt") {
        add(PipelineResourceKind::sampled_texture, 4U, "txNormal");
        add(PipelineResourceKind::sampler, 5U, "txNormalSampler");
        add(PipelineResourceKind::sampled_texture, 6U, "txMaps");
        add(PipelineResourceKind::sampler, 7U, "txMapsSampler");
        if (include_damage_dust) {
            add(PipelineResourceKind::sampled_texture, 8U, "txDust");
            add(PipelineResourceKind::sampler, 9U, "txDustSampler");
        }
        add(PipelineResourceKind::sampled_texture, 12U, "txDamage");
        add(PipelineResourceKind::sampler, 13U, "txDamageSampler");
        add(PipelineResourceKind::sampled_texture, 14U, "txDamageMask");
        add(PipelineResourceKind::sampler, 15U, "txDamageMaskSampler");
    }
    if (directional_shadow_receiver) {
        add(PipelineResourceKind::sampled_texture, 16U, "txShadow0");
        add(PipelineResourceKind::sampled_texture, 17U, "txShadow1");
        add(PipelineResourceKind::sampled_texture, 18U, "txShadow2");
        add(PipelineResourceKind::sampler, 19U, "shadowSampler");
        add(PipelineResourceKind::uniform_buffer, 20U, "shadowReceiver");
    }
    if (multimap_reflection) {
        add(PipelineResourceKind::sampled_texture, 21U, "txCube");
        add(PipelineResourceKind::sampler, 22U, "txCubeSampler");
        add(PipelineResourceKind::uniform_buffer, 23U,
            "ksPerPixelMultiMapReflection");
    }
    return resources;
}

[[nodiscard]] std::size_t expected_texture_slots(std::string_view shader) {
    const std::string key = canonical(shader);
    if (key == "ksperpixel" || key == "ksperpixelat" ||
        key == "ksskinnedmesh") return 1U;
    if (key == "ksperpixelnm") return 2U;
    if (key == "ksperpixelmultimap" || key == "ksperpixelmultimap_at") return 3U;
    return 5U;
}

bool validate_module_sets(std::span<const StockMaterialShaderModules> sets,
                          const StockMaterialExecutionLimits& limits, Backend backend,
                          Diagnostic& diagnostic) {
    if (sets.size() > limits.max_shader_sets) {
        diagnostic = diag("stock_material_shader_set_limit", "Shader module set count exceeds the configured limit");
        return false;
    }
    std::set<std::tuple<int, std::string, int, bool, bool>> keys;
    for (const StockMaterialShaderModules& set : sets) {
        if (set.key.empty() || set.key.size() > limits.max_shader_key_bytes) {
            diagnostic = diag("stock_material_shader_key_invalid", "Shader module key is empty or exceeds the configured limit");
            return false;
        }
        if (!keys.insert({static_cast<int>(set.key_kind), canonical(set.key),
                          static_cast<int>(set.variant),
                          set.directional_shadow_receiver,
                          set.multimap_reflection}).second) {
            diagnostic = diag("stock_material_shader_key_duplicate", "Shader module keys must be unique");
            return false;
        }
        if (set.multimap_reflection &&
            set.key_kind == StockMaterialShaderKeyKind::shader_family &&
            !multimap_reflection_family(set.key)) {
            diagnostic = diag(
                "stock_material_multimap_reflection_family_invalid",
                "Portable MultiMap reflection modules require one of the four bounded MultiMap shader-family keys");
            return false;
        }
        if (set.variant == StockMaterialShaderVariant::damage_dust &&
            set.key_kind == StockMaterialShaderKeyKind::shader_family &&
            canonical(set.key) != "ksperpixelmultimap_damage_dirt") {
            diagnostic = diag("stock_material_shader_variant_invalid",
                              "The damage-dust shader variant requires the damage-dirt shader family");
            return false;
        }
        if (set.modules.size() != 2U) {
            diagnostic = diag("stock_material_shader_modules_incomplete", "A production material requires exactly vertex and fragment modules");
            return false;
        }
        bool vertex = false;
        bool fragment = false;
        for (const PipelineShaderModule& module : set.modules) {
            if (!backend_shader_format(backend, module.format)) {
                diagnostic = diag("stock_material_shader_module_format", "Shader module format does not match the preparing backend");
                return false;
            }
            if (module.bytes.empty()) {
                diagnostic = diag("stock_material_shader_module_empty", "Shader module bytecode is empty");
                return false;
            }
            if (module.bytes.size() > limits.scene.pipeline.max_shader_module_bytes) {
                diagnostic = diag("stock_material_shader_module_limit", "Shader module exceeds the configured byte limit");
                return false;
            }
            vertex = vertex || module.stage == PipelineShaderStage::vertex;
            fragment = fragment || module.stage == PipelineShaderStage::fragment;
            if (module.stage == PipelineShaderStage::geometry) {
                diagnostic = diag("stock_material_shader_stage_unsupported", "Geometry shader modules are not supported by this handoff");
                return false;
            }
        }
        if (!vertex || !fragment) {
            diagnostic = diag("stock_material_shader_modules_incomplete", "Shader module set must contain vertex and fragment stages");
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool packet_resources_match(const DrawPacket& packet, std::string_view shader,
                                          std::size_t max_string, Diagnostic& diagnostic) {
    const std::size_t expected = expected_texture_slots(shader);
    const bool damage = canonical(shader) == "ksperpixelmultimap_damage_dirt";
    if ((!damage && packet.resources.size() != expected) ||
        (damage && packet.resources.size() != expected &&
         packet.resources.size() != expected + 1U)) {
        diagnostic = diag("stock_material_resources_incomplete", "Draw packet resources do not contain the complete supported stock texture set");
        return false;
    }
    std::set<std::string> seen;
    for (const DrawResourceSlot& resource : packet.resources) {
        const std::string key = canonical(resource.slot);
        if (resource.slot.size() > max_string || resource.texture.size() > max_string ||
            resource.texture.empty()) {
            diagnostic = diag("stock_material_resource_string_invalid", "Draw packet resource names exceed the configured limit or are empty");
            return false;
        }
        if (!seen.insert(key).second) {
            diagnostic = diag("stock_material_resource_duplicate", "Draw packet resource slots must be unique");
            return false;
        }
        const bool allowed = damage
                                 ? key == "txdiffuse" || key == "txnormal" || key == "txmaps" ||
                                       key == "txdamage" || key == "txdamagemask" || key == "txdust"
                                 : key == "txdiffuse" || key == "txnormal" || key == "txmaps" ||
                                       key == "txdetail" || key == "txnormaldetail" || key == "txdetailnm";
        if (!allowed) {
            diagnostic = diag("stock_material_resource_unsupported", "Draw packet contains a resource outside the supported stock family");
            return false;
        }
    }
    const bool missing_diffuse = seen.count("txdiffuse") == 0U;
    const bool missing_normal = expected >= 2U && seen.count("txnormal") == 0U;
    const bool missing_maps = expected >= 3U && seen.count("txmaps") == 0U;
    const bool missing_detail = expected == 5U && !damage &&
                                (seen.count("txdetail") == 0U ||
                                 (seen.count("txnormaldetail") == 0U &&
                                  seen.count("txdetailnm") == 0U));
    const bool missing_damage = damage &&
                                (seen.count("txdamage") == 0U ||
                                 seen.count("txdamagemask") == 0U);
    const bool dust_count_mismatch = damage &&
                                     ((packet.resources.size() == expected + 1U &&
                                       seen.count("txdust") == 0U) ||
                                      (packet.resources.size() == expected &&
                                       seen.count("txdust") != 0U));
    if (missing_diffuse || missing_normal || missing_maps || missing_detail || missing_damage ||
        dust_count_mismatch) {
        diagnostic = diag("stock_material_resources_incomplete", "Draw packet is missing a required stock texture role");
        return false;
    }
    return true;
}

[[nodiscard]] MaterialOverride state_override(const MaterialBindingOverrides* overrides) {
    MaterialOverride result;
    if (overrides == nullptr) return result;
    result.shader = overrides->shader;
    result.blend_mode = overrides->blend_mode;
    result.depth_mode = overrides->depth_mode;
    result.cull_mode = overrides->cull_mode;
    result.is_transparent = overrides->is_transparent;
    return result;
}

bool charge(std::uint64_t amount, std::uint64_t& total, std::uint64_t limit) {
    if (amount > limit - std::min(total, limit)) return false;
    total += amount;
    return true;
}

bool estimate_adapter_copy(const StockMaterialExecutionRequest& request,
                           Diagnostic& diagnostic) {
    const std::uint64_t limit = request.limits.scene.max_preparation_bytes;
    std::uint64_t total = 0U;
    const auto string_bytes = [&](std::string_view value) {
        return static_cast<std::uint64_t>(value.size());
    };
    const auto charge_count = [&](std::size_t count, std::size_t element_size) {
        if (element_size != 0U && count > std::numeric_limits<std::uint64_t>::max() / element_size)
            return false;
        return charge(static_cast<std::uint64_t>(count) * element_size, total, limit);
    };
    const auto charge_packet = [&](const DrawPacket& packet) {
        if (!charge(sizeof(DrawPacket), total, limit) ||
            !charge_count(packet.resources.size(), sizeof(DrawResourceSlot)) ||
            !charge_count(packet.bone_palette.size(), sizeof(apex::scene::Matrix4)))
            return false;
        for (const DrawResourceSlot& resource : packet.resources)
            if (!charge(string_bytes(resource.slot) + string_bytes(resource.texture), total, limit))
                return false;
        const MaterialRenderProfile& profile = packet.material_profile;
        for (const std::string_view value : {std::string_view(profile.shader),
                                             std::string_view(profile.blend_source),
                                             std::string_view(profile.blend),
                                             std::string_view(profile.blend_mode),
                                             std::string_view(profile.depth_mode),
                                             std::string_view(profile.cull_source)})
            if (!charge(string_bytes(value), total, limit)) return false;
        return true;
    };
    for (const DrawPacket& packet : request.packets) {
        if (!charge_packet(packet)) {
            diagnostic = diag("stock_material_preparation_limit",
                              "Copied packet metadata exceeds the adapter preparation limit");
            return false;
        }
    }
    std::uint64_t largest_module_set_bytes = 0U;
    for (const StockMaterialShaderModules& set : request.shader_modules) {
        if (!charge(sizeof(StockMaterialShaderModules) + string_bytes(set.key) +
                        4U * sizeof(void*),
                    total, limit) ||
            !charge_count(set.modules.size(), sizeof(PipelineShaderModule))) {
            diagnostic = diag("stock_material_preparation_limit",
                              "Copied shader-module metadata exceeds the adapter preparation limit");
            return false;
        }
        std::uint64_t module_set_bytes = 0U;
        for (const PipelineShaderModule& module : set.modules) {
            if (module.bytes.size() >
                std::numeric_limits<std::uint64_t>::max() - module_set_bytes) {
                diagnostic = diag("stock_material_preparation_limit",
                                  "Copied shader bytecode exceeds the adapter preparation limit");
                return false;
            }
            module_set_bytes += static_cast<std::uint64_t>(module.bytes.size());
        }
        largest_module_set_bytes = std::max(largest_module_set_bytes,
                                            module_set_bytes);
    }
    const std::uint64_t copied_pipeline_count = request.packets.size();
    if (copied_pipeline_count >
        (std::numeric_limits<std::uint64_t>::max() - 1U) / 2U) {
        diagnostic = diag("stock_material_preparation_limit",
                          "Copied shader bytecode exceeds the adapter preparation limit");
        return false;
    }
    const std::uint64_t peak_pipeline_copy_count = copied_pipeline_count * 2U + 1U;
    if (peak_pipeline_copy_count != 0U &&
        largest_module_set_bytes >
            std::numeric_limits<std::uint64_t>::max() / peak_pipeline_copy_count) {
        diagnostic = diag("stock_material_preparation_limit",
                          "Copied shader bytecode exceeds the adapter preparation limit");
        return false;
    }
    if (!charge(largest_module_set_bytes * peak_pipeline_copy_count, total, limit)) {
        diagnostic = diag("stock_material_preparation_limit",
                          "Copied shader bytecode exceeds the adapter preparation limit");
        return false;
    }
    if (request.builtin_vulkan_source ==
        BuiltinVulkanStockSourceSelector::ks_per_pixel) {
        const std::uint64_t material_count = request.model->materials.size();
        if (material_count > std::numeric_limits<std::uint64_t>::max() / 2U) {
            diagnostic = diag("stock_material_preparation_limit",
                              "Built-in source owner count exceeds the preparation limit");
            return false;
        }
        const std::uint64_t owner_count = std::min(
            static_cast<std::uint64_t>(request.packets.size()),
            material_count * 2U);
        const std::uint64_t owner_shader_bytes = std::max(
            static_cast<std::uint64_t>(
                stock_ks_per_pixel_vulkan_source_shader_bytes(
                    StockKsPerPixelVariant::base)),
            static_cast<std::uint64_t>(
                stock_ks_per_pixel_vulkan_source_shader_bytes(
                    StockKsPerPixelVariant::alpha_to_coverage)));
        if (owner_count > std::numeric_limits<std::uint64_t>::max() - 2U ||
            (owner_count + 2U != 0U &&
             owner_shader_bytes >
                 std::numeric_limits<std::uint64_t>::max() /
                     (owner_count + 2U))) {
            diagnostic = diag("stock_material_preparation_limit",
                              "Built-in source shader copies exceed the preparation limit");
            return false;
        }
        // Retained owners coexist transiently with the profile-building seed
        // and the final immutable factory result.
        if (!charge((owner_count + 2U) * owner_shader_bytes, total, limit) ||
            !charge_count(request.packets.size(), sizeof(std::uint32_t)) ||
            !charge_count(request.model->materials.size(),
                          sizeof(StockKsPerPixelMaterialConstants) +
                              sizeof(bool))) {
            diagnostic = diag("stock_material_preparation_limit",
                              "Built-in source metadata exceeds the preparation limit");
            return false;
        }
    }
    if (request.builtin_multimap_source ==
        BuiltinStockMultiMapSourceSelector::normal_detail) {
        const std::uint64_t owner_shader_bytes = std::max(
            static_cast<std::uint64_t>(stock_multimap_source_shader_bytes(
                Backend::Vulkan,
                StockMultiMapSourceVariant::alpha_to_coverage_normal_detail)),
            static_cast<std::uint64_t>(stock_multimap_source_shader_bytes(
                Backend::D3D12,
                StockMultiMapSourceVariant::alpha_to_coverage_normal_detail)));
        const std::uint64_t copies = peak_pipeline_copy_count + 2U;
        if (copies != 0U &&
            owner_shader_bytes >
                std::numeric_limits<std::uint64_t>::max() / copies) {
            diagnostic = diag(
                "stock_material_preparation_limit",
                "Built-in MultiMap shader copies exceed the preparation limit");
            return false;
        }
        if (!charge(owner_shader_bytes * copies, total, limit)) {
            diagnostic = diag(
                "stock_material_preparation_limit",
                "Built-in MultiMap shader copies exceed the preparation limit");
            return false;
        }
    }
    if (request.builtin_d3d12_native !=
        BuiltinD3D12StockNativeSelector::disabled) {
        if (!charge_count(request.builtin_d3d12_native_programs.size(),
                          sizeof(StockMaterialD3D12NativeProgram)) ||
            !charge_count(request.packets.size(), sizeof(std::uint32_t))) {
            diagnostic = diag("stock_material_preparation_limit",
                              "D3D12 native owner metadata exceeds the preparation limit");
            return false;
        }
        for (const StockMaterialD3D12NativeProgram& entry :
             request.builtin_d3d12_native_programs) {
            if (!charge(string_bytes(entry.key), total, limit)) {
                diagnostic = diag("stock_material_preparation_limit",
                                  "D3D12 native shader keys exceed the preparation limit");
                return false;
            }
            if (entry.program == nullptr) continue;
            const std::uint64_t shader_bytes =
                static_cast<std::uint64_t>(entry.program->vertex_shader().size()) +
                static_cast<std::uint64_t>(entry.program->pixel_shader().size());
            const std::uint64_t copies =
                static_cast<std::uint64_t>(request.packets.size()) + 2U;
            if ((copies != 0U &&
                 shader_bytes >
                     std::numeric_limits<std::uint64_t>::max() / copies) ||
                !charge(shader_bytes * copies, total, limit)) {
                diagnostic = diag("stock_material_preparation_limit",
                                  "D3D12 native shader copies exceed the preparation limit");
                return false;
            }
        }
    }
    if (!charge_count(request.model->materials.size(), sizeof(formats::Kn5Material)) ||
        !charge_count(request.packets.size(), sizeof(PipelineProgram)) ||
        !charge_count(request.packets.size(), sizeof(MaterialRenderProfile)) ||
        !charge_count(request.model->materials.size(), sizeof(KsPerPixelMaterialConstants)) ||
        (request.multimap_reflection &&
         !charge_count(request.model->materials.size(),
                       sizeof(KsPerPixelMultiMapReflectionConstants))) ||
        !charge_count(request.model->materials.size(),
                      sizeof(StockShadowCasterMaterialConstants)) ||
        !charge_count(request.model->materials.size(), sizeof(bool) + sizeof(std::size_t) +
                                                           2U * sizeof(std::size_t)) ||
        !charge_count(request.packets.size(), sizeof(const PipelineProgram*))) {
        diagnostic = diag("stock_material_preparation_limit",
                          "Copied material and pipeline tables exceed the adapter preparation limit");
        return false;
    }
    for (const formats::Kn5Material& material : request.model->materials) {
        if (!charge(string_bytes(material.name) + string_bytes(material.shader), total, limit)) {
            diagnostic = diag("stock_material_preparation_limit",
                              "Copied material strings exceed the adapter preparation limit");
            return false;
        }
        for (const formats::Kn5MaterialResource& resource : material.resources)
            if (!charge(string_bytes(resource.slot) + string_bytes(resource.texture) +
                        sizeof(resource), total, limit)) {
                diagnostic = diag("stock_material_preparation_limit",
                                  "Copied material resources exceed the adapter preparation limit");
                return false;
            }
        for (const formats::Kn5MaterialProperty& property : material.properties)
            if (!charge(string_bytes(property.name) + sizeof(property), total, limit)) {
                diagnostic = diag("stock_material_preparation_limit",
                                  "Copied material properties exceed the adapter preparation limit");
                return false;
            }
    }
    const std::uint64_t resource_pipeline_count = std::max(
        static_cast<std::uint64_t>(request.model->materials.size()),
        peak_pipeline_copy_count);
    // Reserve the base twelve declarations plus each selected orthogonal
    // extension: five directional-shadow declarations and three reflection
    // declarations.
    const std::size_t max_resource_declarations =
        12U + (request.directional_shadow_receiver ? 5U : 0U) +
        (request.multimap_reflection ? 3U : 0U);
    if (resource_pipeline_count >
            std::numeric_limits<std::size_t>::max() / max_resource_declarations ||
        !charge_count(static_cast<std::size_t>(resource_pipeline_count) *
                          max_resource_declarations,
                      sizeof(PipelineResourceBinding))) {
        diagnostic = diag("stock_material_preparation_limit",
                          "Copied pipeline resource declarations exceed the adapter preparation limit");
        return false;
    }
    for (const MaterialBindingOverrides& overrides : request.overrides_by_material) {
        if (!charge(sizeof(MaterialBindingOverrides), total, limit)) {
            diagnostic = diag("stock_material_preparation_limit",
                              "Copied material overrides exceed the adapter preparation limit");
            return false;
        }
        const auto override_limit = [&]() {
            diagnostic = diag("stock_material_preparation_limit",
                              "Copied material overrides exceed the adapter preparation limit");
            return false;
        };
        if (overrides.shader && !charge(string_bytes(*overrides.shader), total, limit)) return override_limit();
        if (overrides.blend_mode && !charge(string_bytes(*overrides.blend_mode), total, limit)) return override_limit();
        if (overrides.depth_mode && !charge(string_bytes(*overrides.depth_mode), total, limit)) return override_limit();
        if (overrides.cull_mode && !charge(string_bytes(*overrides.cull_mode), total, limit)) return override_limit();
        for (const auto& [name, value] : overrides.properties)
            if (!charge(string_bytes(name) + sizeof(value), total, limit)) return override_limit();
        for (const auto& [name, value] : overrides.resources)
            if (!charge(string_bytes(name) + string_bytes(value.texture) + string_bytes(value.file) + sizeof(value), total, limit)) return override_limit();
    }
    if (total > limit) {
        diagnostic = diag("stock_material_preparation_limit",
                          "Adapter-owned preparation metadata exceeds the configured limit");
        return false;
    }
    return true;
}

} // namespace

StockMaterialExecutionResult prepare_stock_material_execution(
    Device& device, const StockMaterialExecutionRequest& request) {
    try {
        if (request.model == nullptr || request.scene == nullptr)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_material_scene_missing", "A validated KN5 model and scene are required");
        if (request.packets.empty() || request.packets.size() > request.limits.scene.max_draws)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_material_packet_count_invalid", "The packet table is empty or exceeds the configured limit");
        if (request.model->materials.empty() || request.model->materials.size() > request.limits.scene.max_materials)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_material_material_count_invalid", "The model material table is empty or exceeds the configured limit");
        if (!request.overrides_by_material.empty() &&
            request.overrides_by_material.size() != request.model->materials.size())
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_material_override_table_invalid", "Material overrides must match the final KN5 material table");
        if (request.builtin_vulkan_source !=
                BuiltinVulkanStockSourceSelector::disabled &&
            request.builtin_vulkan_source !=
                BuiltinVulkanStockSourceSelector::ks_per_pixel)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_material_builtin_source_selector_invalid",
                        "The built-in Vulkan stock source selector is invalid");
        if (request.builtin_multimap_source !=
                BuiltinStockMultiMapSourceSelector::disabled &&
            request.builtin_multimap_source !=
                BuiltinStockMultiMapSourceSelector::normal_detail)
            return fail(
                StaticSceneResourceStatus::invalid_request,
                "stock_material_multimap_source_selector_invalid",
                "The built-in MultiMap source selector is invalid");
        if (request.builtin_vulkan_source !=
                BuiltinVulkanStockSourceSelector::disabled &&
            request.builtin_d3d12_native !=
                BuiltinD3D12StockNativeSelector::disabled)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_material_native_selector_conflict",
                        "Vulkan source and installed D3D12 native selectors are mutually exclusive");
        if (request.builtin_d3d12_native !=
                BuiltinD3D12StockNativeSelector::disabled &&
            device.info().backend != Backend::D3D12)
            return fail(StaticSceneResourceStatus::unsupported,
                        "stock_material_d3d12_native_backend_unsupported",
                        "Installed native ksPerPixel packages require D3D12");
        if (request.shader_modules.size() > request.limits.max_shader_sets)
            return fail(StaticSceneResourceStatus::invalid_request,
                        "stock_material_shader_set_limit", "Shader module set count exceeds the configured limit");
        Diagnostic module_diagnostic;
        if (!validate_d3d12_native_programs(request, module_diagnostic))
            return {StaticSceneResourceStatus::invalid_request,
                    std::move(module_diagnostic), nullptr};
        if (!estimate_adapter_copy(request, module_diagnostic))
            return {StaticSceneResourceStatus::invalid_request, std::move(module_diagnostic), nullptr};
        if (!validate_module_sets(request.shader_modules, request.limits, device.info().backend,
                                  module_diagnostic))
            return {StaticSceneResourceStatus::invalid_request, std::move(module_diagnostic), nullptr};

        std::vector<DrawPacket> packets(request.packets.begin(), request.packets.end());
        std::vector<PipelineProgram> pipelines;
        pipelines.reserve(packets.size());
        std::vector<MaterialRenderProfile> pipeline_profiles;
        pipeline_profiles.reserve(packets.size());
        std::vector<const PipelineProgram*> pipeline_ptrs(packets.size(), nullptr);
        std::vector<std::shared_ptr<const
            ValidatedStockKsPerPixelVulkanSourceProgram>> source_programs;
        source_programs.reserve(std::min(
            packets.size(), request.model->materials.size() * 2U));
        std::vector<MaterialRenderProfile> source_pipeline_profiles;
        source_pipeline_profiles.reserve(source_programs.capacity());
        std::vector<std::uint32_t> source_program_by_packet(
            packets.size(), invalid_static_scene_source_program_index);
        std::vector<std::shared_ptr<const
            ValidatedStockKsPerPixelNativeProgram>> d3d12_native_programs;
        std::vector<std::uint32_t> d3d12_native_program_by_packet(
            packets.size(), invalid_static_scene_source_program_index);
        std::vector<KsPerPixelMaterialConstants> constants(request.model->materials.size());
        std::vector<KsPerPixelMultiMapReflectionConstants>
            multimap_reflection_constants(request.model->materials.size());
        std::vector<StockKsPerPixelMaterialConstants> native_constants(
            request.model->materials.size());
        std::vector<bool> native_constants_ready(
            request.model->materials.size(), false);
        std::vector<StockShadowCasterMaterialConstants> shadow_constants(
            request.model->materials.size());
        bool has_alpha_shadow_constants = false;
        std::vector<bool> used(request.model->materials.size(), false);
        std::vector<std::size_t> first_packet(request.model->materials.size(), 0U);
        std::vector<std::array<std::size_t, 2U>> pipeline_indices(
            request.model->materials.size(),
            {std::numeric_limits<std::size_t>::max(),
             std::numeric_limits<std::size_t>::max()});
        std::vector<std::array<std::size_t, 2U>> source_pipeline_indices(
            request.model->materials.size(),
            {std::numeric_limits<std::size_t>::max(),
             std::numeric_limits<std::size_t>::max()});

        for (std::size_t packet_index = 0U; packet_index < packets.size(); ++packet_index) {
            DrawPacket& packet = packets[packet_index];
            if (packet.material >= request.model->materials.size() ||
                request.scene->find_node(packet.node) == nullptr)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "stock_material_packet_reference_invalid", "A packet references an unknown material or scene node");
            const std::size_t material_index = packet.material;
            if (!used[material_index]) {
                used[material_index] = true;
                first_packet[material_index] = packet_index;
            }
        }

        for (std::size_t material_index = 0U; material_index < request.model->materials.size(); ++material_index) {
            if (!used[material_index]) continue;
            const formats::Kn5Material& source = request.model->materials[material_index];
            const MaterialBindingOverrides* overrides = request.overrides_by_material.empty()
                                                            ? nullptr
                                                            : &request.overrides_by_material[material_index];
            const DrawPacket& representative = packets[first_packet[material_index]];
            const apex::scene::SceneNode* node = request.scene->find_node(representative.node);
            const MaterialBinding binding = build_material_binding(
                source, node != nullptr && node->transparent, request.model->textures.size(),
                overrides, request.limits.material);
            const std::string shader_key = canonical(binding.shader);
            const bool reflection_enabled =
                request.multimap_reflection &&
                multimap_reflection_family(binding.shader);
            if (binding.status != MaterialBindingStatus::complete)
                return fail(StaticSceneResourceStatus::invalid_request,
                            "stock_material_binding_incomplete", "The material binding is incomplete or unsupported");
            if (!supported_family(binding.shader))
                return fail(StaticSceneResourceStatus::unsupported,
                            "stock_material_family_unsupported", "The material shader family is outside the bounded production handoff");
            if (overrides != nullptr && !overrides->resources.empty())
                return fail(StaticSceneResourceStatus::unsupported,
                            "stock_material_texture_override_unsupported", "Texture overrides require an explicit caller texture authority and are not silently translated");

            const KsPerPixelMaterialResolveResult resolved =
                resolve_ks_per_pixel_material_constants(binding,
                    {false, shader_key == "ksperpixelat" ||
                                shader_key == "ksperpixelmultimap_at" ||
                                shader_key ==
                                    "ksperpixelmultimap_at_nmdetail",
                     reflection_enabled});
            if (!resolved.ok()) {
                const StaticSceneResourceStatus status =
                    resolved.status == KsPerPixelMaterialResolveStatus::unsupported
                        ? StaticSceneResourceStatus::unsupported
                        : StaticSceneResourceStatus::invalid_request;
                return fail(status,
                            resolved.diagnostic.code.empty() ? "stock_material_constants_invalid"
                                                              : resolved.diagnostic.code,
                            resolved.diagnostic.message.empty()
                                ? "The bounded material constant resolver rejected the material"
                                : resolved.diagnostic.message);
            }
            if (!finite_material(resolved.constants))
                return fail(StaticSceneResourceStatus::invalid_request,
                            "stock_material_constants_invalid",
                            "The bounded material constant resolver produced non-finite values");
            constants[material_index] = resolved.constants;
            if (reflection_enabled) {
                const KsPerPixelMultiMapReflectionResolveResult
                    reflection_resolved =
                        resolve_ks_per_pixel_multimap_reflection_constants(
                            binding);
                if (!reflection_resolved.ok()) {
                    const StaticSceneResourceStatus status =
                        reflection_resolved.status ==
                                KsPerPixelMultiMapReflectionResolveStatus::unsupported
                            ? StaticSceneResourceStatus::unsupported
                            : StaticSceneResourceStatus::invalid_request;
                    return fail(
                        status,
                        reflection_resolved.diagnostic.code.empty()
                            ? "stock_material_multimap_reflection_constants_invalid"
                            : reflection_resolved.diagnostic.code,
                        reflection_resolved.diagnostic.message.empty()
                            ? "The bounded MultiMap reflection resolver rejected the material"
                            : reflection_resolved.diagnostic.message);
                }
                multimap_reflection_constants[material_index] =
                    reflection_resolved.constants;
            }
            bool material_needs_alpha_shadow = false;

            if (source.serializedBlendMode > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
                source.depthMode > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
                return fail(StaticSceneResourceStatus::invalid_request,
                            "stock_material_state_invalid", "Serialized material state exceeds the supported integer range");
            for (std::size_t packet_index = 0U; packet_index < packets.size(); ++packet_index) {
                DrawPacket& packet = packets[packet_index];
                if (packet.material != material_index) continue;
                if (!packet_resources_match(packet, binding.shader,
                                            request.limits.scene.max_resource_string_bytes,
                                            module_diagnostic))
                    return {StaticSceneResourceStatus::invalid_request,
                            std::move(module_diagnostic), nullptr};
                if (packet.flags.wireframe != request.wireframe)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "stock_material_wireframe_mismatch", "Packet wireframe state does not match the handoff request");
                MaterialInput material_input{source.shader,
                                             static_cast<int>(source.serializedBlendMode),
                                             static_cast<int>(source.depthMode)};
                MaterialOverride profile_override = state_override(overrides);
                if (!profile_override.is_transparent.has_value())
                    profile_override.is_transparent = packet.flags.transparent;
                const bool desired_transparent = *profile_override.is_transparent;
                const std::size_t state_index = desired_transparent ? 1U : 0U;
                const bool damage_dirt_shader =
                    canonical(binding.shader) == "ksperpixelmultimap_damage_dirt";
                const bool packet_has_dust = std::any_of(
                    packet.resources.begin(), packet.resources.end(), [](const DrawResourceSlot& resource) {
                        return canonical(resource.slot) == "txdust";
                    });
                const bool material_has_dust =
                    binding.textures.find("txdust") != binding.textures.end();
                if (damage_dirt_shader && packet_has_dust != material_has_dust)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "stock_material_resource_layout_mismatch",
                                "The damage packet resource set must match the material txDust declaration");
                const StockMaterialShaderVariant shader_variant =
                    damage_dirt_shader && packet_has_dust
                        ? StockMaterialShaderVariant::damage_dust
                        : StockMaterialShaderVariant::standard;
                const StockMaterialShaderModules* modules = find_modules(
                    request.shader_modules, source.name, binding.shader, shader_variant,
                    request.directional_shadow_receiver,
                    reflection_enabled);
                const bool exact_source_family =
                    canonical(source.shader) == shader_key &&
                    (shader_key == "ksperpixel" ||
                     shader_key == "ksperpixelat");
                const bool source_candidate =
                    request.builtin_vulkan_source ==
                        BuiltinVulkanStockSourceSelector::ks_per_pixel &&
                    exact_source_family &&
                    packet.primitive == DrawPrimitiveKind::static_mesh &&
                    shader_variant == StockMaterialShaderVariant::standard &&
                    !request.directional_shadow_receiver;
                const bool use_builtin_source =
                    modules == nullptr && source_candidate &&
                    device.info().backend == Backend::Vulkan;
                const bool exact_multimap_source_family =
                    canonical(source.shader) == shader_key &&
                    (shader_key == "ksperpixelmultimap_nmdetail" ||
                     shader_key ==
                         "ksperpixelmultimap_at_nmdetail");
                const bool multimap_source_candidate =
                    request.builtin_multimap_source ==
                        BuiltinStockMultiMapSourceSelector::normal_detail &&
                    exact_multimap_source_family &&
                    packet.primitive == DrawPrimitiveKind::static_mesh &&
                    shader_variant == StockMaterialShaderVariant::standard &&
                    !request.directional_shadow_receiver &&
                    !reflection_enabled && !request.wireframe &&
                    !desired_transparent;
                const bool use_builtin_multimap_source =
                    modules == nullptr && multimap_source_candidate;
                const bool d3d12_native_combined =
                    request.builtin_d3d12_native ==
                    BuiltinD3D12StockNativeSelector::
                        ks_per_pixel_base_and_alpha_to_coverage;
                const bool d3d12_native_alpha_to_coverage =
                    shader_key == "ksperpixelat";
                const bool d3d12_native_key_selected =
                    (shader_key == "ksperpixel" &&
                     request.builtin_d3d12_native !=
                         BuiltinD3D12StockNativeSelector::
                             ks_per_pixel_alpha_to_coverage) ||
                    (shader_key == "ksperpixelat" &&
                     request.builtin_d3d12_native !=
                         BuiltinD3D12StockNativeSelector::ks_per_pixel_base);
                const bool d3d12_native_family =
                    request.builtin_d3d12_native !=
                        BuiltinD3D12StockNativeSelector::disabled &&
                    canonical(source.shader) == shader_key &&
                    d3d12_native_key_selected;
                const bool d3d12_native_candidate =
                    request.builtin_d3d12_native !=
                        BuiltinD3D12StockNativeSelector::disabled &&
                    d3d12_native_family &&
                    packet.primitive == DrawPrimitiveKind::static_mesh &&
                    shader_variant == StockMaterialShaderVariant::standard &&
                    !request.directional_shadow_receiver &&
                    !desired_transparent && !packet.flags.blend_enabled &&
                    !request.wireframe;
                const StockMaterialD3D12NativeProgram* d3d12_native_entry =
                    d3d12_native_candidate
                        ? find_d3d12_native_program(
                              request.builtin_d3d12_native_programs,
                              binding.shader)
                        : nullptr;
                const bool use_builtin_d3d12_native =
                    modules == nullptr && d3d12_native_candidate &&
                    d3d12_native_entry != nullptr;
                if (modules == nullptr && source_candidate &&
                    device.info().backend != Backend::Vulkan)
                    return fail(
                        StaticSceneResourceStatus::unsupported,
                        "stock_material_builtin_source_backend_unsupported",
                        "The built-in ksPerPixel source-equivalent fallback requires Vulkan");
                if (modules == nullptr && d3d12_native_family &&
                    request.builtin_d3d12_native !=
                        BuiltinD3D12StockNativeSelector::disabled &&
                    !use_builtin_d3d12_native)
                    return fail(
                        StaticSceneResourceStatus::unsupported,
                        "stock_material_d3d12_native_packet_unsupported",
                        "Installed D3D12 native execution requires an exact selected opaque solid static ksPerPixel variant");
                if (use_builtin_d3d12_native &&
                    (request.targets.colors.size() != 1U ||
                     !d3d12_native_scene_color_format(
                         request.targets.colors.front().format) ||
                     request.targets.colors.front().samples !=
                         ((d3d12_native_alpha_to_coverage ||
                           d3d12_native_combined)
                              ? 4U
                              : 1U) ||
                     (request.targets.has_depth &&
                      request.targets.depth.samples !=
                          ((d3d12_native_alpha_to_coverage ||
                            d3d12_native_combined)
                               ? 4U
                               : 1U))))
                    return fail(
                        StaticSceneResourceStatus::unsupported,
                        "stock_material_d3d12_native_target_unsupported",
                        (d3d12_native_alpha_to_coverage ||
                         d3d12_native_combined)
                            ? "Installed D3D12 alpha-to-coverage and combined programs require a four-sample RGBA8, BGRA8, or RGBA16F color target and optional matching depth target"
                            : "The installed D3D12 base program requires a one-sample RGBA8, BGRA8, or RGBA16F color target and optional matching depth target");
                if (modules == nullptr && !use_builtin_source &&
                    !use_builtin_multimap_source &&
                    !use_builtin_d3d12_native)
                    return fail(
                        StaticSceneResourceStatus::unsupported,
                        shader_variant == StockMaterialShaderVariant::damage_dust
                            ? "stock_material_shader_variant_missing"
                            : "stock_material_shader_module_missing",
                        shader_variant == StockMaterialShaderVariant::damage_dust
                            ? "The txDust packet requires caller-supplied damage-dust shader modules"
                            : "No caller-supplied executable shader modules match the material or shader family");
                if ((use_builtin_source || use_builtin_d3d12_native) &&
                    !native_constants_ready[material_index]) {
                    const StockShadowCasterMaterialResolveResult
                        native_resolved =
                            resolve_stock_shadow_caster_material_constants(
                                binding);
                    if (!native_resolved.ok())
                        return fail(
                            StaticSceneResourceStatus::invalid_request,
                            native_resolved.diagnostic.code.empty()
                                ? "stock_material_builtin_source_constants_invalid"
                                : native_resolved.diagnostic.code,
                            native_resolved.diagnostic.message.empty()
                                ? "The recovered native b4 material resolver rejected the source material"
                                : native_resolved.diagnostic.message);
                    native_constants[material_index] =
                        native_material_constants(native_resolved.constants);
                    native_constants_ready[material_index] = true;
                }
                const apex::scene::SceneNode* packet_node = request.scene->find_node(packet.node);
                const auto apply_profile = [&](const MaterialRenderProfile& profile) {
                    packet.material_profile = profile;
                    packet.flags.transparent = profile.transparent;
                    packet.flags.blend_enabled = profile.blend_enabled;
                    packet.flags.alpha_to_coverage =
                        profile.alpha_to_coverage;
                    packet.flags.depth_test = profile.depth_test;
                    packet.flags.depth_write =
                        profile.depth_write && !profile.transparent;
                    material_needs_alpha_shadow =
                        material_needs_alpha_shadow ||
                        profile.shadow_alpha_tested;
                };
                if (use_builtin_source &&
                    source_pipeline_indices[material_index][state_index] !=
                        std::numeric_limits<std::size_t>::max()) {
                    const std::size_t source_index =
                        source_pipeline_indices[material_index][state_index];
                    pipeline_ptrs[packet_index] =
                        &source_programs[source_index]->pipeline();
                    source_program_by_packet[packet_index] =
                        static_cast<std::uint32_t>(source_index);
                    apply_profile(source_pipeline_profiles[source_index]);
                    continue;
                }
                if (!use_builtin_source &&
                    pipeline_indices[material_index][state_index] !=
                        std::numeric_limits<std::size_t>::max()) {
                    const std::size_t pipeline_index =
                        pipeline_indices[material_index][state_index];
                    const bool pipeline_has_dust =
                        classify_indexed_portable_resource_layout(pipelines[pipeline_index]) ==
                        IndexedPortableResourceLayout::diffuse_normal_maps_damage_dust_with_constants_and_frame;
                    if (pipeline_has_dust != (damage_dirt_shader && packet_has_dust))
                        return fail(StaticSceneResourceStatus::invalid_request,
                                    "stock_material_resource_layout_mismatch",
                                    "Packets sharing one stock material state must agree on txDust presence");
                    pipeline_ptrs[packet_index] = &pipelines[pipeline_index];
                    const MaterialRenderProfile& profile = pipeline_profiles[pipeline_index];
                    if (use_builtin_d3d12_native) {
                        const auto existing = std::find(
                            d3d12_native_programs.begin(),
                            d3d12_native_programs.end(),
                            d3d12_native_entry->program);
                        std::size_t native_index = 0U;
                        if (existing == d3d12_native_programs.end()) {
                            native_index = d3d12_native_programs.size();
                            d3d12_native_programs.push_back(
                                d3d12_native_entry->program);
                        } else {
                            native_index = static_cast<std::size_t>(
                                existing - d3d12_native_programs.begin());
                        }
                        d3d12_native_program_by_packet[packet_index] =
                            static_cast<std::uint32_t>(native_index);
                    }
                    apply_profile(profile);
                    continue;
                }
                StockPipelineRequest pipeline_request;
                pipeline_request.material = std::move(material_input);
                pipeline_request.node = {packet_node != nullptr && packet_node->transparent};
                pipeline_request.override_values = &profile_override;
                if (use_builtin_source) {
                    StockKsPerPixelVulkanSourceProgramResult seed =
                        create_builtin_stock_ks_per_pixel_vulkan_source_program(
                            StockKsPerPixelVariant::base);
                    if (!seed.ok())
                        return fail(
                            StaticSceneResourceStatus::unsupported,
                            "stock_material_builtin_source_seed_invalid",
                            "The immutable Vulkan ksPerPixel source package is unavailable");
                    pipeline_request.shaders =
                        seed.program->pipeline().shaders;
                } else if (use_builtin_multimap_source) {
                    const StockMultiMapSourceVariant variant =
                        shader_key ==
                                "ksperpixelmultimap_at_nmdetail"
                            ? StockMultiMapSourceVariant::
                                  alpha_to_coverage_normal_detail
                            : StockMultiMapSourceVariant::normal_detail;
                    StockMultiMapSourceProgramResult seed =
                        create_builtin_stock_multimap_source_program(
                            device.info().backend, variant);
                    if (!seed.ok())
                        return fail(
                            StaticSceneResourceStatus::unsupported,
                            "stock_material_multimap_source_seed_" +
                                std::string(
                                    stock_multimap_source_status_name(
                                        seed.status)),
                            "The immutable MultiMap source-equivalent package is unavailable");
                    pipeline_request.shaders = seed.program->shaders;
                } else if (use_builtin_d3d12_native) {
                    const ValidatedStockKsPerPixelNativeProgram& program =
                        *d3d12_native_entry->program;
                    pipeline_request.shaders = {
                        {PipelineShaderStage::vertex,
                         PipelineShaderFormat::dxbc,
                         {program.vertex_shader().begin(),
                          program.vertex_shader().end()}},
                        {PipelineShaderStage::fragment,
                         PipelineShaderFormat::dxbc,
                         {program.pixel_shader().begin(),
                          program.pixel_shader().end()}},
                    };
                } else {
                    pipeline_request.shaders.assign(
                        modules->modules.begin(), modules->modules.end());
                }
                pipeline_request.targets = request.targets;
                if (!use_builtin_d3d12_native)
                    pipeline_request.resources = resources_for(
                        binding.shader,
                        damage_dirt_shader && packet_has_dust,
                        request.directional_shadow_receiver,
                        reflection_enabled);
                pipeline_request.wireframe = request.wireframe;
                StockPipelineResult built =
                    build_stock_pipeline(pipeline_request, request.limits.scene.pipeline);
                if (!built.validation.valid)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "stock_material_pipeline_invalid",
                                built.validation.diagnostics.empty()
                                    ? "Stock pipeline validation failed"
                                    : built.validation.diagnostics.front().message);
                built.program.transform_contract = use_builtin_d3d12_native
                                                       ? PipelineTransformContract::none
                                                       : PipelineTransformContract::draw_matrices;
                built.program.depth.compare = PipelineCompareOperation::less;
                built.program.depth.write_enabled =
                    built.profile.depth_write && !built.profile.transparent;
                if (use_builtin_multimap_source) {
                    const StockMultiMapSourceVariant variant =
                        shader_key ==
                                "ksperpixelmultimap_at_nmdetail"
                            ? StockMultiMapSourceVariant::
                                  alpha_to_coverage_normal_detail
                            : StockMultiMapSourceVariant::normal_detail;
                    StockMultiMapSourcePipelineState state;
                    state.targets = built.program.targets;
                    state.raster = built.program.raster;
                    state.blend = built.program.blend;
                    state.depth = built.program.depth;
                    StockMultiMapSourceProgramResult source_result =
                        create_builtin_stock_multimap_source_program(
                            device.info().backend, variant,
                            std::move(state));
                    if (!source_result.ok())
                        return fail(
                            StaticSceneResourceStatus::unsupported,
                            "stock_material_multimap_source_" +
                                std::string(
                                    stock_multimap_source_status_name(
                                        source_result.status)),
                            "The resolved render state is incompatible with the immutable MultiMap source-equivalent package");
                    built.program = std::move(*source_result.program);
                }
                const PipelineValidationResult final_validation =
                    validate_pipeline(built.program, request.limits.scene.pipeline);
                if (!final_validation.valid)
                    return fail(StaticSceneResourceStatus::invalid_request,
                                "stock_material_pipeline_invalid",
                                final_validation.diagnostics.empty()
                                    ? "Stock pipeline validation failed after explicit execution state"
                                    : final_validation.diagnostics.front().message);
                if (use_builtin_d3d12_native &&
                    (built.profile.transparent || built.profile.blend_enabled ||
                     built.profile.alpha_to_coverage !=
                         d3d12_native_alpha_to_coverage))
                    return fail(
                        StaticSceneResourceStatus::unsupported,
                        "stock_material_d3d12_native_packet_unsupported",
                        "Installed D3D12 native execution requires a matching opaque solid ksPerPixel variant");
                if (use_builtin_source) {
                    const StockKsPerPixelVariant variant =
                        built.profile.alpha_to_coverage
                            ? StockKsPerPixelVariant::alpha_to_coverage
                            : StockKsPerPixelVariant::base;
                    StockKsPerPixelVulkanSourcePipelineState source_state;
                    source_state.targets = built.program.targets;
                    source_state.raster = built.program.raster;
                    source_state.blend = built.program.blend;
                    source_state.depth = built.program.depth;
                    StockKsPerPixelVulkanSourceProgramResult source_result =
                        create_builtin_stock_ks_per_pixel_vulkan_source_program(
                            variant, std::move(source_state));
                    if (!source_result.ok())
                        return fail(
                            StaticSceneResourceStatus::unsupported,
                            "stock_material_builtin_source_" + std::string(
                                stock_ks_per_pixel_vulkan_source_status_name(
                                    source_result.status)),
                            "The resolved render state is incompatible with the immutable Vulkan ksPerPixel source package");
                    if (source_programs.size() >=
                        static_cast<std::size_t>(
                            invalid_static_scene_source_program_index))
                        return fail(
                            StaticSceneResourceStatus::invalid_request,
                            "stock_material_builtin_source_owner_limit",
                            "Built-in Vulkan source owners exceed packet index capacity");
                    const std::size_t source_index = source_programs.size();
                    auto owner = std::make_shared<const
                        ValidatedStockKsPerPixelVulkanSourceProgram>(
                            std::move(*source_result.program));
                    source_programs.push_back(std::move(owner));
                    source_pipeline_profiles.push_back(built.profile);
                    source_pipeline_indices[material_index][state_index] =
                        source_index;
                    source_program_by_packet[packet_index] =
                        static_cast<std::uint32_t>(source_index);
                    pipeline_ptrs[packet_index] =
                        &source_programs.back()->pipeline();
                } else {
                    pipeline_indices[material_index][state_index] =
                        pipelines.size();
                    pipelines.push_back(std::move(built.program));
                    pipeline_profiles.push_back(built.profile);
                    pipeline_ptrs[packet_index] = &pipelines.back();
                    if (use_builtin_d3d12_native) {
                        if (d3d12_native_programs.size() >=
                            static_cast<std::size_t>(
                                invalid_static_scene_source_program_index))
                            return fail(
                                StaticSceneResourceStatus::invalid_request,
                                "stock_material_d3d12_native_owner_limit",
                                "Installed D3D12 native owners exceed packet index capacity");
                        const auto existing = std::find(
                            d3d12_native_programs.begin(),
                            d3d12_native_programs.end(),
                            d3d12_native_entry->program);
                        std::size_t native_index = 0U;
                        if (existing == d3d12_native_programs.end()) {
                            native_index = d3d12_native_programs.size();
                            d3d12_native_programs.push_back(
                                d3d12_native_entry->program);
                        } else {
                            native_index = static_cast<std::size_t>(
                                existing - d3d12_native_programs.begin());
                        }
                        d3d12_native_program_by_packet[packet_index] =
                            static_cast<std::uint32_t>(native_index);
                    }
                }
                apply_profile(built.profile);
            }
            if (material_needs_alpha_shadow) {
                const StockShadowCasterMaterialResolveResult shadow_resolved =
                    resolve_stock_shadow_caster_material_constants(binding);
                if (!shadow_resolved.ok())
                    return fail(
                        StaticSceneResourceStatus::invalid_request,
                        shadow_resolved.diagnostic.code.empty()
                            ? "stock_shadow_material_constants_invalid"
                            : shadow_resolved.diagnostic.code,
                        shadow_resolved.diagnostic.message.empty()
                            ? "The recovered shadow material resolver rejected the material"
                            : shadow_resolved.diagnostic.message);
                shadow_constants[material_index] = shadow_resolved.constants;
                has_alpha_shadow_constants = true;
            }
        }

        const std::size_t d3d12_native_packet_count =
            static_cast<std::size_t>(std::count_if(
                d3d12_native_program_by_packet.begin(),
                d3d12_native_program_by_packet.end(),
                [](std::uint32_t value) {
                    return value != invalid_static_scene_source_program_index;
                }));
        if (d3d12_native_packet_count >
            max_stock_ks_per_pixel_native_batch_draws)
            return fail(
                StaticSceneResourceStatus::unsupported,
                "stock_material_d3d12_native_draw_limit",
                "The installed D3D12 native scene exceeds the sampler-heap batch limit");

        StaticScenePrepareRequest scene_request;
        scene_request.model = request.model;
        scene_request.scene = request.scene;
        scene_request.packets = packets;
        scene_request.pipelines_by_packet = pipeline_ptrs;
        scene_request.material_constants_by_material = constants;
        if (request.multimap_reflection)
            scene_request.multimap_reflection_constants_by_material =
                multimap_reflection_constants;
        if (!source_programs.empty()) {
            scene_request.stock_vulkan_source_programs = source_programs;
            scene_request.stock_vulkan_source_program_by_packet =
                source_program_by_packet;
            scene_request.stock_vulkan_source_material_constants_by_material =
                native_constants;
            scene_request.stock_vulkan_source_sampler_settings =
                request.builtin_vulkan_source_sampler_settings;
        }
        if (!d3d12_native_programs.empty()) {
            scene_request.stock_d3d12_native_programs =
                d3d12_native_programs;
            scene_request.stock_d3d12_native_program_by_packet =
                d3d12_native_program_by_packet;
            scene_request.stock_d3d12_native_material_constants_by_material =
                native_constants;
            scene_request.stock_d3d12_native_sampler_settings =
                request.builtin_d3d12_native_sampler_settings;
        }
        if (has_alpha_shadow_constants)
            scene_request.stock_shadow_constants_by_material = shadow_constants;
        scene_request.texture_authority = request.texture_authority;
        scene_request.limits = request.limits.scene;
        StaticSceneResourceResult prepared = prepare_static_scene_resources(device, scene_request);
        return {prepared.status, std::move(prepared.diagnostic), std::move(prepared.resources)};
    } catch (const MaterialBindingError& error) {
        return fail(StaticSceneResourceStatus::invalid_request, error.code(), error.what());
    } catch (const std::bad_alloc&) {
        return fail(StaticSceneResourceStatus::allocation_failed,
                    "stock_material_allocation_failed", "The bounded production handoff could not allocate its copied tables");
    }
}

} // namespace apex::render
