#pragma once

#include "apex/app/workspace_session.hpp"
#include "apex/assets/asset_source.hpp"
#include "apex/formats/fbx.hpp"
#include "apex/formats/fbx_conversion.hpp"
#include "apex/render/fbx_external_texture_authority.hpp"
#include "apex/render/fbx_render_adapter.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace apex::app {

enum class FbxPreviewDocumentStatus : std::uint8_t {
    ready,
    staged,
    invalid_request,
    unsupported,
    resource_limit,
    failed,
};

struct FbxPreviewDocumentDiagnostic {
    std::string code;
    std::string path;
    std::string message;
    std::size_t offset = 0U;
};

struct FbxPreviewTextureGrant {
    std::string id;
    const assets::AssetSource* source = nullptr;
};

struct FbxPreviewDocumentRequest {
    // This is a bounded logical file name, not a filesystem path.
    std::string source = "scene.fbx";
    std::span<const std::uint8_t> bytes{};
    std::optional<FbxPreviewTextureGrant> textures;
};

struct FbxPreviewDocumentLimits {
    std::size_t max_diagnostics = 20'000U;
    std::size_t max_diagnostic_bytes = 32U * 1024U * 1024U;
    formats::FbxLimits parser{};
    formats::FbxConversionLimits conversion{};
    render::FbxExternalTextureAuthorityLimits textures{};
    render::FbxRenderAdapterLimits adapter{};
    workspace::WorkspaceLimits workspace{};
    workspace::WorkspaceSceneLimits scene_binding{};
};

// A ready document can enter the shared Vulkan/D3D12 viewport. A staged
// document remains available for inspection, but gpu_renderable() stays false.
struct FbxPreviewDocumentResult {
    FbxPreviewDocumentStatus status =
        FbxPreviewDocumentStatus::invalid_request;
    std::vector<FbxPreviewDocumentDiagnostic> diagnostics;
    std::optional<WorkspaceSessionDocument> document;
    std::vector<formats::FbxAnimationClip> animations;

    [[nodiscard]] bool ok() const noexcept {
        return (status == FbxPreviewDocumentStatus::ready ||
                status == FbxPreviewDocumentStatus::staged) &&
               document.has_value();
    }

    [[nodiscard]] bool gpu_renderable() const noexcept {
        return status == FbxPreviewDocumentStatus::ready &&
               document.has_value();
    }
};

// Parse caller-owned FBX bytes and create the standard viewport document.
// External images are read only through the optional explicit AssetSource
// grant. No source, grant, file handle, path, or GPU object is retained.
[[nodiscard]] FbxPreviewDocumentResult open_fbx_preview_document(
    const FbxPreviewDocumentRequest& request,
    FbxPreviewDocumentLimits limits = {});

[[nodiscard]] const char* fbx_preview_document_status_name(
    FbxPreviewDocumentStatus status) noexcept;

} // namespace apex::app
