cmake_minimum_required(VERSION 3.21)
project(ReSaverPP VERSION 19 LANGUAGES C CXX)

# ReSaver++ : C++ port of ReSaver (FallrimTools) by Mark Fairchild. Apache License 2.0.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (MSVC)
	add_compile_options(/utf-8 /MP)
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(RESAVERPP_GUI "Build the GUI" ON)
option(RESAVERPP_TESTS "Build tests" ON)

# ---- third party ------------------------------------------------------------------------
add_library(rs_thirdparty STATIC
	third_party/lz4/lz4.c
	third_party/miniz/miniz.c
	third_party/miniz/miniz_tdef.c
	third_party/miniz/miniz_tinfl.c)
target_include_directories(rs_thirdparty PUBLIC third_party/lz4 third_party/miniz)
target_compile_definitions(rs_thirdparty PUBLIC MINIZ_NO_ARCHIVE_APIS MINIZ_NO_STDIO MINIZ_NO_TIME)

# ---- core -------------------------------------------------------------------------------
file(GLOB RS_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(rs_core STATIC ${RS_CORE_SOURCES})
target_include_directories(rs_core PUBLIC src)
target_link_libraries(rs_core PUBLIC rs_thirdparty)

# ---- Windows version information and manifest (file Properties > Details) -----------------
function(rs_add_version_info target name description)
	if (WIN32)
		set(RS_NAME "${name}")
		set(RS_DESCRIPTION "${description}")
		set(RS_MANIFEST "${CMAKE_SOURCE_DIR}/res/resaverpp.manifest")
		configure_file(${CMAKE_SOURCE_DIR}/res/version.rc.in ${CMAKE_BINARY_DIR}/${name}_version.rc @ONLY)
		target_sources(${target} PRIVATE ${CMAKE_BINARY_DIR}/${name}_version.rc)
		if (MSVC)
			target_link_options(${target} PRIVATE /MANIFEST:NO)  # the manifest comes from the .rc
		endif()
	endif()
endfunction()

# ---- command line -----------------------------------------------------------------------
file(GLOB RS_CLI_SOURCES CONFIGURE_DEPENDS src/cli/*.cpp)
if (RS_CLI_SOURCES)
	add_executable(resaverpp-cli ${RS_CLI_SOURCES})
	target_link_libraries(resaverpp-cli PRIVATE rs_core)
	rs_add_version_info(resaverpp-cli resaverpp-cli "ReSaver++ command line - Skyrim save editor")
	if (MINGW)
		target_link_options(resaverpp-cli PRIVATE -static)
	endif()
endif()

if (RESAVERPP_TESTS)
	add_executable(rs_roundtrip tests/roundtrip.cpp)
	target_link_libraries(rs_roundtrip PRIVATE rs_core)
	add_executable(rs_edit_test tests/edit_test.cpp)
	target_link_libraries(rs_edit_test PRIVATE rs_core)
	add_executable(rs_cf_test tests/cf_test.cpp)
	target_link_libraries(rs_cf_test PRIVATE rs_core)
	add_executable(rs_decomp_test tests/decomp_test.cpp)
	target_link_libraries(rs_decomp_test PRIVATE rs_core)
	add_executable(rs_psc_test tests/psc_test.cpp)
	target_link_libraries(rs_psc_test PRIVATE rs_core)
	add_executable(rs_unit tests/unit_test.cpp)
	target_link_libraries(rs_unit PRIVATE rs_core)
	add_executable(rs_save_test tests/save_test.cpp)
	target_link_libraries(rs_save_test PRIVATE rs_core)

	# ---- CTest ------------------------------------------------------------------------------
	enable_testing()
	# needs nothing: regression tests built from data made in memory
	add_test(NAME unit COMMAND rs_unit)
	# real saves: -DRESAVERPP_TEST_SAVES=<folder with .ess files>. Each is loaded, written back and
	# compared, backups checked. A folder given without saves in it fails the run (fixtures missing).
	set(RESAVERPP_TEST_SAVES "" CACHE PATH "Folder of .ess saves for the fixture tests")
	if (RESAVERPP_TEST_SAVES)
		file(GLOB RS_TEST_SAVES "${RESAVERPP_TEST_SAVES}/*.ess")
		if (NOT RS_TEST_SAVES)
			message(FATAL_ERROR "RESAVERPP_TEST_SAVES=${RESAVERPP_TEST_SAVES} has no .ess files (fixtures missing)")
		endif()
		set(n 0)
		foreach (save ${RS_TEST_SAVES})
			math(EXPR n "${n} + 1")
			get_filename_component(name "${save}" NAME_WE)
			add_test(NAME "save_${n}" COMMAND rs_save_test "${save}" "${CMAKE_BINARY_DIR}/save_test_${n}")
			add_test(NAME "edit_${n}" COMMAND rs_edit_test "${save}" "${CMAKE_BINARY_DIR}/edit_test_${n}.ess")
			add_test(NAME "roundtrip_${n}" COMMAND rs_roundtrip "${save}")
			set_tests_properties("roundtrip_${n}" PROPERTIES FAIL_REGULAR_EXPRESSION "MISMATCH")
		endforeach()
	else()
		add_test(NAME fixtures COMMAND ${CMAKE_COMMAND} -E echo "No real saves configured (-DRESAVERPP_TEST_SAVES=<folder>): fixture tests skipped")
		set_tests_properties(fixtures PROPERTIES SKIP_REGULAR_EXPRESSION "skipped")
	endif()
endif()

# ---- GUI --------------------------------------------------------------------------------
if (RESAVERPP_GUI AND EXISTS ${CMAKE_SOURCE_DIR}/src/gui/main.cpp)
	set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
	set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
	add_subdirectory(third_party/glfw EXCLUDE_FROM_ALL)
	find_package(OpenGL REQUIRED)

	add_library(rs_imgui STATIC
		third_party/imgui/imgui.cpp
		third_party/imgui/imgui_draw.cpp
		third_party/imgui/imgui_tables.cpp
		third_party/imgui/imgui_widgets.cpp
		third_party/imgui/imgui_demo.cpp
		third_party/imgui/backends/imgui_impl_glfw.cpp
		third_party/imgui/backends/imgui_impl_opengl3.cpp
		third_party/ImGuiColorTextEdit/TextEditor.cpp)
	target_include_directories(rs_imgui PUBLIC third_party/imgui third_party/imgui/backends third_party/ImGuiColorTextEdit)
	target_link_libraries(rs_imgui PUBLIC glfw OpenGL::GL)

	file(GLOB RS_GUI_SOURCES CONFIGURE_DEPENDS src/gui/*.cpp)
	add_executable(ReSaverPP WIN32 ${RS_GUI_SOURCES})
	target_link_libraries(ReSaverPP PRIVATE rs_core rs_imgui)
	rs_add_version_info(ReSaverPP ReSaverPP "ReSaver++ - Skyrim save editor")
	if (WIN32)
		target_link_libraries(ReSaverPP PRIVATE comdlg32 shell32 ole32 advapi32)
		if (MSVC)
			target_link_options(ReSaverPP PRIVATE /ENTRY:wWinMainCRTStartup)
		else()
			target_link_options(ReSaverPP PRIVATE -municode -static)
		endif()
	endif()
endif()
