#include "apex/authoring/project.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <regex>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>

namespace apex::authoring {

namespace {

[[nodiscard]] AuthoringError error(std::string_view code, std::string_view message) {
    return AuthoringError("AUTHORING", "project", 0, std::string(code), std::string(message));
}

[[nodiscard]] std::string trim(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) --end;
    return std::string(value.substr(begin, end - begin));
}

[[nodiscard]] std::string lowerAscii(std::string_view value) {
    std::string output(value);
    for (auto& character : output) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            character = static_cast<char>(byte + ('a' - 'A'));
    }
    return output;
}

[[nodiscard]] std::string upperAscii(std::string_view value) {
    std::string output(value);
    for (auto& character : output) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z'))
            character = static_cast<char>(byte - ('a' - 'A'));
    }
    return output;
}

[[nodiscard]] std::string safeText(std::string value, std::size_t maximum,
                                   std::string_view label, bool allowEmpty = false) {
    value = trim(value);
    if ((!allowEmpty && value.empty()) || value.size() > maximum)
        throw error("EDIT_INVALID", std::string(label) + " is empty or exceeds its limit");
    for (const auto character : value)
        if (static_cast<unsigned char>(character) < 0x20U || character == '\x7f' || character == '\0')
            throw error("EDIT_INVALID", std::string(label) + " contains an unsafe character");
    return value;
}

[[nodiscard]] std::string safePath(std::string value, std::size_t maximum,
                                   std::string_view label) {
    value = safeText(std::move(value), maximum, label);
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.front() == '/' ||
        (value.size() >= 2 && std::isalpha(static_cast<unsigned char>(value[0])) != 0 && value[1] == ':'))
        throw error("EDIT_INVALID", std::string(label) + " must be relative");
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find('/', begin);
        const auto part = value.substr(begin, end == std::string::npos ? value.size() - begin : end - begin);
        if (part.empty() || part == "." || part == "..")
            throw error("EDIT_INVALID", std::string(label) + " contains an unsafe path component");
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return value;
}

[[nodiscard]] std::string safeKey(std::string value, std::size_t maximum,
                                  std::string_view label) {
    return safeText(std::move(value), maximum, label);
}

template <std::size_t N>
void validateVector(const std::array<float, N>& value, std::string_view label) {
    for (const auto component : value)
        if (!std::isfinite(component)) throw error("EDIT_INVALID", std::string(label) + " must be finite");
}

void validateMatrix(const Matrix4& value, std::string_view label) {
    validateVector(value, label);
}

template <typename T>
void validateOptionalFloat(const std::optional<T>& value, std::string_view label) {
    if (value.has_value() && !std::isfinite(static_cast<float>(*value)))
        throw error("EDIT_INVALID", std::string(label) + " must be finite");
}

void validateNodeEdit(NodeEdit& edit, const AuthoringLimits& limits) {
    if (!edit.name && !edit.active && !edit.transform)
        throw error("EDIT_INVALID", "node edit has no fields");
    if (edit.name) *edit.name = safeText(std::move(*edit.name), limits.maxStringBytes, "node name");
    if (edit.transform) validateMatrix(*edit.transform, "node transform");
}

void validateWorkspaceEdit(WorkspaceFileEdit& edit, const AuthoringLimits& limits) {
    if (edit.name) *edit.name = safePath(std::move(*edit.name), limits.maxStringBytes, "workspace file name");
    if (edit.position) validateVector(*edit.position, "workspace position");
    if (edit.rotation) validateVector(*edit.rotation, "workspace rotation");
    if (edit.positionCenter) validateVector(*edit.positionCenter, "workspace position center");
    if (edit.positionRange) validateVector(*edit.positionRange, "workspace position range");
    if (edit.velocityBase) validateVector(*edit.velocityBase, "workspace velocity base");
    if (edit.velocityRange) validateVector(*edit.velocityRange, "workspace velocity range");
    validateOptionalFloat(edit.lodIn, "workspace LOD in");
    validateOptionalFloat(edit.lodOut, "workspace LOD out");
    validateOptionalFloat(edit.probability, "workspace probability");
    if (edit.multiplicity) validateVector(*edit.multiplicity, "workspace multiplicity");
    if (edit.posMode) *edit.posMode = upperAscii(safeText(std::move(*edit.posMode), limits.maxStringBytes, "position mode"));
    if (edit.velMode) *edit.velMode = upperAscii(safeText(std::move(*edit.velMode), limits.maxStringBytes, "velocity mode"));
    if (edit.playWav) *edit.playWav = safeText(std::move(*edit.playWav), limits.maxStringBytes, "workspace wav", true);
}

void validateMaterialScalar(MaterialScalar& value, const AuthoringLimits& limits) {
    if (auto* number = std::get_if<float>(&value)) {
        if (!std::isfinite(*number)) throw error("EDIT_INVALID", "material scalar must be finite");
    } else if (auto* text = std::get_if<std::string>(&value)) {
        *text = safeText(std::move(*text), limits.maxStringBytes, "material scalar");
    }
}

void validateMaterialVector(MaterialVector& value) {
    if (value.components < 2 || value.components > value.values.size())
        throw error("EDIT_INVALID", "material vector must contain two to four components");
    validateVector(value.values, "material vector");
}

void validateResource(MaterialResource& value, const AuthoringLimits& limits) {
    if (value.clear) {
        if (value.texture || value.file || value.color)
            throw error("EDIT_INVALID", "cleared material resource cannot contain a value");
        return;
    }
    const auto count = static_cast<unsigned>(value.texture.has_value()) +
                       static_cast<unsigned>(value.file.has_value()) +
                       static_cast<unsigned>(value.color.has_value());
    if (count != 1) throw error("EDIT_INVALID", "material resource must contain one value");
    if (value.texture) *value.texture = safePath(std::move(*value.texture), limits.maxStringBytes, "material texture");
    if (value.file) *value.file = safePath(std::move(*value.file), limits.maxStringBytes, "material resource file");
    if (value.color) validateVector(*value.color, "material resource color");
}

void validateSurface(SurfaceEdit& edit, const AuthoringLimits& limits) {
    if (edit.key) *edit.key = upperAscii(safeKey(std::move(*edit.key), limits.maxStringBytes, "surface key"));
    if (edit.wav) *edit.wav = safeText(std::move(*edit.wav), limits.maxStringBytes, "surface wav", true);
    if (edit.ffEffect) *edit.ffEffect = safeText(std::move(*edit.ffEffect), limits.maxStringBytes, "surface effect", true);
    validateOptionalFloat(edit.friction, "surface friction");
    validateOptionalFloat(edit.damping, "surface damping");
    validateOptionalFloat(edit.dirtAdditive, "surface dirt additive");
    validateOptionalFloat(edit.blackFlagTime, "surface black flag time");
    validateOptionalFloat(edit.sinHeight, "surface sine height");
    validateOptionalFloat(edit.sinLength, "surface sine length");
    validateOptionalFloat(edit.vibrationGain, "surface vibration gain");
    validateOptionalFloat(edit.vibrationLength, "surface vibration length");
    validateOptionalFloat(edit.wavPitch, "surface wav pitch");
}

void validateCollider(ColliderEdit& edit) {
    if (edit.transform) validateMatrix(*edit.transform, "collider transform");
}

void validateBottomCollider(BottomColliderEdit& edit) {
    if (edit.centre) validateVector(*edit.centre, "bottom collider centre");
    if (edit.size) {
        validateVector(*edit.size, "bottom collider size");
        for (const auto component : *edit.size)
            if (component <= 0.0F) throw error("EDIT_INVALID", "bottom collider size must be positive");
    }
}

void validateDamage(DamageEdit& edit, const AuthoringLimits& limits) {
    validateOptionalFloat(edit.minSpeed, "damage minimum speed");
    validateOptionalFloat(edit.maxSpeed, "damage maximum speed");
    validateOptionalFloat(edit.initialLevel, "damage initial level");
    validateOptionalFloat(edit.staticRotationAngle, "damage rotation angle");
    validateOptionalFloat(edit.multG, "damage G multiplier");
    validateOptionalFloat(edit.fullSpeed, "damage full speed");
    validateOptionalFloat(edit.oscillationMinAngle, "damage minimum oscillation");
    validateOptionalFloat(edit.oscillationMaxAngle, "damage maximum oscillation");
    if (edit.initialLevel && (*edit.initialLevel < 0.0F || *edit.initialLevel > 100.0F))
        throw error("EDIT_INVALID", "damage initial level must be from 0 to 100");
    if (edit.minSpeed && *edit.minSpeed < 0.0F) throw error("EDIT_INVALID", "damage minimum speed cannot be negative");
    if (edit.maxSpeed && *edit.maxSpeed < 0.0F) throw error("EDIT_INVALID", "damage maximum speed cannot be negative");
    if (edit.fullSpeed && *edit.fullSpeed < 0.0F) throw error("EDIT_INVALID", "damage full speed cannot be negative");
    if (edit.staticRotationAxis) validateVector(*edit.staticRotationAxis, "damage rotation axis");
    if (edit.oscillationAxis) validateVector(*edit.oscillationAxis, "damage oscillation axis");
    if (edit.allowedG) validateVector(*edit.allowedG, "damage allowed G");
    if (edit.name) *edit.name = safeText(std::move(*edit.name), limits.maxStringBytes, "damage name");
    if (edit.damageZone) {
        *edit.damageZone = upperAscii(safeText(std::move(*edit.damageZone), limits.maxStringBytes, "damage zone"));
        if (edit.damageZone->size() > 64 ||
            !std::all_of(edit.damageZone->begin(), edit.damageZone->end(), [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            }))
            throw error("EDIT_INVALID", "damage zone is not a safe token");
    }
}

template <typename T>
void mergeOptional(std::optional<T>& destination, const std::optional<T>& source) {
    if (source) destination = source;
}

void mergeNode(NodeEdit& destination, const NodeEdit& source) {
    mergeOptional(destination.name, source.name);
    mergeOptional(destination.active, source.active);
    mergeOptional(destination.transform, source.transform);
}

void mergeWorkspace(WorkspaceFileEdit& destination, const WorkspaceFileEdit& source) {
    mergeOptional(destination.name, source.name);
    mergeOptional(destination.position, source.position);
    mergeOptional(destination.rotation, source.rotation);
    mergeOptional(destination.lodIn, source.lodIn);
    mergeOptional(destination.lodOut, source.lodOut);
    mergeOptional(destination.probability, source.probability);
    mergeOptional(destination.multiplicity, source.multiplicity);
    mergeOptional(destination.posMode, source.posMode);
    mergeOptional(destination.positionCenter, source.positionCenter);
    mergeOptional(destination.positionRange, source.positionRange);
    mergeOptional(destination.velMode, source.velMode);
    mergeOptional(destination.velocityBase, source.velocityBase);
    mergeOptional(destination.velocityRange, source.velocityRange);
    mergeOptional(destination.playWav, source.playWav);
}

void mergeSurface(SurfaceEdit& destination, const SurfaceEdit& source) {
    mergeOptional(destination.key, source.key);
    mergeOptional(destination.friction, source.friction);
    mergeOptional(destination.damping, source.damping);
    mergeOptional(destination.dirtAdditive, source.dirtAdditive);
    mergeOptional(destination.blackFlagTime, source.blackFlagTime);
    mergeOptional(destination.isValidTrack, source.isValidTrack);
    mergeOptional(destination.isPitlane, source.isPitlane);
    mergeOptional(destination.sinHeight, source.sinHeight);
    mergeOptional(destination.sinLength, source.sinLength);
    mergeOptional(destination.vibrationGain, source.vibrationGain);
    mergeOptional(destination.vibrationLength, source.vibrationLength);
    mergeOptional(destination.wav, source.wav);
    mergeOptional(destination.wavPitch, source.wavPitch);
    mergeOptional(destination.ffEffect, source.ffEffect);
}

void mergeCollider(ColliderEdit& destination, const ColliderEdit& source) {
    mergeOptional(destination.transform, source.transform);
    mergeOptional(destination.removeDegenerate, source.removeDegenerate);
    mergeOptional(destination.reverseWinding, source.reverseWinding);
    mergeOptional(destination.recalculateNormals, source.recalculateNormals);
}

void mergeBottomCollider(BottomColliderEdit& destination, const BottomColliderEdit& source) {
    mergeOptional(destination.centre, source.centre);
    mergeOptional(destination.size, source.size);
    mergeOptional(destination.groundEnabled, source.groundEnabled);
}

void mergeDamage(DamageEdit& destination, const DamageEdit& source) {
    mergeOptional(destination.minSpeed, source.minSpeed);
    mergeOptional(destination.maxSpeed, source.maxSpeed);
    mergeOptional(destination.initialLevel, source.initialLevel);
    mergeOptional(destination.staticRotationAngle, source.staticRotationAngle);
    mergeOptional(destination.multG, source.multG);
    mergeOptional(destination.fullSpeed, source.fullSpeed);
    mergeOptional(destination.oscillationMinAngle, source.oscillationMinAngle);
    mergeOptional(destination.oscillationMaxAngle, source.oscillationMaxAngle);
    mergeOptional(destination.staticRotationAxis, source.staticRotationAxis);
    mergeOptional(destination.oscillationAxis, source.oscillationAxis);
    mergeOptional(destination.allowedG, source.allowedG);
    mergeOptional(destination.enabled, source.enabled);
    mergeOptional(destination.name, source.name);
    mergeOptional(destination.damageZone, source.damageZone);
}

[[nodiscard]] std::string nodePath(std::string value, std::size_t maximum) {
    value = safeText(std::move(value), maximum, "node path");
    if (value == "root") return value;
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto end = value.find('/', begin);
        const auto part = value.substr(begin, end == std::string::npos ? value.size() - begin : end - begin);
        if (part.empty() || !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
            throw error("EDIT_INVALID", "node path is not a stable hierarchy path");
        if (part.size() > 1u && part.front() == '0')
            throw error("EDIT_INVALID", "node path contains a non-canonical index");
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return value;
}

[[nodiscard]] std::string sourceName(std::string value, std::size_t maximum) {
    value = safePath(std::move(value), maximum, "source name");
    return value;
}

void validateState(ProjectState& state, const AuthoringLimits& limits) {
    state.source = normalizeSourceIdentity(std::move(state.source));
    for (auto& [path, edit] : state.nodes) {
        if (nodePath(path, limits.maxStringBytes) != path)
            throw error("RECOVERY_INVALID", "recovery node path is not canonical");
        validateNodeEdit(edit, limits);
    }
    for (auto& [index, edit] : state.workspaceFiles) {
        (void)index;
        validateWorkspaceEdit(edit, limits);
    }
    for (auto& [material, edit] : state.materials) {
        if (safeKey(material, limits.maxStringBytes, "material key") != material)
            throw error("RECOVERY_INVALID", "recovery material key is not canonical");
        for (auto& [field, value] : edit.scalars) {
            if (safeKey(field, limits.maxStringBytes, "material scalar field") != field)
                throw error("RECOVERY_INVALID", "recovery material field is not canonical");
            validateMaterialScalar(value, limits);
        }
        for (auto& [field, value] : edit.vectors) {
            if (safeKey(field, limits.maxStringBytes, "material vector field") != field)
                throw error("RECOVERY_INVALID", "recovery material field is not canonical");
            validateMaterialVector(value);
        }
        for (auto& [slot, value] : edit.resources) {
            if (safeKey(slot, limits.maxStringBytes, "material resource slot") != slot)
                throw error("RECOVERY_INVALID", "recovery material slot is not canonical");
            validateResource(value, limits);
        }
    }
    for (auto& [index, edit] : state.surfaces) {
        (void)index;
        validateSurface(edit, limits);
    }
    for (auto& [index, edit] : state.colliders) {
        (void)index;
        validateCollider(edit);
    }
    for (auto& [index, edit] : state.bottomColliders) {
        (void)index;
        validateBottomCollider(edit);
    }
    for (auto& [section, edit] : state.damage) {
        if (upperAscii(safeKey(section, limits.maxStringBytes, "damage section")) != section)
            throw error("RECOVERY_INVALID", "recovery damage section is not canonical");
        validateDamage(edit, limits);
    }
}

template <typename T>
[[nodiscard]] std::size_t optionalCount(const std::optional<T>& value) { return value.has_value() ? 1U : 0U; }

[[nodiscard]] std::uint64_t nextRevision(std::uint64_t revision) {
    if (revision == std::numeric_limits<std::uint64_t>::max())
        throw error("REVISION_LIMIT", "project revision cannot advance beyond uint64 maximum");
    return revision + 1u;
}

} // namespace

SourceIdentity normalizeSourceIdentity(SourceIdentity identity) {
    identity.name = sourceName(std::move(identity.name), 4096);
    identity.sha256 = lowerAscii(trim(identity.sha256));
    if (identity.sha256.size() != 64 ||
        !std::all_of(identity.sha256.begin(), identity.sha256.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw error("SOURCE_IDENTITY_INVALID", "source SHA-256 must contain 64 hexadecimal characters");
    return identity;
}

bool sourceIdentityMatches(const SourceIdentity& expected, const SourceIdentity& actual) {
    if (lowerAscii(expected.name) != lowerAscii(actual.name) || expected.size != actual.size ||
        lowerAscii(expected.sha256) != lowerAscii(actual.sha256))
        return false;
    return !expected.kn5Version.has_value() || !actual.kn5Version.has_value() ||
           expected.kn5Version == actual.kn5Version;
}

ProjectSession::ProjectSession(SourceIdentity source, AuthoringLimits limits)
    : source_(normalizeSourceIdentity(std::move(source))), limits_(limits) {
    if (limits_.maxStringBytes == 0 || limits_.maxOperationsPerTransaction == 0)
        throw error("LIMIT_INVALID", "authoring string and operation limits must be positive");
    current_.source = source_;
}

ProjectState ProjectSession::baselineState() const {
    ProjectState baseline;
    baseline.source = source_;
    return baseline;
}

ProjectState ProjectSession::applyTransaction(const AuthoringTransaction& transaction) const {
    if (transaction.label.size() > limits_.maxStringBytes)
        throw error("TRANSACTION_INVALID", "transaction label exceeds its limit");
    if (transaction.operations.size() > limits_.maxOperationsPerTransaction)
        throw error("TRANSACTION_LIMIT", "transaction operation count exceeds its limit");
    ProjectState candidate = current_;
    for (const auto& operation : transaction.operations) {
        std::visit([&](auto operationValue) {
            using Operation = std::decay_t<decltype(operationValue)>;
            if constexpr (std::is_same_v<Operation, SetNodeEdit>) {
                operationValue.path = nodePath(std::move(operationValue.path), limits_.maxStringBytes);
                validateNodeEdit(operationValue.edit, limits_);
                mergeNode(candidate.nodes[operationValue.path], operationValue.edit);
            } else if constexpr (std::is_same_v<Operation, ClearNodeEdit>) {
                candidate.nodes.erase(nodePath(std::move(operationValue.path), limits_.maxStringBytes));
            } else if constexpr (std::is_same_v<Operation, SetWorkspaceFileEdit>) {
                validateWorkspaceEdit(operationValue.edit, limits_);
                mergeWorkspace(candidate.workspaceFiles[operationValue.index], operationValue.edit);
            } else if constexpr (std::is_same_v<Operation, ClearWorkspaceFileEdit>) {
                candidate.workspaceFiles.erase(operationValue.index);
            } else if constexpr (std::is_same_v<Operation, SetWorkspaceSettingsEdit>) {
                validateOptionalFloat(operationValue.edit.cockpitHrDistance, "cockpit distance");
                validateOptionalFloat(operationValue.edit.driverHrDistance, "driver distance");
                if (!operationValue.edit.cockpitHrDistance && !operationValue.edit.driverHrDistance)
                    throw error("EDIT_INVALID", "workspace settings edit has no fields");
                mergeOptional(candidate.workspace.cockpitHrDistance, operationValue.edit.cockpitHrDistance);
                mergeOptional(candidate.workspace.driverHrDistance, operationValue.edit.driverHrDistance);
            } else if constexpr (std::is_same_v<Operation, ClearWorkspaceSettingsEdit>) {
                candidate.workspace = {};
            } else if constexpr (std::is_same_v<Operation, SetMaterialScalarEdit>) {
                const auto material = safeKey(std::move(operationValue.material), limits_.maxStringBytes, "material key");
                const auto field = safeKey(std::move(operationValue.field), limits_.maxStringBytes, "material scalar field");
                validateMaterialScalar(operationValue.value, limits_);
                candidate.materials[material].scalars[field] = std::move(operationValue.value);
            } else if constexpr (std::is_same_v<Operation, SetMaterialVectorEdit>) {
                const auto material = safeKey(std::move(operationValue.material), limits_.maxStringBytes, "material key");
                const auto field = safeKey(std::move(operationValue.field), limits_.maxStringBytes, "material vector field");
                validateMaterialVector(operationValue.value);
                candidate.materials[material].vectors[field] = operationValue.value;
            } else if constexpr (std::is_same_v<Operation, SetMaterialResourceEdit>) {
                const auto material = safeKey(std::move(operationValue.material), limits_.maxStringBytes, "material key");
                const auto slot = safeKey(std::move(operationValue.slot), limits_.maxStringBytes, "material resource slot");
                validateResource(operationValue.value, limits_);
                if (operationValue.value.clear) candidate.materials[material].resources.erase(slot);
                else candidate.materials[material].resources[slot] = std::move(operationValue.value);
            } else if constexpr (std::is_same_v<Operation, ClearMaterialEdit>) {
                candidate.materials.erase(safeKey(std::move(operationValue.material), limits_.maxStringBytes, "material key"));
            } else if constexpr (std::is_same_v<Operation, SetSurfaceEdit>) {
                validateSurface(operationValue.edit, limits_);
                mergeSurface(candidate.surfaces[operationValue.index], operationValue.edit);
            } else if constexpr (std::is_same_v<Operation, ClearSurfaceEdit>) {
                candidate.surfaces.erase(operationValue.index);
            } else if constexpr (std::is_same_v<Operation, SetColliderEdit>) {
                validateCollider(operationValue.edit);
                if (!operationValue.edit.transform && !operationValue.edit.removeDegenerate &&
                    !operationValue.edit.reverseWinding && !operationValue.edit.recalculateNormals)
                    throw error("EDIT_INVALID", "collider edit has no fields");
                mergeCollider(candidate.colliders[operationValue.index], operationValue.edit);
            } else if constexpr (std::is_same_v<Operation, ClearColliderEdit>) {
                candidate.colliders.erase(operationValue.index);
            } else if constexpr (std::is_same_v<Operation, SetBottomColliderEdit>) {
                validateBottomCollider(operationValue.edit);
                if (!operationValue.edit.centre && !operationValue.edit.size && !operationValue.edit.groundEnabled)
                    throw error("EDIT_INVALID", "bottom collider edit has no fields");
                mergeBottomCollider(candidate.bottomColliders[operationValue.index], operationValue.edit);
            } else if constexpr (std::is_same_v<Operation, ClearBottomColliderEdit>) {
                candidate.bottomColliders.erase(operationValue.index);
            } else if constexpr (std::is_same_v<Operation, SetDamageEdit>) {
                const auto section = upperAscii(safeKey(std::move(operationValue.section), limits_.maxStringBytes, "damage section"));
                validateDamage(operationValue.edit, limits_);
                mergeDamage(candidate.damage[section], operationValue.edit);
            } else if constexpr (std::is_same_v<Operation, ClearDamageEdit>) {
                candidate.damage.erase(upperAscii(safeKey(std::move(operationValue.section), limits_.maxStringBytes, "damage section")));
            }
        }, operation);
        if (editCount(candidate) > limits_.maxTotalEdits)
            throw error("EDIT_LIMIT", "project edit count exceeds its limit");
    }
    return candidate;
}

std::size_t ProjectSession::editCount(const ProjectState& state) const {
    std::size_t count = optionalCount(state.workspace.cockpitHrDistance) +
                        optionalCount(state.workspace.driverHrDistance);
    for (const auto& [index, edit] : state.workspaceFiles) {
        (void)index;
        count += 1 + optionalCount(edit.name) + optionalCount(edit.position) + optionalCount(edit.rotation) +
                 optionalCount(edit.lodIn) + optionalCount(edit.lodOut) + optionalCount(edit.probability) +
                 optionalCount(edit.multiplicity) + optionalCount(edit.posMode) + optionalCount(edit.positionCenter) +
                 optionalCount(edit.positionRange) + optionalCount(edit.velMode) + optionalCount(edit.velocityBase) +
                 optionalCount(edit.velocityRange) + optionalCount(edit.playWav);
    }
    for (const auto& [key, edit] : state.nodes) count += 1 + optionalCount(edit.name) + optionalCount(edit.active) + optionalCount(edit.transform);
    for (const auto& [key, edit] : state.materials) {
        (void)key;
        count += 1 + edit.scalars.size() + edit.vectors.size() + edit.resources.size();
    }
    count += state.surfaces.size() + state.colliders.size() + state.bottomColliders.size() + state.damage.size();
    return count;
}

void ProjectSession::pushUndo(const ProjectState& state) {
    if (limits_.maxHistory == 0) return;
    if (undo_.size() >= limits_.maxHistory) undo_.pop_front();
    undo_.push_back(state);
}

std::uint64_t ProjectSession::commit(const AuthoringTransaction& transaction) {
    if (transaction.operations.empty()) return current_.revision;
    (void)safeText(transaction.label, limits_.maxStringBytes, "transaction label", true);
    const auto candidate = applyTransaction(transaction);
    const auto revision = nextRevision(current_.revision);
    pushUndo(current_);
    current_ = candidate;
    current_.revision = revision;
    redo_.clear();
    return current_.revision;
}

std::uint64_t ProjectSession::undo() {
    if (undo_.empty()) return current_.revision;
    if (redo_.size() >= limits_.maxHistory && limits_.maxHistory != 0) redo_.pop_front();
    const auto revision = nextRevision(current_.revision);
    redo_.push_back(current_);
    current_ = undo_.back();
    undo_.pop_back();
    current_.revision = revision;
    return current_.revision;
}

std::uint64_t ProjectSession::redo() {
    if (redo_.empty()) return current_.revision;
    const auto revision = nextRevision(current_.revision);
    pushUndo(current_);
    current_ = redo_.back();
    redo_.pop_back();
    current_.revision = revision;
    return current_.revision;
}

std::uint64_t ProjectSession::restoreBaseline() {
    const auto baseline = baselineState();
    if (editCount(current_) == 0) return current_.revision;
    const auto revision = nextRevision(current_.revision);
    pushUndo(current_);
    current_ = baseline;
    current_.revision = revision;
    redo_.clear();
    return current_.revision;
}

RecoverySnapshot ProjectSession::recoverySnapshot() const { return {current_}; }

RecoveryResult ProjectSession::validateRecoveryInternal(const RecoverySnapshot& snapshot,
                                                         const SourceIdentity& observedSource) const {
    RecoveryResult result;
    try {
        const auto observed = normalizeSourceIdentity(observedSource);
        auto candidate = snapshot.state;
        // Compare the same canonical representation that recovery will
        // install.  Otherwise harmless path separators and hex casing make a
        // valid snapshot look stale, while recover() later normalizes it.
        validateState(candidate, limits_);
        if (!sourceIdentityMatches(source_, observed))
            result.diagnostics.push_back({"STALE_SOURCE", "observed source identity does not match the open source"});
        if (!sourceIdentityMatches(source_, candidate.source))
            result.diagnostics.push_back({"STALE_SOURCE", "recovery snapshot belongs to a different source"});
        if (editCount(candidate) > limits_.maxRecoveryEdits)
            result.diagnostics.push_back({"RECOVERY_LIMIT", "recovery snapshot exceeds the edit limit"});
        if (editCount(candidate) > limits_.maxTotalEdits)
            result.diagnostics.push_back({"EDIT_LIMIT", "recovery snapshot exceeds the project edit limit"});
    } catch (const AuthoringError& errorValue) {
        result.diagnostics.push_back({errorValue.code(), errorValue.what()});
    }
    result.restored = result.diagnostics.empty();
    return result;
}

RecoveryResult ProjectSession::validateRecovery(const RecoverySnapshot& snapshot,
                                                const SourceIdentity& observedSource) const {
    return validateRecoveryInternal(snapshot, observedSource);
}

RecoveryResult ProjectSession::recover(const RecoverySnapshot& snapshot,
                                       const SourceIdentity& observedSource) {
    auto result = validateRecoveryInternal(snapshot, observedSource);
    if (!result.restored) return result;
    auto candidate = snapshot.state;
    validateState(candidate, limits_);
    const auto revision = nextRevision(current_.revision);
    pushUndo(current_);
    current_ = std::move(candidate);
    current_.revision = revision;
    redo_.clear();
    return result;
}

} // namespace apex::authoring
