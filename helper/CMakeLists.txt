cmake_minimum_required(VERSION 3.21)

# DLL name: ReSaverHelper.dll - the live-game companion of ReSaver++ (ReSaver Plus)
project(ReSaverHelper VERSION 10.0.0 LANGUAGES CXX)

if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
	message(FATAL_ERROR "in-source builds are not allowed")
endif()

# Build output stays in the build folder only (build\release-msvc or build\debug-msvc).
# Nothing is copied anywhere else; copy ReSaverHelper.dll, ReSaverHelper.pdb and ReSaverHelper.ini yourself.

# CommonLibSSE-NG source tree (the same copy The Watcher and Precision Animal Excluder build against).
# Override the location with the CommonLibSSEPath_NG environment variable.
if(DEFINED ENV{CommonLibSSEPath_NG} AND IS_DIRECTORY "$ENV{CommonLibSSEPath_NG}")
	set(COMMONLIB_DIR "$ENV{CommonLibSSEPath_NG}")
else()
	set(COMMONLIB_DIR "C:/dllmaker/CommonLibSSE-NG")
endif()
message(STATUS "Using CommonLibSSE-NG from: ${COMMONLIB_DIR}")

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
# VR needs the OpenVR SDK headers and import library in CommonLibSSE-NG\extern\openvr (as for The Watcher).
# The helper calls no OpenVR function, so the DLL does not load openvr_api.dll on SE or AE.
set(ENABLE_SKYRIM_VR ON CACHE BOOL "" FORCE)
if(NOT EXISTS "${COMMONLIB_DIR}/extern/openvr/headers/openvr.h" OR NOT EXISTS "${COMMONLIB_DIR}/extern/openvr/lib/win64/openvr_api.lib")
	message(FATAL_ERROR "Skyrim VR support needs ${COMMONLIB_DIR}/extern/openvr/headers/openvr.h and lib/win64/openvr_api.lib (OpenVR SDK)")
endif()
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_PATCH_SAFETY OFF CACHE BOOL "" FORCE)

add_subdirectory("${COMMONLIB_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)
if(NOT COMMAND add_commonlibsse_plugin)
	include("${COMMONLIB_DIR}/cmake/CommonLibSSE.cmake")
endif()

include(cmake/headerlist.cmake)
include(cmake/sourcelist.cmake)
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${headers} ${sources})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Z7>)
target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)

# named pipe security (owner-only access): advapi32
target_link_libraries(${PROJECT_NAME} PRIVATE advapi32)
