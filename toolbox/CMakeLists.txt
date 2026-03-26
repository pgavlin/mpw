
if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
	# Clang or AppleClang
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-declarations")
endif()

include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

set(TOOLBOX_SRC 
	toolbox.cpp
	mm.cpp
	loader.cpp
	rm.cpp
	os.cpp
	os_alias.cpp
	os_fileinfo.cpp
	os_gestalt.cpp
	os_hfs_dispatch.cpp
	os_highlevel.cpp
	os_hwpriv.cpp
	os_internal.cpp
	os_volume.cpp
	qd.cpp
	sane.cpp
	packages.cpp
	pathnames.cpp
	process.cpp
	utility.cpp
	fs_spec.cpp
	path_utils.cpp
	realpath.c
	dispatch.cpp
	fpinfo.cpp
	debug.cpp
	pef_loader.cpp
	cfm_stubs.cpp
	ppc_dispatch.cpp

)

add_custom_command(
	OUTPUT pathnames.cpp
	COMMAND "${RAGEL}" -p -G2 -o pathnames.cpp "${CMAKE_CURRENT_SOURCE_DIR}/pathnames.rl"
	MAIN_DEPENDENCY pathnames.rl 
)

set_source_files_properties(
	pathnames.cpp
	PROPERTIES
	COMPILE_FLAGS 
	"${CMAKE_CXX_FLAGS} -Wno-unused-variable"
)


add_library(TOOLBOX_LIB ${TOOLBOX_SRC})
target_link_libraries(TOOLBOX_LIB MACOS_LIB sane RSRC_LIB)

add_executable(pef_test pef_test.cpp)
target_link_libraries(pef_test TOOLBOX_LIB MPW_LIB MACOS_LIB MPLITE_LIB M68K_LIB RSRC_LIB sane)

add_executable(cfm_test cfm_test.cpp)
target_link_libraries(cfm_test TOOLBOX_LIB MPW_LIB MACOS_LIB MPLITE_LIB M68K_LIB PPC_LIB RSRC_LIB sane)

add_executable(dispatch_test dispatch_test.cpp)
target_link_libraries(dispatch_test TOOLBOX_LIB MPW_LIB MACOS_LIB MPLITE_LIB M68K_LIB PPC_LIB RSRC_LIB sane)
