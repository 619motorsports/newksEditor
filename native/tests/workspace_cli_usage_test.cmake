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

set(analog_config "${APEX_SOURCE_DIR}/test/content/cars/619_gen6_arca_base/data/analog_instruments.ini")
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --analog-instruments "${analog_config}" --rpm 6000
  RESULT_VARIABLE analog_result
  OUTPUT_VARIABLE analog_output
  ERROR_VARIABLE analog_error
)
if(NOT analog_result STREQUAL "1")
  message(FATAL_ERROR "analog RPM workspace returned ${analog_result}: ${analog_error}")
endif()
string(FIND "${analog_output}" "analog RPM: node=Dial_RPM, matches=1, rpm=6000" analog_position)
if(analog_position EQUAL -1)
  message(FATAL_ERROR "analog RPM did not bind before renderer setup: ${analog_output}${analog_error}")
endif()
string(FIND "${analog_error}" "caller-supplied shader modules" analog_modules_position)
if(analog_modules_position EQUAL -1)
  message(FATAL_ERROR "analog RPM workspace did not reach shader validation: ${analog_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}" --rpm 6000
  RESULT_VARIABLE detached_rpm_result
  OUTPUT_VARIABLE detached_rpm_output
  ERROR_VARIABLE detached_rpm_error
)
if(NOT detached_rpm_result STREQUAL "1")
  message(FATAL_ERROR "detached RPM returned ${detached_rpm_result}: ${detached_rpm_error}")
endif()
string(FIND "${detached_rpm_error}" "--rpm requires --analog-instruments" detached_rpm_position)
if(detached_rpm_position EQUAL -1)
  message(FATAL_ERROR "detached RPM was not diagnosed: ${detached_rpm_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --analog-instruments "${analog_config}" --rpm nan
  RESULT_VARIABLE invalid_rpm_result
  OUTPUT_VARIABLE invalid_rpm_output
  ERROR_VARIABLE invalid_rpm_error
)
if(NOT invalid_rpm_result STREQUAL "1")
  message(FATAL_ERROR "non-finite RPM returned ${invalid_rpm_result}: ${invalid_rpm_error}")
endif()
string(FIND "${invalid_rpm_error}" "RPM value must be a finite number" invalid_rpm_position)
if(invalid_rpm_position EQUAL -1)
  message(FATAL_ERROR "non-finite RPM was not diagnosed: ${invalid_rpm_error}")
endif()

set(truncated_analog "${CMAKE_CURRENT_BINARY_DIR}/apex-truncated-analog.ini")
file(WRITE "${truncated_analog}" "[RPM_INDICATOR]\nOBJECT_NAME=Dial_RPM\\")
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --analog-instruments "${truncated_analog}"
  RESULT_VARIABLE truncated_analog_result
  OUTPUT_VARIABLE truncated_analog_output
  ERROR_VARIABLE truncated_analog_error
)
if(NOT truncated_analog_result STREQUAL "1")
  message(FATAL_ERROR
    "truncated analog config returned ${truncated_analog_result}: ${truncated_analog_error}")
endif()
string(FIND "${truncated_analog_error}" "TRUNCATED_CONTINUATION" truncated_analog_position)
if(truncated_analog_position EQUAL -1)
  message(FATAL_ERROR
    "truncated analog config was not diagnosed: ${truncated_analog_error}")
endif()

set(lut_analog "${CMAKE_CURRENT_BINARY_DIR}/apex-lut-analog.ini")
file(WRITE "${lut_analog}"
  "[RPM_INDICATOR]\nOBJECT_NAME=Dial_RPM\nZERO=0\nMIN_VALUE=0\nSTEP=1\nLUT=(0=0|1=1)\n")
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --analog-instruments "${lut_analog}"
  RESULT_VARIABLE lut_analog_result
  OUTPUT_VARIABLE lut_analog_output
  ERROR_VARIABLE lut_analog_error
)
if(NOT lut_analog_result STREQUAL "1")
  message(FATAL_ERROR "LUT analog config returned ${lut_analog_result}: ${lut_analog_error}")
endif()
string(FIND "${lut_analog_error}" "analog RPM LUT preview is unsupported" lut_analog_position)
if(lut_analog_position EQUAL -1)
  message(FATAL_ERROR "LUT analog config was not rejected: ${lut_analog_error}")
endif()
file(REMOVE "${truncated_analog}" "${lut_analog}")

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
file(REMOVE "${truncated_model}")
