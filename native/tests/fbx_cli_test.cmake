if(NOT DEFINED APEX_NATIVE_COMMAND OR NOT DEFINED APEX_BINARY_DIR)
  message(FATAL_ERROR "APEX_NATIVE_COMMAND and APEX_BINARY_DIR are required")
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
}
Connections: {
 C: "OO", 100, 200
 C: "OO", 300, 200
 C: "OP", 400, 300, "DiffuseColor"
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
