cmake_minimum_required(VERSION 3.25)

project(ApexEditorNative VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(APEX_BUILD_TESTS "Build the native unit and backend tests" ${BUILD_TESTING})
option(APEX_WARNINGS_AS_ERRORS "Treat native compiler warnings as errors" ON)
option(APEX_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(APEX_ENABLE_VULKAN "Build Vulkan support when the SDK is available" ON)
option(APEX_ENABLE_D3D12 "Build D3D12 support on Windows" ${WIN32})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(native)
