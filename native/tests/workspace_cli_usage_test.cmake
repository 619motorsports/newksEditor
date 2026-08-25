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
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --sun-height 55
  RESULT_VARIABLE detached_lighting_result
  ERROR_VARIABLE detached_lighting_error
)
if(NOT detached_lighting_result STREQUAL "1")
  message(FATAL_ERROR
    "detached lighting returned ${detached_lighting_result}: ${detached_lighting_error}")
endif()
string(FIND "${detached_lighting_error}"
  "lighting options require a workspace model" detached_lighting_position)
if(detached_lighting_position EQUAL -1)
  message(FATAL_ERROR
    "detached lighting was not diagnosed: ${detached_lighting_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --sun-heading nan
  RESULT_VARIABLE invalid_sun_result
  ERROR_VARIABLE invalid_sun_error
)
if(NOT invalid_sun_result STREQUAL "1")
  message(FATAL_ERROR
    "non-finite sun returned ${invalid_sun_result}: ${invalid_sun_error}")
endif()
string(FIND "${invalid_sun_error}"
  "sun heading must be a finite number" invalid_sun_position)
if(invalid_sun_position EQUAL -1)
  message(FATAL_ERROR "non-finite sun was not diagnosed: ${invalid_sun_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --weather 3_clear --weather 5_light_clouds
  RESULT_VARIABLE duplicate_weather_result
  ERROR_VARIABLE duplicate_weather_error
)
if(NOT duplicate_weather_result STREQUAL "1")
  message(FATAL_ERROR
    "duplicate weather returned ${duplicate_weather_result}: ${duplicate_weather_error}")
endif()
string(FIND "${duplicate_weather_error}"
  "duplicate --weather option" duplicate_weather_position)
if(duplicate_weather_position EQUAL -1)
  message(FATAL_ERROR
    "duplicate weather was not diagnosed: ${duplicate_weather_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --directional-shadow-vertex shadow.spv
  RESULT_VARIABLE detached_shadow_result
  OUTPUT_VARIABLE detached_shadow_output
  ERROR_VARIABLE detached_shadow_error
)
if(NOT detached_shadow_result STREQUAL "1")
  message(FATAL_ERROR
    "detached shadow shader returned ${detached_shadow_result}: ${detached_shadow_error}")
endif()
string(FIND "${detached_shadow_error}"
  "shader modules require a workspace model" detached_shadow_position)
if(detached_shadow_position EQUAL -1)
  message(FATAL_ERROR
    "detached shadow shader was not diagnosed: ${detached_shadow_error}")
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
          --directional-shadow-vertex shadow.spv
  RESULT_VARIABLE opaque_shadow_without_receivers_result
  ERROR_VARIABLE opaque_shadow_without_receivers_error
)
if(NOT opaque_shadow_without_receivers_result STREQUAL "1")
  message(FATAL_ERROR
    "opaque shadow without receivers returned ${opaque_shadow_without_receivers_result}: ${opaque_shadow_without_receivers_error}")
endif()
string(FIND "${opaque_shadow_without_receivers_error}"
  "--directional-shadow-vertex requires receiver-capable material shader modules"
  opaque_shadow_without_receivers_position)
if(opaque_shadow_without_receivers_position EQUAL -1)
  message(FATAL_ERROR
    "legacy opaque shadow diagnostic changed: ${opaque_shadow_without_receivers_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --shader-family fixture --shader-vertex fixture.vert.spv
          --shader-fragment fixture.frag.spv
          --directional-shadow-alpha-vertex alpha.vert.spv
  RESULT_VARIABLE incomplete_alpha_shadow_result
  ERROR_VARIABLE incomplete_alpha_shadow_error
)
if(NOT incomplete_alpha_shadow_result STREQUAL "1")
  message(FATAL_ERROR
    "incomplete alpha shadow returned ${incomplete_alpha_shadow_result}: ${incomplete_alpha_shadow_error}")
endif()
string(FIND "${incomplete_alpha_shadow_error}"
  "directional-shadow alpha vertex and fragment modules must be supplied together"
  incomplete_alpha_shadow_position)
if(incomplete_alpha_shadow_position EQUAL -1)
  message(FATAL_ERROR
    "incomplete alpha shadow pair was not diagnosed: ${incomplete_alpha_shadow_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --directional-shadow-alpha-vertex alpha.vert.spv
          --directional-shadow-alpha-fragment alpha.frag.spv
  RESULT_VARIABLE alpha_shadow_without_receivers_result
  ERROR_VARIABLE alpha_shadow_without_receivers_error
)
if(NOT alpha_shadow_without_receivers_result STREQUAL "1")
  message(FATAL_ERROR
    "alpha shadow without receivers returned ${alpha_shadow_without_receivers_result}: ${alpha_shadow_without_receivers_error}")
endif()
string(FIND "${alpha_shadow_without_receivers_error}"
  "directional-shadow modules require receiver-capable material shader modules"
  alpha_shadow_without_receivers_position)
if(alpha_shadow_without_receivers_position EQUAL -1)
  message(FATAL_ERROR
    "alpha shadow receiver requirement was not diagnosed: ${alpha_shadow_without_receivers_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --directional-shadow-skinned-vertex skinned.vert.spv
  RESULT_VARIABLE skinned_shadow_without_receivers_result
  ERROR_VARIABLE skinned_shadow_without_receivers_error
)
if(NOT skinned_shadow_without_receivers_result STREQUAL "1")
  message(FATAL_ERROR
    "skinned shadow without receivers returned ${skinned_shadow_without_receivers_result}: ${skinned_shadow_without_receivers_error}")
endif()
string(FIND "${skinned_shadow_without_receivers_error}"
  "directional-shadow modules require receiver-capable material shader modules"
  skinned_shadow_without_receivers_position)
if(skinned_shadow_without_receivers_position EQUAL -1)
  message(FATAL_ERROR
    "skinned shadow receiver requirement was not diagnosed: ${skinned_shadow_without_receivers_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}" --grid
  RESULT_VARIABLE grid_without_modules_result
  ERROR_VARIABLE grid_without_modules_error
)
if(NOT grid_without_modules_result STREQUAL "1")
  message(FATAL_ERROR
    "grid without modules returned ${grid_without_modules_result}: ${grid_without_modules_error}")
endif()
string(FIND "${grid_without_modules_error}"
  "--grid requires authoring-overlay shader modules" grid_without_modules_position)
if(grid_without_modules_position EQUAL -1)
  message(FATAL_ERROR
    "grid without modules was not diagnosed: ${grid_without_modules_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --authoring-overlay-vertex overlay.vert.spv
          --authoring-overlay-fragment overlay.frag.spv
  RESULT_VARIABLE detached_overlay_result
  ERROR_VARIABLE detached_overlay_error
)
if(NOT detached_overlay_result STREQUAL "1")
  message(FATAL_ERROR
    "detached overlay modules returned ${detached_overlay_result}: ${detached_overlay_error}")
endif()
string(FIND "${detached_overlay_error}"
  "authoring-overlay shader modules require --selected-node, --grid, or --view-axis"
  detached_overlay_position)
if(detached_overlay_position EQUAL -1)
  message(FATAL_ERROR
    "detached overlay modules were not diagnosed: ${detached_overlay_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}" --view-axis
  RESULT_VARIABLE view_axis_without_modules_result
  ERROR_VARIABLE view_axis_without_modules_error
)
if(NOT view_axis_without_modules_result STREQUAL "1")
  message(FATAL_ERROR
    "view axis without modules returned ${view_axis_without_modules_result}: ${view_axis_without_modules_error}")
endif()
string(FIND "${view_axis_without_modules_error}"
  "--view-axis requires authoring-overlay shader modules"
  view_axis_without_modules_position)
if(view_axis_without_modules_position EQUAL -1)
  message(FATAL_ERROR
    "view axis without modules was not diagnosed: ${view_axis_without_modules_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --grid --selection-axis-vertex overlay.vert.spv
  RESULT_VARIABLE incomplete_overlay_result
  ERROR_VARIABLE incomplete_overlay_error
)
if(NOT incomplete_overlay_result STREQUAL "1")
  message(FATAL_ERROR
    "incomplete overlay modules returned ${incomplete_overlay_result}: ${incomplete_overlay_error}")
endif()
string(FIND "${incomplete_overlay_error}"
  "authoring-overlay vertex and fragment modules must be supplied together"
  incomplete_overlay_position)
if(incomplete_overlay_position EQUAL -1)
  message(FATAL_ERROR
    "incomplete overlay modules were not diagnosed: ${incomplete_overlay_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --weather missing
  RESULT_VARIABLE unknown_weather_result
  ERROR_VARIABLE unknown_weather_error
)
if(NOT unknown_weather_result STREQUAL "1")
  message(FATAL_ERROR
    "unknown weather returned ${unknown_weather_result}: ${unknown_weather_error}")
endif()
string(FIND "${unknown_weather_error}"
  "workspace_viewport_weather_unknown" unknown_weather_position)
if(unknown_weather_position EQUAL -1)
  message(FATAL_ERROR
    "unknown weather was not diagnosed: ${unknown_weather_error}")
endif()

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

set(animation "${APEX_SOURCE_DIR}/test/content/cars/619_gen6_arca_base/animations/gascap.ksanim")
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --animation "${animation}" --animation-position 0.5
  RESULT_VARIABLE animation_result
  OUTPUT_VARIABLE animation_output
  ERROR_VARIABLE animation_error
)
if(NOT animation_result STREQUAL "1")
  message(FATAL_ERROR "animation workspace returned ${animation_result}: ${animation_error}")
endif()
string(FIND "${animation_output}"
  "animation: tracks=5, animated=2, matched-tracks=0, matched-nodes=0, position=0.5"
  animation_position)
if(animation_position EQUAL -1)
  message(FATAL_ERROR
    "animation did not reach native model binding: ${animation_output}${animation_error}")
endif()
string(FIND "${animation_error}" "caller-supplied shader modules" animation_modules_position)
if(animation_modules_position EQUAL -1)
  message(FATAL_ERROR
    "animation workspace did not reach shader validation: ${animation_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --animation "${animation}" --animation-position 2
  RESULT_VARIABLE clamped_animation_result
  OUTPUT_VARIABLE clamped_animation_output
  ERROR_VARIABLE clamped_animation_error
)
if(NOT clamped_animation_result STREQUAL "1")
  message(FATAL_ERROR
    "clamped animation returned ${clamped_animation_result}: ${clamped_animation_error}")
endif()
string(FIND "${clamped_animation_output}" "position=1" clamped_animation_position)
if(clamped_animation_position EQUAL -1)
  message(FATAL_ERROR
    "animation position did not use the recovered endpoint clamp: ${clamped_animation_output}${clamped_animation_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --animation-position 0.5
  RESULT_VARIABLE detached_animation_result
  OUTPUT_VARIABLE detached_animation_output
  ERROR_VARIABLE detached_animation_error
)
if(NOT detached_animation_result STREQUAL "1")
  message(FATAL_ERROR
    "detached animation position returned ${detached_animation_result}: ${detached_animation_error}")
endif()
string(FIND "${detached_animation_error}"
  "--animation-position requires --animation" detached_animation_position)
if(detached_animation_position EQUAL -1)
  message(FATAL_ERROR
    "detached animation position was not diagnosed: ${detached_animation_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --animation "${animation}" --animation-position inf
  RESULT_VARIABLE invalid_animation_position_result
  OUTPUT_VARIABLE invalid_animation_position_output
  ERROR_VARIABLE invalid_animation_position_error
)
if(NOT invalid_animation_position_result STREQUAL "1")
  message(FATAL_ERROR
    "non-finite animation position returned ${invalid_animation_position_result}: ${invalid_animation_position_error}")
endif()
string(FIND "${invalid_animation_position_error}"
  "animation position must be a finite number" invalid_animation_position)
if(invalid_animation_position EQUAL -1)
  message(FATAL_ERROR
    "non-finite animation position was not diagnosed: ${invalid_animation_position_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --node-search "GEO_Fabric1" --selected-node 0 --show-hidden --wireframe
  RESULT_VARIABLE hierarchy_result
  OUTPUT_VARIABLE hierarchy_output
  ERROR_VARIABLE hierarchy_error
)
if(NOT hierarchy_result STREQUAL "1")
  message(FATAL_ERROR "hierarchy workspace returned ${hierarchy_result}: ${hierarchy_error}")
endif()
string(FIND "${hierarchy_output}" "hierarchy: matches=" hierarchy_position)
if(hierarchy_position EQUAL -1)
  message(FATAL_ERROR
    "hierarchy search did not reach native selection: ${hierarchy_output}${hierarchy_error}")
endif()
string(FIND "${hierarchy_error}" "caller-supplied shader modules" hierarchy_modules_position)
if(hierarchy_modules_position EQUAL -1)
  message(FATAL_ERROR
    "hierarchy workspace did not reach shader validation: ${hierarchy_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --selected-node 999999
  RESULT_VARIABLE missing_node_result
  OUTPUT_VARIABLE missing_node_output
  ERROR_VARIABLE missing_node_error
)
if(NOT missing_node_result STREQUAL "1")
  message(FATAL_ERROR
    "missing selected node returned ${missing_node_result}: ${missing_node_error}")
endif()
string(FIND "${missing_node_error}"
  "workspace_selection_node_invalid" missing_node_position)
if(missing_node_position EQUAL -1)
  message(FATAL_ERROR
    "missing selected node was not diagnosed: ${missing_node_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --selected-node 0 --isolate-selected
  RESULT_VARIABLE group_isolation_result
  OUTPUT_VARIABLE group_isolation_output
  ERROR_VARIABLE group_isolation_error
)
if(NOT group_isolation_result STREQUAL "1")
  message(FATAL_ERROR
    "group isolation returned ${group_isolation_result}: ${group_isolation_error}")
endif()
string(FIND "${group_isolation_error}"
  "workspace_selection_isolation_invalid" group_isolation_position)
if(group_isolation_position EQUAL -1)
  message(FATAL_ERROR
    "group isolation was not diagnosed: ${group_isolation_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --isolate-selected
  RESULT_VARIABLE detached_isolation_result
  OUTPUT_VARIABLE detached_isolation_output
  ERROR_VARIABLE detached_isolation_error
)
if(NOT detached_isolation_result STREQUAL "1")
  message(FATAL_ERROR
    "detached isolation returned ${detached_isolation_result}: ${detached_isolation_error}")
endif()
string(FIND "${detached_isolation_error}"
  "--isolate-selected requires --selected-node" detached_isolation_position)
if(detached_isolation_position EQUAL -1)
  message(FATAL_ERROR
    "detached isolation was not diagnosed: ${detached_isolation_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --selected-node -1
  RESULT_VARIABLE invalid_node_id_result
  OUTPUT_VARIABLE invalid_node_id_output
  ERROR_VARIABLE invalid_node_id_error
)
if(NOT invalid_node_id_result STREQUAL "1")
  message(FATAL_ERROR
    "invalid node ID returned ${invalid_node_id_result}: ${invalid_node_id_error}")
endif()
string(FIND "${invalid_node_id_error}"
  "selected node ID must be a valid unsigned integer" invalid_node_id_position)
if(invalid_node_id_position EQUAL -1)
  message(FATAL_ERROR
    "invalid node ID was not diagnosed: ${invalid_node_id_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --model "${model}"
          --lod-index 0
  RESULT_VARIABLE detached_lod_result
  OUTPUT_VARIABLE detached_lod_output
  ERROR_VARIABLE detached_lod_error
)
if(NOT detached_lod_result STREQUAL "1")
  message(FATAL_ERROR
    "detached LOD index returned ${detached_lod_result}: ${detached_lod_error}")
endif()
string(FIND "${detached_lod_error}"
  "--lod-index requires a carLods workspace" detached_lod_position)
if(detached_lod_position EQUAL -1)
  message(FATAL_ERROR
    "detached LOD index was not diagnosed: ${detached_lod_error}")
endif()

execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan --workspace-root x
          --manifest lods.ini --kind carLods --lod-index -1
  RESULT_VARIABLE invalid_lod_result
  OUTPUT_VARIABLE invalid_lod_output
  ERROR_VARIABLE invalid_lod_error
)
if(NOT invalid_lod_result STREQUAL "1")
  message(FATAL_ERROR
    "invalid LOD index returned ${invalid_lod_result}: ${invalid_lod_error}")
endif()
string(FIND "${invalid_lod_error}"
  "LOD index must be a valid unsigned 32-bit integer" invalid_lod_position)
if(invalid_lod_position EQUAL -1)
  message(FATAL_ERROR
    "invalid LOD index was not diagnosed: ${invalid_lod_error}")
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
          --manifest data/lods.ini --kind carLods --lod-index 0
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
execute_process(
  COMMAND "${APEX_NATIVE_COMMAND}" --window vulkan
          --workspace-root "${workspace_root}"
          --manifest data/lods.ini --kind carLods --lod-index 1
  RESULT_VARIABLE missing_lod_result
  OUTPUT_VARIABLE missing_lod_output
  ERROR_VARIABLE missing_lod_error
)
if(NOT missing_lod_result STREQUAL "1")
  message(FATAL_ERROR
    "missing LOD index returned ${missing_lod_result}: ${missing_lod_error}")
endif()
string(FIND "${missing_lod_error}"
  "selected workspace LOD index is not present" missing_lod_position)
if(missing_lod_position EQUAL -1)
  message(FATAL_ERROR
    "missing LOD index was not diagnosed: ${missing_lod_error}")
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
