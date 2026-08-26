#include "apex/app/fbx_preview_document.hpp"

#include "apex/core/parse_error.hpp"
#include "apex/scene/kn5_scene.hpp"
#include "apex/workspace/workspace.hpp"
#include "apex/workspace/workspace_scene.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apex::app {
namespace {

class PreviewFailure final : public std::runtime_error {
public:
    PreviewFailure(FbxPreviewDocumentStatus status,
                   FbxPreviewDocumentDiagnostic diagnostic)
        : std::runtime_error(diagnostic.message), status_(status),
          diagnostic_(std::move(diagnostic)) {}

    FbxPreviewDocumentStatus status_;
    FbxPreviewDocumentDiagnostic diagnostic_;
};

[[noreturn]] void fail(FbxPreviewDocumentStatus status, std::string code,
                       std::string path, std::string message,
                       std::size_t offset = 0U) {
    throw PreviewFailure(status,
                         {std::move(code), std::move(path),
                          std::move(message), offset});
}

class DiagnosticCollector final {
public:
    explicit DiagnosticCollector(const FbxPreviewDocumentLimits& limits)
        : limits_(limits) {}

    void add(std::string code, std::string path, std::string message,
             std::size_t offset = 0U) {
        if (values_.size() >= limits_.max_diagnostics)
            fail(FbxPreviewDocumentStatus::resource_limit,
                 "fbx_preview_diagnostic_limit", "diagnostics",
                 "FBX preview diagnostic count exceeds its limit");
        const auto bytes = checked_add(
            checked_add(code.size(), path.size()), message.size());
        if (bytes > limits_.max_diagnostic_bytes - used_bytes_)
            fail(FbxPreviewDocumentStatus::resource_limit,
                 "fbx_preview_diagnostic_limit", "diagnostics",
                 "FBX preview diagnostic bytes exceed their limit");
        used_bytes_ += bytes;
        values_.push_back({std::move(code), std::move(path),
                           std::move(message), offset});
    }

    [[nodiscard]] std::vector<FbxPreviewDocumentDiagnostic> take() {
        return std::move(values_);
    }

private:
    [[nodiscard]] static std::size_t checked_add(std::size_t left,
                                                 std::size_t right) {
        if (right > std::numeric_limits<std::size_t>::max() - left)
            fail(FbxPreviewDocumentStatus::resource_limit,
                 "fbx_preview_diagnostic_limit", "diagnostics",
                 "FBX preview diagnostic size overflows");
        return left + right;
    }

    const FbxPreviewDocumentLimits& limits_;
    std::vector<FbxPreviewDocumentDiagnostic> values_;
    std::size_t used_bytes_ = 0U;
};

[[nodiscard]] bool valid_source_name(std::string_view source,
                                     std::size_t max_bytes) noexcept {
    if (source.empty() || source.size() > max_bytes || source == "." ||
        source == "..")
        return false;
    return std::all_of(source.begin(), source.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return character != '/' && character != '\\' && character != ':' &&
               byte >= 0x20U && byte != 0x7fU;
    });
}

[[nodiscard]] FbxPreviewDocumentStatus texture_status(
    render::ExternalTextureAuthorityStatus status) noexcept {
    switch (status) {
    case render::ExternalTextureAuthorityStatus::ready:
        return FbxPreviewDocumentStatus::ready;
    case render::ExternalTextureAuthorityStatus::unsupported:
        return FbxPreviewDocumentStatus::unsupported;
    case render::ExternalTextureAuthorityStatus::resource_limit:
        return FbxPreviewDocumentStatus::resource_limit;
    case render::ExternalTextureAuthorityStatus::invalid_request:
    case render::ExternalTextureAuthorityStatus::rejected:
    case render::ExternalTextureAuthorityStatus::missing:
    case render::ExternalTextureAuthorityStatus::ambiguous:
    case render::ExternalTextureAuthorityStatus::read_failed:
        return FbxPreviewDocumentStatus::invalid_request;
    }
    return FbxPreviewDocumentStatus::invalid_request;
}

[[nodiscard]] FbxPreviewDocumentStatus adapter_status(
    render::FbxRenderAdapterStatus status) noexcept {
    switch (status) {
    case render::FbxRenderAdapterStatus::ready:
        return FbxPreviewDocumentStatus::ready;
    case render::FbxRenderAdapterStatus::staged:
        return FbxPreviewDocumentStatus::staged;
    case render::FbxRenderAdapterStatus::invalid_request:
        return FbxPreviewDocumentStatus::invalid_request;
    case render::FbxRenderAdapterStatus::unsupported:
        return FbxPreviewDocumentStatus::unsupported;
    case render::FbxRenderAdapterStatus::resource_limit:
        return FbxPreviewDocumentStatus::resource_limit;
    }
    return FbxPreviewDocumentStatus::invalid_request;
}

[[nodiscard]] FbxPreviewDocumentStatus workspace_error_status(
    std::string_view code) noexcept {
    return code.find("LIMIT") != std::string_view::npos ||
                   code.find("OVERFLOW") != std::string_view::npos
               ? FbxPreviewDocumentStatus::resource_limit
               : FbxPreviewDocumentStatus::invalid_request;
}

void collect_conversion_diagnostics(
    DiagnosticCollector& output,
    const formats::FbxSceneConversion& conversion) {
    for (const auto& diagnostic : conversion.diagnostics)
        output.add(diagnostic.code, diagnostic.path, diagnostic.message);
}

void collect_texture_diagnostics(
    DiagnosticCollector& output,
    const render::FbxExternalTextureAuthorityResult& textures) {
    for (const auto& diagnostic : textures.diagnostics) {
        std::string path = "textures";
        if (diagnostic.material_index !=
            render::invalid_external_texture_resource)
            path += "/material/" +
                    std::to_string(diagnostic.material_index);
        if (!diagnostic.basename.empty())
            path += "/" + diagnostic.basename;
        output.add(diagnostic.code, std::move(path), diagnostic.message);
    }
    for (const auto& diagnostic : textures.authority.diagnostics)
        output.add(diagnostic.code, diagnostic.path, diagnostic.message);
}

void collect_adapter_diagnostics(
    DiagnosticCollector& output,
    const render::FbxRenderAdapterResult& adapter) {
    for (const auto& diagnostic : adapter.diagnostics)
        output.add(diagnostic.code, diagnostic.path, diagnostic.message);
}

[[nodiscard]] WorkspaceSessionDocument make_workspace_document(
    const std::string& source, const formats::Kn5File& model,
    std::size_t source_bytes, const FbxPreviewDocumentLimits& limits) {
    workspace::WorkspaceModelInput input;
    input.name = source;
    input.model = &model;
    input.size = source_bytes;
    workspace::WorkspaceOptions options;
    options.name = source;
    options.kind = "generic";
    options.limits = limits.workspace;
    auto assembly = workspace::mergeKn5Models(
        std::span<const workspace::WorkspaceModelInput>(&input, 1U),
        std::move(options));
    auto scene = scene::convertKn5Scene(assembly.model, limits.adapter.scene);
    auto binding = workspace::bindWorkspaceScene(
        scene.snapshot, assembly.workspace, limits.scene_binding);
    return {std::move(assembly), std::move(scene), std::move(binding)};
}

} // namespace

const char* fbx_preview_document_status_name(
    FbxPreviewDocumentStatus status) noexcept {
    switch (status) {
    case FbxPreviewDocumentStatus::ready: return "ready";
    case FbxPreviewDocumentStatus::staged: return "staged";
    case FbxPreviewDocumentStatus::invalid_request: return "invalid_request";
    case FbxPreviewDocumentStatus::unsupported: return "unsupported";
    case FbxPreviewDocumentStatus::resource_limit: return "resource_limit";
    case FbxPreviewDocumentStatus::failed: return "failed";
    }
    return "failed";
}

FbxPreviewDocumentResult open_fbx_preview_document(
    const FbxPreviewDocumentRequest& request,
    FbxPreviewDocumentLimits limits) {
    FbxPreviewDocumentResult result;
    try {
        if (limits.max_diagnostics == 0U ||
            limits.max_diagnostic_bytes == 0U)
            fail(FbxPreviewDocumentStatus::invalid_request,
                 "fbx_preview_limits_invalid", "limits",
                 "FBX preview diagnostic limits must be nonzero");
        if (!valid_source_name(request.source,
                               limits.parser.parse.maxStringBytes))
            fail(FbxPreviewDocumentStatus::invalid_request,
                 "fbx_preview_source_invalid", "source",
                 "FBX preview source must be a bounded logical file name");
        if (request.bytes.empty())
            fail(FbxPreviewDocumentStatus::invalid_request,
                 "fbx_preview_input_empty", request.source,
                 "FBX preview input is empty");
        if (request.textures.has_value() &&
            (request.textures->source == nullptr ||
             request.textures->id.empty()))
            fail(FbxPreviewDocumentStatus::invalid_request,
                 "fbx_preview_texture_grant_invalid", "textures",
                 "FBX preview texture grant requires an identity and source");

        DiagnosticCollector diagnostics(limits);
        auto document = formats::parseFbx(request.bytes, request.source,
                                          limits.parser);
        auto conversion = formats::convertFbxScene(
            document, limits.conversion);
        collect_conversion_diagnostics(diagnostics, conversion);

        std::optional<render::FbxExternalTextureAuthorityResult> textures;
        if (request.textures.has_value()) {
            textures = render::resolve_fbx_external_texture_authority(
                conversion, request.textures->id,
                *request.textures->source, limits.textures);
            collect_texture_diagnostics(diagnostics, *textures);
            if (!textures->ok()) {
                result.status = texture_status(textures->status);
                result.diagnostics = diagnostics.take();
                return result;
            }
        }

        auto adapter = render::build_fbx_render_scene(
            conversion, textures.has_value() ? &*textures : nullptr,
            request.source, limits.adapter);
        collect_adapter_diagnostics(diagnostics, adapter);
        result.status = adapter_status(adapter.status);
        if (!adapter.ok()) {
            result.diagnostics = diagnostics.take();
            return result;
        }

        auto workspace_document = make_workspace_document(
            request.source, *adapter.model, request.bytes.size(), limits);
        result.document = std::move(workspace_document);
        result.animations = std::move(conversion.animations);
        result.diagnostics = diagnostics.take();
        return result;
    } catch (const formats::FbxError& error) {
        result.status = FbxPreviewDocumentStatus::invalid_request;
        result.diagnostics.push_back(
            {error.code(), request.source, error.what(), error.offset()});
    } catch (const formats::FbxConversionError& error) {
        result.status = FbxPreviewDocumentStatus::invalid_request;
        result.diagnostics.push_back(
            {error.diagnostic().code, error.diagnostic().path,
             error.diagnostic().message, 0U});
    } catch (const core::ParseError& error) {
        result.status = workspace_error_status(error.code());
        result.diagnostics.push_back(
            {error.code(), error.source(), error.what(), error.offset()});
    } catch (const PreviewFailure& error) {
        result.status = error.status_;
        result.diagnostics.push_back(error.diagnostic_);
    } catch (const std::bad_alloc&) {
        result.status = FbxPreviewDocumentStatus::resource_limit;
        result.diagnostics.push_back(
            {"fbx_preview_allocation_failed", "preview",
             "FBX preview allocation failed", 0U});
    } catch (const std::exception& error) {
        result.status = FbxPreviewDocumentStatus::failed;
        result.diagnostics.push_back(
            {"fbx_preview_failed", "preview", error.what(), 0U});
    }
    result.document.reset();
    result.animations.clear();
    return result;
}

} // namespace apex::app
