#include "apex/app/workspace_session.hpp"

#include "apex/core/parse_error.hpp"

#include <algorithm>
#include <cctype>
#include <new>
#include <string_view>
#include <utility>

namespace apex::app {
namespace {

using core::ParseError;

class WorkspaceModelError final : public std::runtime_error {
public:
    WorkspaceModelError(std::string source, std::size_t offset,
                        std::string message)
        : std::runtime_error(std::move(message)), source_(std::move(source)),
          offset_(offset) {}

    [[nodiscard]] const std::string& source() const noexcept { return source_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    std::string source_;
    std::size_t offset_ = 0U;
};

[[nodiscard]] ParseError sessionError(std::string_view source, std::string_view code,
                                      std::string_view message) {
    return ParseError("WORKSPACE_SESSION", std::string(source), 0,
                      std::string(code), std::string(message));
}

[[nodiscard]] WorkspaceSessionDiagnostic diagnostic(const ParseError& error) {
    return {error.code(), error.source(), error.what(), error.offset()};
}

[[nodiscard]] WorkspaceSessionDiagnostic diagnostic(std::string_view code,
                                                    std::string_view path,
                                                    std::string_view message) {
    return {std::string(code), std::string(path), std::string(message), 0};
}

[[nodiscard]] std::string workspaceKind(WorkspaceSessionKind kind) {
    switch (kind) {
    case WorkspaceSessionKind::generic: return "generic";
    case WorkspaceSessionKind::track: return "track";
    case WorkspaceSessionKind::carLods: return "carLods";
    }
    return "generic";
}

void chargeInput(std::size_t bytes, std::size_t& used,
                 const WorkspaceSessionLimits& limits, std::string_view source) {
    if (bytes > limits.maxTotalInputBytes ||
        used > limits.maxTotalInputBytes - bytes) {
        throw sessionError(source, "INPUT_LIMIT",
                           "workspace input exceeds the aggregate byte limit");
    }
    used += bytes;
}

void validateManifestName(std::string_view value, std::size_t maxBytes) {
    if (value.empty()) return;
    if (value.size() > maxBytes || value.front() == '/' ||
        (value.size() >= 2U && std::isalpha(static_cast<unsigned char>(value[0])) != 0 &&
         value[1] == ':') || value.find('\0') != std::string_view::npos) {
        throw sessionError(value, "UNSAFE_REFERENCE", "workspace manifest path is unsafe");
    }
    std::size_t begin = 0U;
    while (begin <= value.size()) {
        const auto end = value.find_first_of("/\\", begin);
        const auto component = value.substr(
            begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (component == "..")
            throw sessionError(value, "UNSAFE_REFERENCE", "workspace manifest path traverses its root");
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
}

[[nodiscard]] workspace::WorkspaceOptions optionsFor(
    const WorkspaceSessionOpenRequest& request,
    const WorkspaceSessionLimits& limits) {
    workspace::WorkspaceOptions options;
    options.name = request.name;
    options.kind = workspaceKind(request.kind);
    options.manifest = request.manifestName;
    options.limits = limits.workspace;
    return options;
}

[[nodiscard]] WorkspaceSessionDocument openBytes(
    const WorkspaceSessionOpenRequest& request,
    const WorkspaceSessionLimits& limits) {
    if (request.modelFiles.empty())
        throw sessionError(request.name, "EMPTY_WORKSPACE", "workspace has no model files");
    if (request.modelFiles.size() > limits.maxModels)
        throw sessionError(request.name, "COUNT_LIMIT", "workspace model count exceeds its limit");
    const bool hasManifestName = !request.manifestName.empty();
    const bool hasManifestBytes = !request.manifestBytes.empty();
    if (request.kind == WorkspaceSessionKind::generic) {
        if (hasManifestName || hasManifestBytes)
            throw sessionError(request.name, "INVALID_KIND",
                               "generic workspaces do not accept a manifest");
    } else if (!hasManifestName || !hasManifestBytes) {
        throw sessionError(request.name, "INVALID_MANIFEST",
                           "track and car-LOD workspaces require a named manifest");
    }
    if (!request.manifestName.empty()) {
        validateManifestName(request.manifestName, limits.workspace.maxPathBytes);
    }

    std::size_t inputBytes = 0;
    if (!request.manifestBytes.empty())
        chargeInput(request.manifestBytes.size(), inputBytes, limits, request.manifestName);
    for (const auto& file : request.modelFiles) {
        if (file.name.empty())
            throw sessionError(request.name, "INVALID_NAME", "workspace model name is empty");
        validateManifestName(file.name, limits.workspace.maxPathBytes);
        chargeInput(file.bytes.size(), inputBytes, limits, file.name);
    }

    std::vector<formats::Kn5File> models;
    models.reserve(request.modelFiles.size());
    for (const auto& file : request.modelFiles) {
        auto parseOptions = limits.kn5;
        parseOptions.metadataOnly = false;
        try {
            models.push_back(formats::parseKn5(file.bytes, file.name, parseOptions));
        } catch (const formats::Kn5Error& error) {
            throw WorkspaceModelError(file.name, error.offset(), error.what());
        }
    }

    std::vector<workspace::WorkspaceModelInput> inputs;
    inputs.reserve(models.size());
    for (std::size_t index = 0; index < models.size(); ++index) {
        workspace::WorkspaceModelInput input;
        input.name = request.modelFiles[index].name;
        input.model = &models[index];
        input.size = request.modelFiles[index].bytes.size();
        inputs.push_back(std::move(input));
    }

    auto options = optionsFor(request, limits);
    workspace::WorkspaceAssembly assembly;
    if (!request.manifestBytes.empty()) {
        const auto manifestText = std::string_view(
            reinterpret_cast<const char*>(request.manifestBytes.data()),
            request.manifestBytes.size());
        if (request.kind == WorkspaceSessionKind::track) {
            const auto manifest = workspace::parseModelsIni(
                manifestText, request.manifestName, limits.manifestIni, limits.workspace);
            assembly = workspace::assembleTrackWorkspace(manifest, inputs, std::move(options));
        } else if (request.kind == WorkspaceSessionKind::carLods) {
            const auto manifest = workspace::parseCarLodsIni(
                manifestText, request.manifestName, limits.manifestIni, limits.workspace);
            assembly = workspace::assembleCarLodWorkspace(manifest, inputs, std::move(options));
        } else {
            throw sessionError(request.manifestName, "INVALID_KIND",
                               "generic workspaces do not accept a manifest");
        }
    } else {
        assembly = workspace::mergeKn5Models(inputs, std::move(options));
    }

    auto conversion = scene::convertKn5Scene(assembly.model, limits.scene);
    auto binding = workspace::bindWorkspaceScene(
        conversion.snapshot, assembly.workspace, limits.sceneBinding);

    WorkspaceSessionDocument document;
    document.assembly = std::move(assembly);
    document.scene = std::move(conversion);
    document.sceneBinding = std::move(binding);
    return document;
}

[[nodiscard]] std::string manifestDirectory(std::string_view manifestName) {
    const auto slash = manifestName.find_last_of("/\\");
    return slash == std::string_view::npos
               ? std::string{}
               : std::string(manifestName.substr(0, slash));
}

[[nodiscard]] std::string sourceManifestBase(WorkspaceSessionKind kind,
                                             std::string_view manifestName) {
    const auto directory = manifestDirectory(manifestName);
    if (kind != WorkspaceSessionKind::carLods || directory.empty()) return directory;
    const auto slash = directory.find_last_of('/');
    auto leaf = directory.substr(slash == std::string::npos ? 0U : slash + 1U);
    std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    if (leaf == "data" || (leaf.size() > 5U &&
                            (leaf.substr(0U, 5U) == "data_" ||
                             leaf.substr(0U, 5U) == "data-")))
        return slash == std::string::npos ? std::string{} : directory.substr(0U, slash);
    return directory;
}

[[nodiscard]] std::string sourceReference(std::string_view base,
                                          std::string_view reference) {
    if (base.empty()) return std::string(reference);
    return std::string(base) + "/" + std::string(reference);
}

[[nodiscard]] const assets::AssetFile& resolveSourceFile(
    const assets::AssetSource& source, std::string_view requested) {
    const auto resolved = source.resolve(requested);
    if (resolved.status == assets::AssetResolveStatus::ambiguous)
        throw sessionError(requested, "AMBIGUOUS_REFERENCE", resolved.diagnostic);
    if (resolved.status != assets::AssetResolveStatus::resolved || resolved.file == nullptr)
        throw sessionError(requested, resolved.status == assets::AssetResolveStatus::rejected
                                       ? "UNSAFE_REFERENCE"
                                       : "INVALID_REFERENCE",
                           resolved.diagnostic.empty() ? "workspace asset did not resolve"
                                                       : resolved.diagnostic);
    return *resolved.file;
}

}  // namespace

const char* workspace_session_status_name(WorkspaceSessionStatus status) noexcept {
    switch (status) {
    case WorkspaceSessionStatus::ok: return "ok";
    case WorkspaceSessionStatus::invalid: return "invalid";
    case WorkspaceSessionStatus::failed: return "failed";
    }
    return "failed";
}

WorkspaceSession::WorkspaceSession(WorkspaceSessionLimits limits)
    : limits_(std::move(limits)) {}

WorkspaceSessionResult WorkspaceSession::open(
    const WorkspaceSessionOpenRequest& request) const {
    WorkspaceSessionResult result;
    try {
        auto document = openBytes(request, limits_);
        result.document = std::move(document);
        result.status = WorkspaceSessionStatus::ok;
    } catch (const ParseError& error) {
        result.status = WorkspaceSessionStatus::invalid;
        result.diagnostics.push_back(diagnostic(error));
    } catch (const WorkspaceModelError& error) {
        result.status = WorkspaceSessionStatus::invalid;
        result.diagnostics.push_back({"MODEL_INVALID", error.source(), error.what(),
                                      error.offset()});
    } catch (const std::bad_alloc&) {
        result.status = WorkspaceSessionStatus::failed;
        result.diagnostics.push_back(diagnostic(
            "ALLOCATION_FAILED", request.name,
            "workspace allocation failed within configured limits"));
    } catch (const std::exception& error) {
        result.status = WorkspaceSessionStatus::invalid;
        result.diagnostics.push_back(diagnostic(
            "WORKSPACE_OPEN_FAILED", request.name, error.what()));
    }
    return result;
}

WorkspaceSessionResult WorkspaceSession::openAssetSource(
    WorkspaceSessionKind kind, std::string name, std::string manifestName,
    const assets::AssetSource& source) const {
    try {
        if (kind == WorkspaceSessionKind::generic)
            throw sessionError(manifestName, "INVALID_KIND",
                               "generic workspaces do not accept an asset manifest");
        const auto& manifestFile = resolveSourceFile(source, manifestName);
        std::size_t sourceInputBytes = 0;
        chargeInput(manifestFile.size, sourceInputBytes, limits_, manifestName);
        const auto manifestBytes = source.read(manifestFile);
        const auto manifestText = std::string_view(
            reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size());

        std::vector<std::string> references;
        if (kind == WorkspaceSessionKind::track) {
            auto manifest = workspace::parseModelsIni(
                manifestText, manifestName, limits_.manifestIni, limits_.workspace);
            references.reserve(manifest.models.size() + manifest.dynamicObjects.size());
            for (const auto& model : manifest.models) references.push_back(model.file);
            for (const auto& object : workspace::contiguousDynamicTrackObjects(
                         manifest.dynamicObjects, manifest.warnings))
                references.push_back(object.file);
        } else {
            const auto manifest = workspace::parseCarLodsIni(
                manifestText, manifestName, limits_.manifestIni, limits_.workspace);
            references.reserve(manifest.lods.size());
            for (const auto& lod : manifest.lods) references.push_back(lod.file);
        }
        if (references.empty())
            throw sessionError(manifestName, "EMPTY_WORKSPACE", "workspace manifest has no model references");
        if (references.size() > limits_.maxModels)
            throw sessionError(manifestName, "COUNT_LIMIT", "workspace model reference count exceeds its limit");

        // A manifest may reference one shared model from several entries. The
        // assembly layer reuses one available input for those entries, so the
        // source adapter must provide one logical input per unique reference.
        std::vector<std::string> uniqueReferences;
        uniqueReferences.reserve(references.size());
        for (const auto& reference : references) {
            if (std::find(uniqueReferences.begin(), uniqueReferences.end(), reference) ==
                uniqueReferences.end())
                uniqueReferences.push_back(reference);
        }
        references = std::move(uniqueReferences);

        const auto base = sourceManifestBase(kind, manifestName);
        std::vector<std::vector<std::uint8_t>> bytes;
        std::vector<std::string> resolvedNames;
        bytes.reserve(references.size());
        resolvedNames.reserve(references.size());
        for (const auto& reference : references) {
            const auto& file = resolveSourceFile(source, sourceReference(base, reference));
            chargeInput(file.size, sourceInputBytes, limits_, reference);
            resolvedNames.push_back(reference);
            bytes.push_back(source.read(file));
        }
        std::vector<WorkspaceSessionFile> files;
        files.reserve(resolvedNames.size());
        for (std::size_t index = 0; index < resolvedNames.size(); ++index)
            files.push_back({resolvedNames[index], bytes[index]});
        WorkspaceSessionOpenRequest request;
        request.kind = kind;
        request.name = std::move(name);
        request.manifestName = std::move(manifestName);
        request.manifestBytes = manifestBytes;
        request.modelFiles = files;
        return open(request);
    } catch (const ParseError& error) {
        WorkspaceSessionResult result;
        result.status = WorkspaceSessionStatus::invalid;
        result.diagnostics.push_back(diagnostic(error));
        return result;
    } catch (const std::bad_alloc&) {
        WorkspaceSessionResult result;
        result.status = WorkspaceSessionStatus::failed;
        result.diagnostics.push_back(diagnostic(
            "ALLOCATION_FAILED", manifestName,
            "workspace allocation failed within configured limits"));
        return result;
    } catch (const std::exception& error) {
        WorkspaceSessionResult result;
        result.status = WorkspaceSessionStatus::invalid;
        result.diagnostics.push_back(diagnostic(
            "WORKSPACE_SOURCE_FAILED", manifestName, error.what()));
        return result;
    }
}

}  // namespace apex::app
