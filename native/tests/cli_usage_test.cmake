if(NOT DEFINED APEX_NATIVE_COMMAND)
  message(FATAL_ERROR "APEX_NATIVE_COMMAND is required")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --export-project
  RESULT_VARIABLE result
  OUTPUT_VARIABLE standard_output
  ERROR_VARIABLE standard_error
)

if(NOT result STREQUAL "2")
  message(FATAL_ERROR
    "malformed export invocation returned ${result}; stderr: ${standard_error}")
endif()

string(FIND "${standard_error}" "Usage:" usage_position)
if(usage_position EQUAL -1)
  message(FATAL_ERROR "malformed export invocation did not print usage")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --edit-ai-spline
  RESULT_VARIABLE edit_result
  OUTPUT_VARIABLE edit_standard_output
  ERROR_VARIABLE edit_standard_error
)

if(NOT edit_result STREQUAL "2")
  message(FATAL_ERROR
    "malformed AI spline edit invocation returned ${edit_result}; stderr: ${edit_standard_error}")
endif()

string(FIND "${edit_standard_error}" "Usage:" edit_usage_position)
if(edit_usage_position EQUAL -1)
  message(FATAL_ERROR "malformed AI spline edit invocation did not print usage")
endif()
