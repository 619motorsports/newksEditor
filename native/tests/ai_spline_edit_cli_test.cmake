if(NOT DEFINED APEX_NATIVE_COMMAND OR NOT DEFINED APEX_SOURCE_DIR OR
   NOT DEFINED APEX_BINARY_DIR)
  message(FATAL_ERROR "AI spline CLI test paths are required")
endif()

set(input "${APEX_SOURCE_DIR}/test/content/tracks/sepang/ai/fast_lane.ai")
set(output "${APEX_BINARY_DIR}/ai-spline-edit-cli-output.ai")
set(roundtrip_output "${APEX_BINARY_DIR}/ai-spline-edit-cli-roundtrip.ai")
set(invert_output "${APEX_BINARY_DIR}/ai-spline-invert-cli-output.ai")
set(invert_roundtrip "${APEX_BINARY_DIR}/ai-spline-invert-cli-roundtrip.ai")
set(point_input
    "${APEX_SOURCE_DIR}/test/content/tracks/sepang/ai/pit_lane.ai")
set(point_output "${APEX_BINARY_DIR}/ai spline point output.ai")
set(point_roundtrip "${APEX_BINARY_DIR}/ai spline point roundtrip.ai")
set(point_invalid "${APEX_BINARY_DIR}/ai spline point invalid.ai")
file(REMOVE "${output}" "${roundtrip_output}" "${invert_output}"
     "${invert_roundtrip}" "${point_output}" "${point_roundtrip}"
     "${point_invalid}")

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

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --invert-ai-spline "${input}"
          "${invert_output}" --index 0 --index 1 --index 0
  RESULT_VARIABLE invert_result
  OUTPUT_VARIABLE invert_standard_output
  ERROR_VARIABLE invert_standard_error
)
if(NOT invert_result STREQUAL "0" OR NOT EXISTS "${invert_output}")
  message(FATAL_ERROR
    "AI spline camber inversion failed; stderr: ${invert_standard_error}")
endif()
string(FIND "${invert_standard_output}" "selected=2" selected_position)
string(FIND "${invert_standard_output}" "changed=yes" invert_changed_position)
if(selected_position EQUAL -1 OR invert_changed_position EQUAL -1)
  message(FATAL_ERROR
    "AI spline inversion did not preserve UI-compatible unique selection")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --invert-ai-spline "${input}"
          "${invert_output}" --index 0
  RESULT_VARIABLE invert_overwrite_result
  ERROR_VARIABLE invert_overwrite_error
)
if(NOT invert_overwrite_result STREQUAL "1")
  message(FATAL_ERROR
    "existing AI spline inversion output was unexpectedly overwritten")
endif()
string(FIND "${invert_overwrite_error}" "output already exists"
       invert_overwrite_position)
if(invert_overwrite_position EQUAL -1)
  message(FATAL_ERROR
    "AI spline inversion overwrite rejection did not report its cause")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --invert-ai-spline "${invert_output}"
          "${invert_roundtrip}" --index 0 --index 1
  RESULT_VARIABLE invert_roundtrip_result
  ERROR_VARIABLE invert_roundtrip_error
)
if(NOT invert_roundtrip_result STREQUAL "0" OR
   NOT EXISTS "${invert_roundtrip}")
  message(FATAL_ERROR
    "second AI spline camber inversion failed; stderr: ${invert_roundtrip_error}")
endif()
file(SHA256 "${input}" input_hash)
file(SHA256 "${invert_roundtrip}" invert_roundtrip_hash)
# This installed fixture already has the native zero reserved words. Thus,
# byte equality proves that two inversions restore its canonical saved form.
if(NOT input_hash STREQUAL invert_roundtrip_hash)
  message(FATAL_ERROR
    "two AI spline camber inversions did not restore the source bytes")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --set-ai-spline-point "${point_input}"
          "${point_output}" --index 0 --position 375.941 3.949335 72.81585
  RESULT_VARIABLE point_result
  OUTPUT_VARIABLE point_standard_output
  ERROR_VARIABLE point_standard_error
)
if(NOT point_result STREQUAL "0" OR NOT EXISTS "${point_output}")
  message(FATAL_ERROR
    "AI spline point movement failed; stderr: ${point_standard_error}")
endif()
string(FIND "${point_standard_output}" "point=0" moved_point_position)
string(FIND "${point_standard_output}" "changed=yes" point_changed_position)
string(FIND "${point_standard_output}" "grid=rebuilt" grid_rebuilt_position)
if(moved_point_position EQUAL -1 OR point_changed_position EQUAL -1 OR
   grid_rebuilt_position EQUAL -1)
  message(FATAL_ERROR
    "AI spline point movement did not report its point, change, and grid")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --set-ai-spline-point "${point_output}"
          "${point_roundtrip}" --index 0 --position 375.941 3.949335 72.81585
  RESULT_VARIABLE point_roundtrip_result
  OUTPUT_VARIABLE point_roundtrip_standard_output
  ERROR_VARIABLE point_roundtrip_standard_error
)
if(NOT point_roundtrip_result STREQUAL "0" OR
   NOT EXISTS "${point_roundtrip}")
  message(FATAL_ERROR
    "moved AI spline did not parse again; stderr: ${point_roundtrip_standard_error}")
endif()
string(FIND "${point_roundtrip_standard_output}" "changed=no"
       point_no_change_position)
string(FIND "${point_roundtrip_standard_output}" "grid=preserved"
       grid_preserved_position)
if(point_no_change_position EQUAL -1 OR grid_preserved_position EQUAL -1)
  message(FATAL_ERROR
    "identical AI spline point movement did not report a no-op")
endif()
file(SHA256 "${point_output}" point_output_hash)
file(SHA256 "${point_roundtrip}" point_roundtrip_hash)
if(NOT point_output_hash STREQUAL point_roundtrip_hash)
  message(FATAL_ERROR
    "identical AI spline point movement changed canonical bytes")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --set-ai-spline-point "${point_output}"
          "${point_output}" --index 0 --position 375.941 3.949335 72.81585
  RESULT_VARIABLE point_overwrite_result
  ERROR_VARIABLE point_overwrite_error
)
if(NOT point_overwrite_result STREQUAL "1")
  message(FATAL_ERROR
    "existing AI spline point output was unexpectedly overwritten")
endif()
string(FIND "${point_overwrite_error}" "output already exists"
       point_overwrite_position)
if(point_overwrite_position EQUAL -1)
  message(FATAL_ERROR
    "point-position overwrite rejection did not report its cause")
endif()
file(SHA256 "${point_output}" point_overwrite_hash)
if(NOT point_output_hash STREQUAL point_overwrite_hash)
  message(FATAL_ERROR "overwrite rejection changed the existing AI spline")
endif()
file(GLOB point_temporary_files "${point_output}.apex-tmp-*")
if(point_temporary_files)
  message(FATAL_ERROR "overwrite rejection left temporary AI spline files")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --set-ai-spline-point "${point_input}"
          "${point_invalid}" --index 0 --position nan 0 0
  RESULT_VARIABLE point_invalid_result
  ERROR_VARIABLE point_invalid_error
)
if(NOT point_invalid_result STREQUAL "1" OR EXISTS "${point_invalid}")
  message(FATAL_ERROR "non-finite AI spline point movement was not rejected")
endif()
string(FIND "${point_invalid_error}" "finite" point_invalid_position)
if(point_invalid_position EQUAL -1)
  message(FATAL_ERROR
    "non-finite AI spline point rejection did not report its cause")
endif()

file(REMOVE "${output}" "${roundtrip_output}" "${invert_output}"
     "${invert_roundtrip}" "${point_output}" "${point_roundtrip}"
     "${point_invalid}")
