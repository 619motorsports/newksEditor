if(NOT DEFINED APEX_NATIVE_COMMAND OR NOT DEFINED APEX_SOURCE_DIR OR
   NOT DEFINED APEX_BINARY_DIR)
  message(FATAL_ERROR "AI spline CLI test paths are required")
endif()

set(input "${APEX_SOURCE_DIR}/test/content/tracks/sepang/ai/fast_lane.ai")
set(output "${APEX_BINARY_DIR}/ai-spline-edit-cli-output.ai")
set(roundtrip_output "${APEX_BINARY_DIR}/ai-spline-edit-cli-roundtrip.ai")
file(REMOVE "${output}" "${roundtrip_output}")

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --edit-ai-spline "${input}" "${output}"
          --index 0 --add-radius 1.25 --set-camber-degrees 2.5
  RESULT_VARIABLE result
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR
    "AI spline edit failed with ${result}; stderr: ${standard_error}")
endif()
if(NOT EXISTS "${output}")
  message(FATAL_ERROR "AI spline edit did not create its output")
endif()
file(SIZE "${output}" output_size)
if(output_size LESS 1)
  message(FATAL_ERROR "AI spline edit created an empty output")
endif()
string(FIND "${standard_output}" "point=0" point_position)
string(FIND "${standard_output}" "changed=yes" changed_position)
if(point_position EQUAL -1 OR changed_position EQUAL -1)
  message(FATAL_ERROR "AI spline edit did not report the edited point and change")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --edit-ai-spline "${output}" "${roundtrip_output}"
          --index 0 --add-grade -0.125
  RESULT_VARIABLE roundtrip_result
  OUTPUT_VARIABLE roundtrip_standard_output
  ERROR_VARIABLE roundtrip_standard_error
)
if(NOT roundtrip_result STREQUAL "0" OR NOT EXISTS "${roundtrip_output}")
  message(FATAL_ERROR
    "edited AI spline did not parse and edit again; stderr: ${roundtrip_standard_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --edit-ai-spline "${input}" "${output}"
          --index 0 --add-radius 1
  RESULT_VARIABLE overwrite_result
  OUTPUT_VARIABLE overwrite_standard_output
  ERROR_VARIABLE overwrite_standard_error
)
if(NOT overwrite_result STREQUAL "1")
  message(FATAL_ERROR "existing AI spline output was unexpectedly overwritten")
endif()
string(FIND "${overwrite_standard_error}" "output already exists" overwrite_position)
if(overwrite_position EQUAL -1)
  message(FATAL_ERROR "existing-output rejection did not report its cause")
endif()

file(REMOVE "${output}" "${roundtrip_output}")
