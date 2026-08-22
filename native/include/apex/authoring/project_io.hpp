#pragma once

#include "apex/authoring/project.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace apex::authoring {

inline constexpr std::string_view kProjectFormat = "apex-editor-project";
inline constexpr int kProjectVersion = 1;

struct ProjectIoLimits {
    std::size_t maxInputBytes = 16U * 1024U * 1024U;
    std::size_t maxOutputBytes = 16U * 1024U * 1024U;
    std::size_t maxDepth = 64;
    std::size_t maxMembers = 100'000;
    std::size_t maxStringBytes = 4096;
    std::size_t maxEdits = 10'000;
};

struct ProjectIoDiagnostic {
    std::string code;
    std::string path;
    std::string message;
};

struct ProjectIoResult {
    std::optional<ProjectState> project;
    std::vector<ProjectIoDiagnostic> diagnostics;
};

[[nodiscard]] std::string serializeProject(const ProjectState& project,
                                           ProjectIoLimits limits = {});
[[nodiscard]] ProjectIoResult parseProject(std::string_view text,
                                           ProjectIoLimits limits = {});
[[nodiscard]] ProjectIoResult parseProject(std::string_view text,
                                           const SourceIdentity& expectedSource,
                                           ProjectIoLimits limits = {});

// Exports the modeled material and mesh edits in the same deterministic CSP
// section shape as serializeEditorCsp. Unmodeled project categories are
// reported in diagnostics and are not silently represented as CSP edits.
[[nodiscard]] std::string serializeEditorCsp(const ProjectState& project,
                                             ProjectIoLimits limits = {},
                                             std::vector<ProjectIoDiagnostic>* diagnostics = nullptr);

} // namespace apex::authoring
