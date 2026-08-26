if(NOT DEFINED APEX_NATIVE_COMMAND OR
   NOT DEFINED APEX_AI_SPLINE_FIXTURE_COMMAND OR
   NOT DEFINED APEX_SOURCE_DIR OR NOT DEFINED APEX_BINARY_DIR)
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
set(point_truncated "${APEX_BINARY_DIR}/ai spline point truncated.ai")
set(save_input "${APEX_BINARY_DIR}/ai spline save input.ai")
set(legacy_save_input "${APEX_BINARY_DIR}/ai spline legacy save input.ai")
set(legacy_truncated_input
    "${APEX_BINARY_DIR}/ai spline legacy truncated input.ai")
set(legacy_save_output "${APEX_BINARY_DIR}/ai spline legacy save output.ai")
set(legacy_save_roundtrip
    "${APEX_BINARY_DIR}/ai spline legacy save roundtrip.ai")
set(legacy_edit_output "${APEX_BINARY_DIR}/ai spline legacy edit output.ai")
set(legacy_edit_roundtrip
    "${APEX_BINARY_DIR}/ai spline legacy edit roundtrip.ai")
set(legacy_convert_output "${APEX_BINARY_DIR}/ai spline legacy converted.ai")
set(legacy_convert_roundtrip
    "${APEX_BINARY_DIR}/ai spline legacy converted roundtrip.ai")
set(legacy_convert_invalid
    "${APEX_BINARY_DIR}/ai spline legacy conversion invalid.ai")
set(save_output "${APEX_BINARY_DIR}/ai spline save output.ai")
set(save_roundtrip "${APEX_BINARY_DIR}/ai spline save roundtrip.ai")
file(REMOVE "${output}" "${roundtrip_output}" "${invert_output}"
     "${invert_roundtrip}" "${point_output}" "${point_roundtrip}"
     "${point_invalid}" "${point_truncated}" "${save_input}"
     "${legacy_save_input}" "${legacy_truncated_input}" "${legacy_save_output}"
     "${legacy_save_roundtrip}" "${legacy_edit_output}"
     "${legacy_edit_roundtrip}"
     "${legacy_convert_output}" "${legacy_convert_roundtrip}"
     "${legacy_convert_invalid}" "${save_output}" "${save_roundtrip}")
file(WRITE "${point_truncated}" "x")
execute_process(
  COMMAND "${APEX_AI_SPLINE_FIXTURE_COMMAND}" "${save_input}"
          "${legacy_save_input}" "${legacy_truncated_input}"
  RESULT_VARIABLE save_fixture_result
)
if(NOT save_fixture_result STREQUAL "0")
  message(FATAL_ERROR "AI spline save fixtures were not created")
endif()
configure_file("${save_input}" "${save_output}" COPYONLY)

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --save-ai-spline "${save_output}"
          "${save_output}"
  RESULT_VARIABLE in_place_save_result
  OUTPUT_VARIABLE in_place_save_output
  ERROR_VARIABLE in_place_save_error
)
if(NOT in_place_save_result STREQUAL "0")
  message(FATAL_ERROR
    "in-place AI spline save failed; stderr: ${in_place_save_error}")
endif()
string(FIND "${in_place_save_output}" "grid=rebuilt" save_grid_position)
if(save_grid_position EQUAL -1)
  message(FATAL_ERROR "AI spline save did not report its rebuilt grid")
endif()
file(SHA256 "${save_output}" save_hash)

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --save-ai-spline "${point_truncated}"
          "${save_output}"
  RESULT_VARIABLE failed_save_result
  ERROR_VARIABLE failed_save_error
)
if(NOT failed_save_result STREQUAL "1")
  message(FATAL_ERROR "truncated AI spline save was not rejected")
endif()
file(SHA256 "${save_output}" failed_save_hash)
if(NOT save_hash STREQUAL failed_save_hash)
  message(FATAL_ERROR "failed AI spline save changed the existing output")
endif()
file(GLOB save_temporary_files "${save_output}.apex-tmp-*")
if(save_temporary_files)
  message(FATAL_ERROR "AI spline save left temporary output files")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --save-ai-spline "${legacy_save_input}"
          "${legacy_save_output}"
  RESULT_VARIABLE legacy_save_result
  OUTPUT_VARIABLE legacy_save_standard_output
  ERROR_VARIABLE legacy_save_error
)
if(NOT legacy_save_result STREQUAL "0" OR
   NOT EXISTS "${legacy_save_output}")
  message(FATAL_ERROR
    "legacy AI spline save did not upgrade to v7; stderr: ${legacy_save_error}")
endif()
string(FIND "${legacy_save_standard_output}" "grid=rebuilt"
       legacy_save_grid_position)
if(legacy_save_grid_position EQUAL -1)
  message(FATAL_ERROR "legacy AI spline save did not report its rebuilt grid")
endif()

file(SHA256 "${legacy_save_output}" legacy_save_hash)
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --save-ai-spline
          "${legacy_truncated_input}" "${legacy_save_output}"
  RESULT_VARIABLE legacy_failed_save_result
  ERROR_VARIABLE legacy_failed_save_error
)
if(NOT legacy_failed_save_result STREQUAL "1")
  message(FATAL_ERROR
    "truncated legacy AI spline save was not rejected; stderr: ${legacy_failed_save_error}")
endif()
file(SHA256 "${legacy_save_output}" legacy_failed_save_hash)
if(NOT legacy_save_hash STREQUAL legacy_failed_save_hash)
  message(FATAL_ERROR
    "failed legacy AI spline save changed the existing destination")
endif()
file(GLOB legacy_save_temporary_files
     "${legacy_save_output}.apex-tmp-*")
if(legacy_save_temporary_files)
  message(FATAL_ERROR "failed legacy AI spline save left temporary files")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --save-ai-spline "${legacy_save_output}"
          "${legacy_save_roundtrip}"
  RESULT_VARIABLE legacy_save_roundtrip_result
  ERROR_VARIABLE legacy_save_roundtrip_error
)
if(NOT legacy_save_roundtrip_result STREQUAL "0")
  message(FATAL_ERROR
    "legacy AI spline save output is not v7; stderr: ${legacy_save_roundtrip_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --edit-ai-spline "${legacy_save_input}"
          "${legacy_edit_output}" --index 0 --add-radius 1
  RESULT_VARIABLE legacy_edit_result
  ERROR_VARIABLE legacy_edit_error
)
if(NOT legacy_edit_result STREQUAL "0" OR
   NOT EXISTS "${legacy_edit_output}")
  message(FATAL_ERROR
    "legacy AI spline edit did not upgrade to v7; stderr: ${legacy_edit_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --save-ai-spline "${legacy_edit_output}"
          "${legacy_edit_roundtrip}"
  RESULT_VARIABLE legacy_edit_roundtrip_result
  ERROR_VARIABLE legacy_edit_roundtrip_error
)
if(NOT legacy_edit_roundtrip_result STREQUAL "0")
  message(FATAL_ERROR
    "legacy AI spline edit output is not v7; stderr: ${legacy_edit_roundtrip_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --convert-ai-spline-v2
          "${legacy_save_input}" "${legacy_convert_output}"
  RESULT_VARIABLE legacy_convert_result
  OUTPUT_VARIABLE legacy_convert_standard_output
  ERROR_VARIABLE legacy_convert_error
)
if(NOT legacy_convert_result STREQUAL "0" OR
   NOT EXISTS "${legacy_convert_output}")
  message(FATAL_ERROR
    "legacy AI spline conversion failed; stderr: ${legacy_convert_error}")
endif()
string(FIND "${legacy_convert_standard_output}" "output-version=7"
       legacy_convert_version_position)
string(FIND "${legacy_convert_standard_output}" "points=1"
       legacy_convert_count_position)
string(FIND "${legacy_convert_standard_output}" "grid=rebuilt"
       legacy_convert_grid_position)
if(legacy_convert_version_position EQUAL -1 OR
   legacy_convert_count_position EQUAL -1 OR
   legacy_convert_grid_position EQUAL -1)
  message(FATAL_ERROR
    "legacy AI spline conversion did not report its v7 point and grid result")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --save-ai-spline
          "${legacy_convert_output}" "${legacy_convert_roundtrip}"
  RESULT_VARIABLE legacy_convert_roundtrip_result
  ERROR_VARIABLE legacy_convert_roundtrip_error
)
if(NOT legacy_convert_roundtrip_result STREQUAL "0" OR
   NOT EXISTS "${legacy_convert_roundtrip}")
  message(FATAL_ERROR
    "converted legacy AI spline did not parse as v7; stderr: ${legacy_convert_roundtrip_error}")
endif()

file(SHA256 "${legacy_convert_output}" legacy_convert_hash)
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --convert-ai-spline-v2
          "${legacy_save_input}" "${legacy_convert_output}"
  RESULT_VARIABLE legacy_convert_overwrite_result
  ERROR_VARIABLE legacy_convert_overwrite_error
)
if(NOT legacy_convert_overwrite_result STREQUAL "1")
  message(FATAL_ERROR
    "legacy AI spline conversion unexpectedly overwrote its destination")
endif()
string(FIND "${legacy_convert_overwrite_error}" "output already exists"
       legacy_convert_overwrite_position)
if(legacy_convert_overwrite_position EQUAL -1)
  message(FATAL_ERROR
    "legacy conversion overwrite rejection did not report its cause")
endif()
file(SHA256 "${legacy_convert_output}" legacy_convert_after_hash)
if(NOT legacy_convert_hash STREQUAL legacy_convert_after_hash)
  message(FATAL_ERROR
    "legacy conversion overwrite rejection changed the destination")
endif()

file(SHA256 "${legacy_save_input}" legacy_source_hash)
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --convert-ai-spline-v2
          "${legacy_save_input}" "${legacy_save_input}"
  RESULT_VARIABLE legacy_convert_same_path_result
  ERROR_VARIABLE legacy_convert_same_path_error
)
if(NOT legacy_convert_same_path_result STREQUAL "1")
  message(FATAL_ERROR "legacy in-place conversion was not rejected")
endif()
string(FIND "${legacy_convert_same_path_error}" "different input and output"
       legacy_convert_same_path_position)
if(legacy_convert_same_path_position EQUAL -1)
  message(FATAL_ERROR
    "legacy in-place conversion did not report its path requirement")
endif()
file(SHA256 "${legacy_save_input}" legacy_source_after_hash)
if(NOT legacy_source_hash STREQUAL legacy_source_after_hash)
  message(FATAL_ERROR "legacy in-place conversion changed its source")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --convert-ai-spline-v2
          "${save_input}" "${legacy_convert_invalid}"
  RESULT_VARIABLE v7_convert_result
  ERROR_VARIABLE v7_convert_error
)
if(NOT v7_convert_result STREQUAL "1" OR
   EXISTS "${legacy_convert_invalid}")
  message(FATAL_ERROR "version-7 input was not rejected by legacy conversion")
endif()
string(FIND "${v7_convert_error}" "requires a version-2 source"
       v7_convert_error_position)
if(v7_convert_error_position EQUAL -1)
  message(FATAL_ERROR
    "version-7 conversion rejection did not report its version requirement")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --convert-ai-spline-v2
          "${legacy_truncated_input}" "${legacy_convert_invalid}"
  RESULT_VARIABLE truncated_convert_result
  ERROR_VARIABLE truncated_convert_error
)
if(NOT truncated_convert_result STREQUAL "1" OR
   EXISTS "${legacy_convert_invalid}")
  message(FATAL_ERROR
    "truncated legacy conversion input was not rejected; stderr: ${truncated_convert_error}")
endif()
file(GLOB legacy_convert_temporary_files
     "${legacy_convert_output}.apex-tmp-*"
     "${legacy_convert_invalid}.apex-tmp-*")
if(legacy_convert_temporary_files)
  message(FATAL_ERROR "legacy conversion left temporary output files")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --set-ai-spline-point "${save_output}"
          "${save_roundtrip}" --index 0 --position 375.941 3.949335 72.81585
  RESULT_VARIABLE save_roundtrip_result
  ERROR_VARIABLE save_roundtrip_error
)
if(NOT save_roundtrip_result STREQUAL "0" OR
   NOT EXISTS "${save_roundtrip}")
  message(FATAL_ERROR
    "saved AI spline did not parse again; stderr: ${save_roundtrip_error}")
endif()

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
  COMMAND "${APEX_NATIVE_COMMAND}" --set-ai-spline-points "${point_input}"
          "${point_output}"
          --point 0 375.941 3.949335 72.81585
          --point 1 376.5 4 74
          --point 0 375.941 3.949335 72.81585
  RESULT_VARIABLE point_result
  OUTPUT_VARIABLE point_standard_output
  ERROR_VARIABLE point_standard_error
)
if(NOT point_result STREQUAL "0" OR NOT EXISTS "${point_output}")
  message(FATAL_ERROR
    "AI spline point batch failed; stderr: ${point_standard_error}")
endif()
string(FIND "${point_standard_output}" "requested=3" requested_position)
string(FIND "${point_standard_output}" "applied=2" applied_position)
string(FIND "${point_standard_output}" "last-point=1" moved_point_position)
string(FIND "${point_standard_output}" "changed=yes" point_changed_position)
string(FIND "${point_standard_output}" "grid=rebuilt" grid_rebuilt_position)
if(requested_position EQUAL -1 OR applied_position EQUAL -1 OR
   moved_point_position EQUAL -1 OR point_changed_position EQUAL -1 OR
   grid_rebuilt_position EQUAL -1)
  message(FATAL_ERROR
    "AI spline point batch did not report its unique points, change, and grid")
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
  COMMAND "${APEX_NATIVE_COMMAND}" --set-ai-spline-points "${point_input}"
          "${point_invalid}"
          --point 0 375.941 3.949335 72.81585
          --point 0 376.941 3.949335 72.81585
  RESULT_VARIABLE point_conflict_result
  ERROR_VARIABLE point_conflict_error
)
if(NOT point_conflict_result STREQUAL "1" OR EXISTS "${point_invalid}")
  message(FATAL_ERROR
    "conflicting duplicate AI spline point batch was not rejected")
endif()
string(FIND "${point_conflict_error}" "POINT_EDIT_CONFLICT"
       point_conflict_position)
if(point_conflict_position EQUAL -1)
  message(FATAL_ERROR
    "conflicting duplicate AI spline point rejection did not report its cause")
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

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --set-ai-spline-points
          "${point_truncated}" "${point_invalid}"
          --point 0 1 2 3
  RESULT_VARIABLE point_truncated_result
  ERROR_VARIABLE point_truncated_error
)
if(NOT point_truncated_result STREQUAL "1" OR EXISTS "${point_invalid}")
  message(FATAL_ERROR
    "truncated AI spline point batch input was not rejected; stderr: ${point_truncated_error}")
endif()

file(REMOVE "${output}" "${roundtrip_output}" "${invert_output}"
     "${invert_roundtrip}" "${point_output}" "${point_roundtrip}"
     "${point_invalid}" "${point_truncated}" "${save_input}"
     "${legacy_save_input}" "${legacy_truncated_input}" "${legacy_save_output}"
     "${legacy_convert_output}" "${legacy_convert_roundtrip}"
     "${legacy_convert_invalid}" "${save_output}" "${save_roundtrip}")
