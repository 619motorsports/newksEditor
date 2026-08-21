#include "apex/domain/car_damage.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using apex::domain::BottomCollider;
using apex::domain::BottomColliderConfig;
using apex::domain::BottomColliderEdit;
using apex::domain::BottomColliderEdits;
using apex::domain::CarDamageConfig;
using apex::domain::CarDamageEdits;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void parsesKnownSchemasAndRetainsUnknownData() {
    const std::string text = R"ini(; comment
[SCRATCHES]
MIN_SPEED=1
MAX_SPEED=22
UNKNOWN = retained
[OSCILLATIONS]
ENABLED=0
[DAMAGE]
INITIAL_LEVEL=25
[VISUAL_OBJECT_1]
NAME=Door
STATIC_ROTATION_AXIS=0, 1, 0
STATIC_ROTATION_ANGLE=12.5
MULT_G=2
DAMAGE_ZONE=front-left
MIN_SPEED=3
FULL_SPEED=8
OSCILLATION_AXIS=1,0,0
OSCILLATION_MIN_ANGLE=-1
OSCILLATION_MAX_ANGLE=3
ALLOWED_G=1, 2, 3
CUSTOM=kept
[EXTRA]
VALUE=hello
)ini";
    const auto parsed = apex::domain::parse_car_damage_ini(text);
    require(parsed.config.has_value(), "damage config parses");
    require(parsed.config->scratches.max_speed == 22.0F && !parsed.config->oscillations.enabled &&
                parsed.config->damage.initial_level == 25.0F,
            "known sections parse");
    require(parsed.config->visual_objects.size() == 1 && parsed.config->visual_objects.front().damage_zone == "FRONT-LEFT",
            "visual object schema parses and normalizes token");
    require(parsed.config->visual_objects.front().extra_entries.size() == 1 && parsed.config->extra_sections.size() == 1,
            "unknown fields and sections are retained");
    const auto serialized = apex::domain::serialize_car_damage_ini(*parsed.config);
    require(serialized.find("CUSTOM=kept") != std::string::npos && serialized.find("[EXTRA]") != std::string::npos,
            "unknown data serializes deterministically");
}

void preservesDocumentedDefaultsForMissingKnownFields() {
    const auto parsed = apex::domain::parse_car_damage_ini("[VISUAL_OBJECT_2]\nNAME=wheel\n");
    require(parsed.config.has_value(), "visual object with omitted optional fields parses");
    require(parsed.config->visual_objects.size() == 1 && parsed.config->visual_objects.front().allowed_g == apex::domain::Vector3{1.0F, 1.0F, 1.0F} &&
                parsed.config->visual_objects.front().damage_zone == "FRONT",
            "missing known fields use documented defaults");
    for (const auto& diagnostic : parsed.diagnostics)
        require(diagnostic.code != "NUMBER_INVALID" && diagnostic.code != "VECTOR_INVALID", "missing known fields are not diagnosed");
}

void rejectsMalformedHeadersAndDuplicateNumericIndices() {
    const auto malformed = apex::domain::parse_car_damage_ini("[EXTRA[INJECTED]\nVALUE=one\n");
    require(!malformed.config.has_value() && !malformed.diagnostics.empty() && malformed.diagnostics.front().code == "SECTION_INVALID",
            "section brackets are rejected atomically");
    const auto duplicate = apex::domain::parse_car_damage_ini(
        "[VISUAL_OBJECT_01]\nNAME=first\n[VISUAL_OBJECT_1]\nNAME=second\n");
    require(duplicate.config.has_value() && duplicate.config->visual_objects.size() == 1 &&
                duplicate.config->visual_objects.front().object_name == "first",
            "numeric visual-object aliases use first-wins semantics");
}

void appliesDamageEditsFromStableBaseline() {
    CarDamageConfig config;
    config.scratches.min_speed = 2;
    config.scratches.max_speed = 20;
    config.visual_objects.push_back([] {
        apex::domain::VisualObject object;
        object.index = 0;
        object.name = "VISUAL_OBJECT_0";
        object.object_name = "Mirror";
        return object;
    }());
    const auto baseline = apex::domain::capture_car_damage_baseline(config);
    CarDamageEdits edits;
    edits.values["SCRATCHES"]["maxSpeed"] = "40";
    edits.values["VISUAL_OBJECT_0"]["damageZone"] = "rear";
    require(apex::domain::apply_car_damage_edits(config, edits, baseline) == 2 && config.scratches.max_speed == 40.0F &&
                config.visual_objects.front().damage_zone == "REAR",
            "damage edits apply over baseline");
    CarDamageEdits invalid;
    invalid.values["SCRATCHES"]["maxSpeed"] = "nan";
    bool rejected = false;
    try { (void)apex::domain::apply_car_damage_edits(config, invalid, baseline); }
    catch (const apex::domain::CarDamageError&) { rejected = true; }
    require(rejected && config.scratches.max_speed == 40.0F, "invalid damage edit does not partially mutate state");
    CarDamageEdits extra_vector;
    extra_vector.values["VISUAL_OBJECT_0"]["staticRotationAxis"] = "1,2,3,4";
    rejected = false;
    try { (void)apex::domain::apply_car_damage_edits(config, extra_vector, baseline); }
    catch (const apex::domain::CarDamageError& error) { rejected = error.code() == "EDIT_INVALID"; }
    require(rejected && config.visual_objects.front().static_rotation_axis == apex::domain::Vector3{0.0F, 0.0F, 0.0F},
            "four-component vector edit is rejected atomically");
}

void rejectsUntrustedDamageAtomically() {
    apex::domain::CarDamageLimits limits;
    limits.max_input_bytes = 8;
    const auto oversized = apex::domain::parse_car_damage_ini("[SCRATCHES]", "damage.ini", limits);
    require(!oversized.config.has_value() && oversized.limit_exceeded, "oversized damage input is bounded");
    const auto malformedUtf8 = apex::domain::parse_car_damage_ini(std::string("[X]\nV=\xC3\x28", 8));
    require(!malformedUtf8.config.has_value(), "invalid UTF-8 is rejected atomically");
    const auto truncated = apex::domain::parse_car_damage_ini("[SCRATCHES]\nMIN_SPEED=1\\");
    require(truncated.config.has_value() && !truncated.diagnostics.empty(), "truncated continuation is diagnosed without unsafe growth");
    apex::domain::CarDamageConfig invalid;
    invalid.damage.initial_level = std::numeric_limits<float>::quiet_NaN();
    bool rejected = false;
    try { (void)apex::domain::serialize_car_damage_ini(invalid); }
    catch (const apex::domain::CarDamageError& error) { rejected = error.code() == "NON_FINITE" || error.code() == "LEVEL_RANGE"; }
    require(rejected, "non-finite damage state is rejected");
    apex::domain::CarDamageLimits outputLimits;
    outputLimits.max_output_bytes = 16;
    rejected = false;
    try { (void)apex::domain::serialize_car_damage_ini(CarDamageConfig{}, outputLimits); }
    catch (const apex::domain::CarDamageError& error) { rejected = error.code() == "OUTPUT_LIMIT"; }
    require(rejected, "damage output is bounded");
    apex::domain::CarDamageLimits extraLimits;
    extraLimits.max_extra_entries = 0;
    const auto tooManyExtras = apex::domain::parse_car_damage_ini("[EXTRA]\nVALUE=one\n", "damage.ini", extraLimits);
    require(!tooManyExtras.config.has_value() && tooManyExtras.limit_exceeded, "extra-entry limit fails parsing atomically");
    CarDamageConfig unsafeSection;
    unsafeSection.extra_sections.push_back({"[INJECTED]", {}});
    rejected = false;
    try { (void)apex::domain::serialize_car_damage_ini(unsafeSection); }
    catch (const apex::domain::CarDamageError& error) { rejected = error.code() == "UNSAFE_TEXT"; }
    require(rejected, "extra section names cannot inject INI sections");
    CarDamageConfig aggregateExtras;
    aggregateExtras.source = "src";
    aggregateExtras.scratches.extra_entries.push_back({"ONE", "1"});
    aggregateExtras.damage.extra_entries.push_back({"TWO", "2"});
    apex::domain::CarDamageLimits aggregateLimits;
    aggregateLimits.max_extra_entries = 1;
    rejected = false;
    try { (void)apex::domain::serialize_car_damage_ini(aggregateExtras, aggregateLimits); }
    catch (const apex::domain::CarDamageError& error) { rejected = error.code() == "EXTRA_ENTRY_LIMIT"; }
    require(rejected, "serialized extra-entry limits apply across every section");
    CarDamageConfig longExtra;
    longExtra.source = "src";
    longExtra.scratches.extra_entries.push_back({"LONG_KEY", "x"});
    apex::domain::CarDamageLimits stringLimits;
    stringLimits.max_string_bytes = 3;
    rejected = false;
    try { (void)apex::domain::serialize_car_damage_ini(longExtra, stringLimits); }
    catch (const apex::domain::CarDamageError& error) { rejected = error.code() == "UNSAFE_TEXT"; }
    require(rejected, "serialized extra strings honor max_string_bytes");
    CarDamageConfig tooManySections;
    apex::domain::VisualObject sectionObject;
    sectionObject.object_name = "object";
    tooManySections.visual_objects.push_back(sectionObject);
    tooManySections.extra_sections.push_back({"EXTRA", {}});
    apex::domain::CarDamageLimits sectionLimits;
    sectionLimits.max_sections = 4;
    rejected = false;
    try { (void)apex::domain::serialize_car_damage_ini(tooManySections, sectionLimits); }
    catch (const apex::domain::CarDamageError& error) { rejected = error.code() == "COUNT_LIMIT"; }
    require(rejected, "serialized section limits include known sections");
}

void appliesBottomColliderEditsAndRebuildsBounds() {
    BottomColliderConfig config;
    config.colliders.push_back(BottomCollider{{0, 1, 0}, {2, 4, 6}, true, std::nullopt, std::nullopt});
    const auto baseline = apex::domain::capture_bottom_collider_baseline(config);
    BottomColliderEdits edits;
    edits[0].centre = apex::domain::Vector3{2, 3, 4};
    edits[0].size = apex::domain::Vector3{4, 6, 8};
    edits[0].ground_enabled = false;
    const auto applied = apex::domain::apply_bottom_collider_edits(config, edits, baseline);
    require(applied.applied == 3 && applied.diagnostics.empty() && config.colliders[0].bounds_min.has_value() &&
                config.colliders[0].bounds_max.has_value() && (*config.colliders[0].bounds_min)[0] == 0.0F &&
                (*config.colliders[0].bounds_max)[2] == 8.0F && !config.colliders[0].ground_enabled,
            "bottom collider edits apply and rebuild bounds");
    apex::domain::BottomColliderLimits editLimits;
    editLimits.max_edits = 1;
    const auto overEditLimit = apex::domain::apply_bottom_collider_edits(config, edits, baseline, editLimits);
    require(overEditLimit.applied == 0 && overEditLimit.diagnostics.size() == 1 &&
                overEditLimit.diagnostics.front().code == "LIMIT" && config.colliders[0].centre[0] == 2.0F,
            "bottom collider edit limit counts fields rather than map keys");
    BottomColliderEdits invalid;
    invalid[0].size = apex::domain::Vector3{1, 0, 1};
    const auto failed = apex::domain::apply_bottom_collider_edits(config, invalid, baseline);
    require(failed.applied == 0 && !failed.diagnostics.empty() && config.colliders[0].centre[0] == 2.0F,
            "invalid collider edit is atomic");
}

void rejectsBottomColliderLimitsAndNonFiniteValues() {
    BottomColliderConfig config;
    config.colliders.resize(2);
    apex::domain::BottomColliderLimits limits;
    limits.max_colliders = 1;
    bool rejected = false;
    try { (void)apex::domain::capture_bottom_collider_baseline(config, limits); }
    catch (const apex::domain::CarDamageError& error) { rejected = error.code() == "COLLIDER_LIMIT"; }
    require(rejected, "collider count limit is enforced");
    config.colliders.resize(1);
    config.colliders[0].size[0] = std::numeric_limits<float>::quiet_NaN();
    rejected = false;
    try { (void)apex::domain::capture_bottom_collider_baseline(config); }
    catch (const apex::domain::CarDamageError&) { rejected = true; }
    require(rejected, "non-finite collider state is rejected");
    config.colliders[0].centre = {std::numeric_limits<float>::max(), 0.0F, 0.0F};
    config.colliders[0].size = {std::numeric_limits<float>::max(), 1.0F, 1.0F};
    rejected = false;
    try { (void)apex::domain::capture_bottom_collider_baseline(config); }
    catch (const apex::domain::CarDamageError& error) { rejected = error.code() == "COLLIDER_INVALID"; }
    require(rejected, "collider bounds overflow is rejected atomically");
    BottomColliderConfig retained;
    retained.colliders.push_back(BottomCollider{{0, 0, 0}, {1, 1, 1}, true, std::nullopt, std::nullopt});
    retained.colliders.front().bounds_min = apex::domain::Vector3{-9.0F, -8.0F, -7.0F};
    retained.colliders.front().bounds_max = apex::domain::Vector3{9.0F, 8.0F, 7.0F};
    const auto retainedBaseline = apex::domain::capture_bottom_collider_baseline(retained);
    require(retainedBaseline.config.colliders.front().bounds_min == retained.colliders.front().bounds_min &&
                retainedBaseline.config.colliders.front().bounds_max == retained.colliders.front().bounds_max,
            "valid supplied collider bounds are retained");
    BottomColliderEdits noEdits;
    const auto retainedApplied = apex::domain::apply_bottom_collider_edits(retained, noEdits, retainedBaseline);
    require(retainedApplied.applied == 0 && retainedApplied.diagnostics.empty() &&
                retained.colliders.front().bounds_min == retainedBaseline.config.colliders.front().bounds_min &&
                retained.colliders.front().bounds_max == retainedBaseline.config.colliders.front().bounds_max,
            "no collider edits preserve baseline bounds");
}

} // namespace

int main() {
    try {
        parsesKnownSchemasAndRetainsUnknownData();
        preservesDocumentedDefaultsForMissingKnownFields();
        rejectsMalformedHeadersAndDuplicateNumericIndices();
        appliesDamageEditsFromStableBaseline();
        rejectsUntrustedDamageAtomically();
        appliesBottomColliderEditsAndRebuildsBounds();
        rejectsBottomColliderLimitsAndNonFiniteValues();
        std::cout << "car damage tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "car damage tests failed: " << error.what() << '\n';
        return 1;
    }
}
