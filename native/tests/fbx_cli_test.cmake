if(NOT DEFINED APEX_NATIVE_COMMAND OR NOT DEFINED APEX_BINARY_DIR OR
   NOT DEFINED APEX_SOURCE_DIR)
  message(FATAL_ERROR
    "APEX_NATIVE_COMMAND, APEX_BINARY_DIR, and APEX_SOURCE_DIR are required")
endif()

set(test_root "${APEX_BINARY_DIR}/fbx-cli-test")
file(MAKE_DIRECTORY "${test_root}")
set(valid_fbx "${test_root}/triangle.fbx")
set(truncated_fbx "${test_root}/truncated.fbx")

file(WRITE "${valid_fbx}" [=[
FBXVersion: 7400
Objects: {
 Model: 200, "Model::Triangle", "Mesh" { }
 Geometry: 100, "Geometry::Triangle", "Mesh" {
  Vertices: *9 { a: 0.0,0.0,0.0,1.0,0.0,0.0,0.0,1.0,0.0 }
  PolygonVertexIndex: *3 { a: 0,1,-3 }
  LayerElementNormal: 0 {
   MappingInformationType: "ByPolygonVertex"
   ReferenceInformationType: "Direct"
   Normals: *9 { a: 0.0,0.0,1.0,0.0,0.0,1.0,0.0,0.0,1.0 }
  }
  LayerElementUV: 0 {
   MappingInformationType: "ByPolygonVertex"
   ReferenceInformationType: "IndexToDirect"
   UV: *6 { a: 0.0,0.0,1.0,0.0,0.0,1.0 }
   UVIndex: *3 { a: 0,1,2 }
  }
  LayerElementMaterial: 0 {
   MappingInformationType: "AllSame"
   ReferenceInformationType: "IndexToDirect"
   Materials: *1 { a: 0 }
  }
 }
 Material: 300, "Material::Paint", "Material" {
  ShadingModel: "Phong"
  Properties70: {
   P: "AmbientColor", "ColorRGB", "Color", "", 0.2,0.2,0.2
   P: "DiffuseColor", "ColorRGB", "Color", "", 0.8,0.8,0.8
   P: "SpecularColor", "ColorRGB", "Color", "", 0.4,0.4,0.4
   P: "Shininess", "double", "Number", "", 20
  }
 }
 Texture: 400, "Texture::Paint", "TextureVideoClip" {
  FileName: "C:\\car\\texture\\paint.dds"
 }
 AnimationStack: 500, "AnimationStack::Take 001", "AnimationStack" {
  LocalStart: 0
  LocalStop: 100
 }
 AnimationLayer: 501, "AnimationLayer::BaseLayer", "AnimationLayer" { }
 AnimationCurveNode: 502, "AnimationCurveNode::T", "AnimationCurveNode" { }
 AnimationCurve: 503, "AnimationCurve::TX", "AnimationCurve" {
  KeyTime: *2 { a: 0,100 }
  KeyValueFloat: *2 { a: 0.0,10.0 }
  KeyAttrFlags: *2 { a: 4,4 }
 }
}
Connections: {
 C: "OO", 100, 200
 C: "OO", 300, 200
 C: "OP", 400, 300, "DiffuseColor"
 C: "OO", 501, 500
 C: "OO", 502, 501
 C: "OP", 502, 200, "Lcl Translation"
 C: "OP", 503, 502, "d|X"
}
]=])
file(WRITE "${truncated_fbx}" "Kaydara FBX Binary")

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --inspect-fbx "${valid_fbx}"
  RESULT_VARIABLE staged_result
  OUTPUT_VARIABLE staged_output
  ERROR_VARIABLE staged_error
)
if(NOT staged_result STREQUAL "0")
  message(FATAL_ERROR
    "valid staged FBX returned ${staged_result}: ${staged_error}")
endif()
string(FIND "${staged_output}" "FBX staged" staged_status_position)
if(staged_status_position EQUAL -1)
  message(FATAL_ERROR
    "valid staged FBX did not report its status: ${staged_output}")
endif()
string(FIND "${staged_output}" "materials" staged_summary_position)
if(staged_summary_position EQUAL -1)
  message(FATAL_ERROR
    "valid staged FBX did not report its scene summary: ${staged_output}")
endif()
string(FIND "${staged_output}"
  "animation[0]: name=\"Take 001\"" animation_summary_position)
if(animation_summary_position EQUAL -1)
  message(FATAL_ERROR
    "valid FBX did not report animation metadata: ${staged_output}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --inspect-fbx "${truncated_fbx}"
  RESULT_VARIABLE truncated_result
  OUTPUT_VARIABLE truncated_output
  ERROR_VARIABLE truncated_error
)
if(NOT truncated_result STREQUAL "1")
  message(FATAL_ERROR
    "truncated FBX returned ${truncated_result}: ${truncated_error}")
endif()
string(FIND "${truncated_output}" "FBX invalid_request"
  truncated_status_position)
if(truncated_status_position EQUAL -1)
  message(FATAL_ERROR
    "truncated FBX did not report invalid_request: ${truncated_output}")
endif()
string(FIND "${truncated_error}" "recognized FBX header"
  truncated_diagnostic_position)
if(truncated_diagnostic_position EQUAL -1)
  message(FATAL_ERROR
    "truncated FBX did not report a parser diagnostic: ${truncated_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --inspect-fbx "${valid_fbx}"
          --fbx-assets
  RESULT_VARIABLE malformed_result
  OUTPUT_VARIABLE malformed_output
  ERROR_VARIABLE malformed_error
)
if(NOT malformed_result STREQUAL "2")
  message(FATAL_ERROR
    "malformed FBX inspection returned ${malformed_result}: ${malformed_error}")
endif()
string(FIND "${malformed_error}" "Usage:" malformed_usage_position)
if(malformed_usage_position EQUAL -1)
  message(FATAL_ERROR
    "malformed FBX inspection did not print usage: ${malformed_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --fbx "${valid_fbx}"
  RESULT_VARIABLE missing_assets_result
  ERROR_VARIABLE missing_assets_error
)
if(NOT missing_assets_result STREQUAL "1")
  message(FATAL_ERROR
    "window FBX without assets returned ${missing_assets_result}: ${missing_assets_error}")
endif()
string(FIND "${missing_assets_error}"
  "--fbx requires --fbx-assets" missing_assets_position)
if(missing_assets_position EQUAL -1)
  message(FATAL_ERROR
    "window FBX did not require explicit assets: ${missing_assets_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --fbx-assets "${test_root}"
  RESULT_VARIABLE detached_assets_result
  ERROR_VARIABLE detached_assets_error
)
if(NOT detached_assets_result STREQUAL "1")
  message(FATAL_ERROR
    "detached FBX assets returned ${detached_assets_result}: ${detached_assets_error}")
endif()
string(FIND "${detached_assets_error}"
  "--fbx-assets requires --fbx" detached_assets_position)
if(detached_assets_position EQUAL -1)
  message(FATAL_ERROR
    "detached FBX assets were not rejected: ${detached_assets_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --model model.kn5 --fbx "${valid_fbx}"
          --fbx-assets "${test_root}"
  RESULT_VARIABLE combined_source_result
  ERROR_VARIABLE combined_source_error
)
if(NOT combined_source_result STREQUAL "1")
  message(FATAL_ERROR
    "combined model sources returned ${combined_source_result}: ${combined_source_error}")
endif()
string(FIND "${combined_source_error}"
  "mutually exclusive" combined_source_position)
if(combined_source_position EQUAL -1)
  message(FATAL_ERROR
    "combined model sources were not rejected: ${combined_source_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --model "${valid_fbx}"
          --shader-family ksPerPixel
          --shader-vertex missing.vert --shader-fragment missing.frag
  RESULT_VARIABLE legacy_model_result
  ERROR_VARIABLE legacy_model_error
)
if(NOT legacy_model_result STREQUAL "1")
  message(FATAL_ERROR
    "FBX passed through --model returned ${legacy_model_result}: ${legacy_model_error}")
endif()
string(FIND "${legacy_model_error}"
  "workspace open failed" legacy_model_position)
if(legacy_model_position EQUAL -1)
  message(FATAL_ERROR
    "--model no longer retains its KN5-only contract: ${legacy_model_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --fbx "${valid_fbx}" --fbx-assets "${test_root}"
          --animation unsupported.ksanim
          --shader-family ksPerPixel
          --shader-vertex missing.vert --shader-fragment missing.frag
  RESULT_VARIABLE fbx_animation_result
  ERROR_VARIABLE fbx_animation_error
)
if(NOT fbx_animation_result STREQUAL "1")
  message(FATAL_ERROR
    "FBX animation option returned ${fbx_animation_result}: ${fbx_animation_error}")
endif()
string(FIND "${fbx_animation_error}"
  "--animation is not supported with --fbx" fbx_animation_position)
if(fbx_animation_position EQUAL -1)
  message(FATAL_ERROR
    "unsupported FBX animation was not rejected: ${fbx_animation_error}")
endif()

set(empty_assets "${test_root}/empty-assets")
file(MAKE_DIRECTORY "${empty_assets}")
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --fbx "${valid_fbx}" --fbx-assets "${empty_assets}"
          --shader-family ksPerPixel
          --shader-vertex missing.vert --shader-fragment missing.frag
  RESULT_VARIABLE staged_window_result
  ERROR_VARIABLE staged_window_error
)
if(NOT staged_window_result STREQUAL "1")
  message(FATAL_ERROR
    "staged window FBX returned ${staged_window_result}: ${staged_window_error}")
endif()
string(FIND "${staged_window_error}"
  "FBX window preview is staged" staged_window_position)
if(staged_window_position EQUAL -1)
  message(FATAL_ERROR
    "staged FBX was not rejected before backend creation: ${staged_window_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --fbx "${truncated_fbx}" --fbx-assets "${empty_assets}"
          --shader-family ksPerPixel
          --shader-vertex missing.vert --shader-fragment missing.frag
  RESULT_VARIABLE invalid_window_result
  ERROR_VARIABLE invalid_window_error
)
if(NOT invalid_window_result STREQUAL "1")
  message(FATAL_ERROR
    "invalid window FBX returned ${invalid_window_result}: ${invalid_window_error}")
endif()
string(FIND "${invalid_window_error}"
  "recognized FBX header" invalid_window_position)
if(invalid_window_position EQUAL -1)
  message(FATAL_ERROR
    "invalid FBX did not fail before backend creation: ${invalid_window_error}")
endif()

set(ready_assets "${test_root}/ready-assets")
file(MAKE_DIRECTORY "${ready_assets}")
file(COPY_FILE
  "${APEX_SOURCE_DIR}/test/content/cars/619_gen6_arca_base/texture/NULL.dds"
  "${ready_assets}/paint.dds" ONLY_IF_DIFFERENT)
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --fbx "${valid_fbx}" --fbx-assets "${ready_assets}"
          --fbx-animation 0 --animation-position 0.5
          --shader-family ksPerPixel
          --shader-vertex missing.vert --shader-fragment missing.frag
  RESULT_VARIABLE ready_window_result
  OUTPUT_VARIABLE ready_window_output
  ERROR_VARIABLE ready_window_error
)
if(NOT ready_window_result STREQUAL "1")
  message(FATAL_ERROR
    "ready window FBX seam returned ${ready_window_result}: ${ready_window_error}")
endif()
string(FIND "${ready_window_error}"
  "cannot open missing.vert" ready_shader_position)
if(ready_shader_position EQUAL -1)
  message(FATAL_ERROR
    "ready FBX did not reach bounded shader loading: ${ready_window_error}")
endif()
string(FIND "${ready_window_error}"
  "FBX window preview is" ready_staged_position)
if(NOT ready_staged_position EQUAL -1)
  message(FATAL_ERROR
    "ready FBX was incorrectly staged: ${ready_window_error}")
endif()
string(FIND "${ready_window_output}"
  "FBX animation: index=0, name=\"Take 001\"" ready_animation_position)
if(ready_animation_position EQUAL -1)
  message(FATAL_ERROR
    "ready FBX did not apply its selected animation: ${ready_window_output}")
endif()
string(FIND "${ready_window_output}"
  "matched-nodes=1, position=0.5" ready_animation_pose_position)
if(ready_animation_pose_position EQUAL -1)
  message(FATAL_ERROR
    "ready FBX did not report its applied pose: ${ready_window_output}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --fbx "${valid_fbx}" --fbx-assets "${ready_assets}"
          --fbx-animation 1
          --shader-family ksPerPixel
          --shader-vertex missing.vert --shader-fragment missing.frag
  RESULT_VARIABLE missing_animation_result
  ERROR_VARIABLE missing_animation_error
)
if(NOT missing_animation_result STREQUAL "1")
  message(FATAL_ERROR
    "missing FBX animation returned ${missing_animation_result}: ${missing_animation_error}")
endif()
string(FIND "${missing_animation_error}"
  "selected FBX animation index is not present" missing_animation_position)
if(missing_animation_position EQUAL -1)
  message(FATAL_ERROR
    "missing FBX animation did not fail before shader loading: ${missing_animation_error}")
endif()
