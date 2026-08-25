if(NOT DEFINED APEX_NATIVE_COMMAND OR NOT DEFINED APEX_SOURCE_DIR)
  message(FATAL_ERROR "APEX_NATIVE_COMMAND and APEX_SOURCE_DIR are required")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --unknown-option
  RESULT_VARIABLE malformed_result
  OUTPUT_VARIABLE malformed_output
  ERROR_VARIABLE malformed_error
)
if(NOT malformed_result STREQUAL "1")
  message(FATAL_ERROR "unknown workspace-window option returned ${malformed_result}: ${malformed_error}")
endif()
string(FIND "${malformed_error}" "unknown window option" malformed_position)
if(malformed_position EQUAL -1)
  message(FATAL_ERROR "unknown workspace-window option was not diagnosed: ${malformed_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --kind track
  RESULT_VARIABLE kind_result
  OUTPUT_VARIABLE kind_output
  ERROR_VARIABLE kind_error
)
if(NOT kind_result STREQUAL "1")
  message(FATAL_ERROR "detached workspace kind returned ${kind_result}: ${kind_error}")
endif()
string(FIND "${kind_error}" "--kind requires --workspace-root" kind_position)
if(kind_position EQUAL -1)
  message(FATAL_ERROR "detached workspace kind was not diagnosed: ${kind_error}")
endif()

set(model "${APEX_SOURCE_DIR}/test/content/cars/619_gen6_arca_base/619_gen6_fusion13.kn5")
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
  RESULT_VARIABLE missing_modules_result
  OUTPUT_VARIABLE missing_modules_output
  ERROR_VARIABLE missing_modules_error
)
if(NOT missing_modules_result STREQUAL "1")
  message(FATAL_ERROR "missing shader modules returned ${missing_modules_result}: ${missing_modules_error}")
endif()
string(FIND "${missing_modules_error}" "caller-supplied shader modules" missing_modules_position)
if(missing_modules_position EQUAL -1)
  message(FATAL_ERROR "missing shader modules were not diagnosed: ${missing_modules_error}")
endif()

# Exercise the production AssetSource path with a bounded car-LOD manifest.
# The repository's complete car fixture includes optional LOD files that are
# intentionally outside this test's scope, so copy one known-good model into
# a private test workspace and resolve it through a real manifest.
set(workspace_source_root "${APEX_SOURCE_DIR}/test/content/cars/619_gen6_arca_base")
set(workspace_root "${CMAKE_CURRENT_BINARY_DIR}/apex-car-workspace")
file(MAKE_DIRECTORY "${workspace_root}")
file(COPY "${workspace_source_root}/619_gen6_fusion13.kn5" DESTINATION "${workspace_root}")
file(WRITE "${workspace_root}/lods.ini"
  "[LOD_0]\nFILE=619_gen6_fusion13.kn5\nIN=0\nOUT=2000\n")
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --workspace-root "${workspace_root}"
          --manifest data/lods.ini --kind carLods
  RESULT_VARIABLE car_workspace_result
  OUTPUT_VARIABLE car_workspace_output
  ERROR_VARIABLE car_workspace_error
)
if(NOT car_workspace_result STREQUAL "1")
  message(FATAL_ERROR
    "car-LOD workspace load returned ${car_workspace_result}: ${car_workspace_error}")
endif()
string(FIND "${car_workspace_error}" "caller-supplied shader modules" car_workspace_position)
if(car_workspace_position EQUAL -1)
  message(FATAL_ERROR
    "car-LOD workspace did not reach shader validation: ${car_workspace_error}")
endif()
file(REMOVE_RECURSE "${workspace_root}")

# A truncated caller-granted model must fail in the workspace loader, before
# opening a native window or attempting backend initialization.
set(truncated_model "${CMAKE_CURRENT_BINARY_DIR}/apex-truncated-workspace.kn5")
file(WRITE "${truncated_model}" "KN5")
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${truncated_model}"
  RESULT_VARIABLE truncated_result
  OUTPUT_VARIABLE truncated_output
  ERROR_VARIABLE truncated_error
)
if(NOT truncated_result STREQUAL "1")
  message(FATAL_ERROR
    "truncated workspace model returned ${truncated_result}: ${truncated_error}")
endif()
string(FIND "${truncated_error}" "MODEL_INVALID" truncated_position)
if(truncated_position EQUAL -1)
  message(FATAL_ERROR
    "truncated workspace model was not diagnosed at the app boundary: ${truncated_error}")
endif()
