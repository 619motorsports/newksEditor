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
