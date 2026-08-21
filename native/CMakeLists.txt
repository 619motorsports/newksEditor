find_package(Vulkan QUIET)

add_library(apex_native INTERFACE)
add_library(ApexEditor::Native ALIAS apex_native)

target_compile_features(apex_native INTERFACE cxx_std_20)
target_include_directories(apex_native INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

if(MSVC)
  target_compile_options(apex_native INTERFACE /W4 /permissive- /EHsc)
  if(APEX_WARNINGS_AS_ERRORS)
    target_compile_options(apex_native INTERFACE /WX)
  endif()
else()
  target_compile_options(apex_native INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Werror=return-type
  )
  if(APEX_WARNINGS_AS_ERRORS)
    target_compile_options(apex_native INTERFACE -Werror)
  endif()
  if(APEX_ENABLE_SANITIZERS)
    target_compile_options(apex_native INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(apex_native INTERFACE -fsanitize=address,undefined)
  endif()
endif()

if(APEX_ENABLE_VULKAN AND Vulkan_FOUND)
  target_compile_definitions(apex_native INTERFACE APEX_HAS_VULKAN=1)
  target_link_libraries(apex_native INTERFACE Vulkan::Vulkan)
  set(APEX_VULKAN_AVAILABLE TRUE)
else()
  set(APEX_VULKAN_AVAILABLE FALSE)
  if(APEX_ENABLE_VULKAN)
    message(STATUS "Vulkan SDK not found; Vulkan support is disabled")
  endif()
endif()

if(APEX_ENABLE_D3D12)
  if(NOT WIN32)
    message(WARNING "APEX_ENABLE_D3D12 requested on a non-Windows host; disabling it")
    set(APEX_ENABLE_D3D12 OFF CACHE BOOL "Build D3D12 support on Windows" FORCE)
  else()
    target_compile_definitions(apex_native INTERFACE APEX_HAS_D3D12=1)
    target_link_libraries(apex_native INTERFACE d3d12 dxgi)
  endif()
endif()

add_library(apex_core STATIC
  src/core/byte_reader.cpp
  src/core/parse_error.cpp
)
add_library(ApexEditor::Core ALIAS apex_core)
target_link_libraries(apex_core PUBLIC ApexEditor::Native)

add_library(apex_formats STATIC
  src/formats/acd.cpp
  src/formats/bc7.cpp
  src/formats/dds.cpp
  src/formats/fbx.cpp
  src/formats/fbx_conversion.cpp
  src/formats/ini.cpp
  src/formats/kn5.cpp
  src/formats/kn5_write.cpp
  src/formats/knh.cpp
  src/formats/ksanim.cpp
  src/formats/vao.cpp
)
add_library(ApexEditor::Formats ALIAS apex_formats)
target_link_libraries(apex_formats PUBLIC ApexEditor::Core)

add_library(apex_authoring STATIC
  src/authoring/geometry.cpp
  src/authoring/project.cpp
  src/authoring/project_io.cpp
  src/formats/kn5_bake.cpp
)
add_library(ApexEditor::Authoring ALIAS apex_authoring)
target_link_libraries(apex_authoring PUBLIC ApexEditor::Formats)

add_library(apex_assets STATIC
  src/assets/asset_index.cpp
  src/assets/asset_source.cpp
)
add_library(ApexEditor::Assets ALIAS apex_assets)
target_link_libraries(apex_assets PUBLIC ApexEditor::Formats)

add_library(apex_csp STATIC
  src/csp/config_model.cpp
  src/csp/material_evaluator.cpp
)
add_library(ApexEditor::Csp ALIAS apex_csp)
target_link_libraries(apex_csp PUBLIC ApexEditor::Formats)

add_library(apex_domain STATIC
  src/domain/car_damage.cpp
  src/domain/car_validation.cpp
  src/domain/driver.cpp
  src/domain/skin_metadata.cpp
  src/domain/track_data.cpp
)
add_library(ApexEditor::Domain ALIAS apex_domain)
target_link_libraries(apex_domain PUBLIC ApexEditor::Formats)

add_library(apex_scene STATIC
  src/scene/kn5_scene.cpp
)
add_library(ApexEditor::Scene ALIAS apex_scene)
target_link_libraries(apex_scene PUBLIC ApexEditor::Formats)

add_library(apex_workspace STATIC
  src/workspace/workspace.cpp
)
add_library(ApexEditor::Workspace ALIAS apex_workspace)
target_link_libraries(apex_workspace PUBLIC ApexEditor::Formats)

add_library(apex_platform STATIC
  src/platform/security.cpp
)
add_library(ApexEditor::Platform ALIAS apex_platform)
target_link_libraries(apex_platform PUBLIC ApexEditor::Native)

add_library(apex_render STATIC
  src/render/camera.cpp
  src/render/decoded_dds_texture.cpp
  src/render/device.cpp
  src/render/draw_packet.cpp
  src/render/frame_plan.cpp
  src/render/kn5_scene_node_map.cpp
  src/render/lighting.cpp
  src/render/material_binding.cpp
  src/render/material_profile.cpp
  src/render/pipeline.cpp
  src/render/render_plan.cpp
  src/render/static_mesh_upload.cpp
  src/render/skinned_mesh_upload.cpp
  src/render/static_scene.cpp
  src/render/texture_upload.cpp
  src/render/vulkan_device.cpp
  src/render/d3d12_device.cpp
)
add_library(ApexEditor::Render ALIAS apex_render)
target_link_libraries(apex_render PUBLIC ApexEditor::Formats)

add_executable(apex_native_app src/app/main.cpp)
set_target_properties(apex_native_app PROPERTIES OUTPUT_NAME apex-native)
target_link_libraries(apex_native_app PRIVATE ApexEditor::Formats ApexEditor::Render)

if(NOT BUILD_TESTING OR NOT APEX_BUILD_TESTS)
  return()
endif()

# Tests are intentionally discovered by filename. This keeps the native build
# usable while the port is staged and gives each format/backend a separate
# executable once its implementation lands in native/tests.
function(apex_add_native_test source stem library)
  set(test_source "${CMAKE_CURRENT_SOURCE_DIR}/tests/${source}")
  if(NOT EXISTS "${test_source}")
    set(test_source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    if(NOT EXISTS "${test_source}")
      message(STATUS "Skipping ${stem}: ${source} is not present")
      return()
    endif()
  endif()

  add_executable(${stem} "${test_source}")
  target_link_libraries(${stem} PRIVATE ${library})
  add_test(NAME ${stem} COMMAND ${stem})

  # Native tests return 77 when a backend cannot be exercised on the current
  # machine (for example, no Vulkan physical device or no D3D12 adapter).
  # CTest treats that result as skipped instead of turning hardware absence
  # into a CI failure. The regex covers older staged tests with diagnostics.
  set_tests_properties(${stem} PROPERTIES
    SKIP_RETURN_CODE 77
    SKIP_REGULAR_EXPRESSION "[Uu]navailable|[Nn]ot available|[Nn]o physical device|[Nn]o D3D12 adapter"
  )
endfunction()

apex_add_native_test(ksanim_tests.cpp ksanim_tests ApexEditor::Formats)
apex_add_native_test(knh_tests.cpp knh_tests ApexEditor::Formats)
apex_add_native_test(kn5_tests.cpp kn5_tests ApexEditor::Formats)
apex_add_native_test(kn5_write_tests.cpp kn5_write_tests ApexEditor::Formats)
apex_add_native_test(dds_tests.cpp dds_tests ApexEditor::Formats)
apex_add_native_test(bc7_tests.cpp bc7_tests ApexEditor::Formats)
apex_add_native_test(fbx_tests.cpp fbx_tests ApexEditor::Formats)
apex_add_native_test(fbx_conversion_tests.cpp fbx_conversion_tests ApexEditor::Formats)
apex_add_native_test(acd_tests.cpp acd_tests ApexEditor::Formats)
apex_add_native_test(ini_tests.cpp ini_tests ApexEditor::Formats)
apex_add_native_test(vao_tests.cpp vao_tests ApexEditor::Formats)
apex_add_native_test(authoring_project_tests.cpp authoring_project_tests ApexEditor::Authoring)
apex_add_native_test(geometry_bake_tests.cpp geometry_bake_tests ApexEditor::Authoring)
apex_add_native_test(project_io_tests.cpp project_io_tests ApexEditor::Authoring)
apex_add_native_test(asset_index_tests.cpp asset_index_tests ApexEditor::Assets)
apex_add_native_test(asset_source_tests.cpp asset_source_tests ApexEditor::Assets)
apex_add_native_test(csp_config_model_tests.cpp csp_config_model_tests ApexEditor::Csp)
apex_add_native_test(csp_material_evaluator_tests.cpp csp_material_evaluator_tests ApexEditor::Csp)
apex_add_native_test(car_damage_tests.cpp car_damage_tests ApexEditor::Domain)
apex_add_native_test(car_validation_tests.cpp car_validation_tests ApexEditor::Domain)
apex_add_native_test(driver_tests.cpp driver_tests ApexEditor::Domain)
apex_add_native_test(skin_metadata_tests.cpp skin_metadata_tests ApexEditor::Domain)
apex_add_native_test(track_data_tests.cpp track_data_tests ApexEditor::Domain)
apex_add_native_test(kn5_scene_tests.cpp kn5_scene_tests ApexEditor::Scene)
apex_add_native_test(workspace_tests.cpp workspace_tests ApexEditor::Workspace)
apex_add_native_test(camera_tests.cpp camera_tests ApexEditor::Render)
apex_add_native_test(render_backend_tests.cpp render_backend_tests ApexEditor::Render)
apex_add_native_test(draw_packet_tests.cpp draw_packet_tests ApexEditor::Render)
apex_add_native_test(frame_plan_tests.cpp frame_plan_tests ApexEditor::Render)
apex_add_native_test(indexed_draw_tests.cpp indexed_draw_tests ApexEditor::Render)
apex_add_native_test(static_mesh_upload_tests.cpp static_mesh_upload_tests ApexEditor::Render)
apex_add_native_test(skinned_mesh_upload_tests.cpp skinned_mesh_upload_tests ApexEditor::Render)
apex_add_native_test(static_scene_tests.cpp static_scene_tests ApexEditor::Render)
apex_add_native_test(lighting_tests.cpp lighting_tests ApexEditor::Render)
apex_add_native_test(material_binding_tests.cpp material_binding_tests ApexEditor::Render)
apex_add_native_test(material_profile_tests.cpp material_profile_tests ApexEditor::Render)
apex_add_native_test(pipeline_tests.cpp pipeline_tests ApexEditor::Render)
apex_add_native_test(render_plan_tests.cpp render_plan_tests ApexEditor::Render)
apex_add_native_test(texture_upload_tests.cpp texture_upload_tests ApexEditor::Render)
apex_add_native_test(platform_security_tests.cpp platform_security_tests ApexEditor::Platform)

if(TARGET render_backend_tests AND WIN32)
  # The Windows execution test compiles its small HLSL fixture at runtime so
  # CI exercises real D3D12 shader bytecode instead of a shaped container.
  target_link_libraries(render_backend_tests PRIVATE d3dcompiler)
endif()

if(TEST ksanim_tests)
  set_tests_properties(ksanim_tests PROPERTIES LABELS "unit;malformed")
endif()
if(TEST knh_tests)
  set_tests_properties(knh_tests PROPERTIES LABELS "unit;malformed")
endif()
foreach(parser_test IN ITEMS kn5_tests kn5_write_tests dds_tests bc7_tests fbx_tests
                             fbx_conversion_tests acd_tests ini_tests vao_tests)
  if(TEST ${parser_test})
    set_tests_properties(${parser_test} PROPERTIES LABELS "unit;malformed")
  endif()
endforeach()
foreach(boundary_test IN ITEMS authoring_project_tests geometry_bake_tests project_io_tests
                               asset_index_tests asset_source_tests csp_config_model_tests
                               csp_material_evaluator_tests car_damage_tests car_validation_tests
                               driver_tests skin_metadata_tests
                               track_data_tests kn5_scene_tests workspace_tests)
  if(TEST ${boundary_test})
    set_tests_properties(${boundary_test} PROPERTIES LABELS "unit;malformed")
  endif()
endforeach()
if(TEST render_backend_tests)
  set_tests_properties(render_backend_tests PROPERTIES LABELS "unit;render")
  if(APEX_ENABLE_SANITIZERS AND NOT MSVC)
    # Mesa/lavapipe keeps small loader allocations alive until process exit.
    # Keep ASan/UBSan enabled for our backend code while excluding that
    # external-driver lifetime behavior from the generic runtime contract.
    set_property(TEST render_backend_tests APPEND PROPERTY
      ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  endif()
endif()
foreach(render_test IN ITEMS camera_tests draw_packet_tests frame_plan_tests indexed_draw_tests static_mesh_upload_tests skinned_mesh_upload_tests static_scene_tests lighting_tests material_binding_tests
                             material_profile_tests pipeline_tests render_plan_tests texture_upload_tests)
  if(TEST ${render_test})
    set_tests_properties(${render_test} PROPERTIES LABELS "unit;render")
  endif()
endforeach()
if(TEST platform_security_tests)
  set_tests_properties(platform_security_tests PROPERTIES LABELS "unit;security")
endif()

if(TARGET render_backend_tests)
  if(APEX_VULKAN_AVAILABLE)
    add_test(NAME render_backend_vulkan COMMAND render_backend_tests)
    set_tests_properties(render_backend_vulkan PROPERTIES
      ENVIRONMENT "APEX_RENDER_BACKEND=vulkan;APEX_RENDER_VALIDATION=1"
      SKIP_RETURN_CODE 77
      SKIP_REGULAR_EXPRESSION "[Uu]navailable|[Nn]ot available|[Nn]o physical device"
    )
    if(APEX_ENABLE_SANITIZERS AND NOT MSVC)
      set_property(TEST render_backend_vulkan APPEND PROPERTY
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
    endif()
  endif()

  if(WIN32 AND APEX_ENABLE_D3D12)
    add_test(NAME render_backend_d3d12_warp COMMAND render_backend_tests)
    set_tests_properties(render_backend_d3d12_warp PROPERTIES
      ENVIRONMENT "APEX_RENDER_BACKEND=d3d12;APEX_D3D12_ADAPTER=warp;APEX_RENDER_VALIDATION=1"
      SKIP_RETURN_CODE 77
      SKIP_REGULAR_EXPRESSION "[Uu]navailable|[Nn]ot available|[Nn]o D3D12 adapter"
    )
  endif()
endif()
