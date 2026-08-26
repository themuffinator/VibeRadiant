cmake_minimum_required (VERSION 3.14)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VibeyVersion.cmake")
vmt_resolve_version("${CMAKE_CURRENT_SOURCE_DIR}")

project (VibeyMapTools VERSION ${VIBEY_VERSION_NUMERIC})

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/extern/Catch2/extras")

message(STATUS "VibeyMapTools version: ${VIBEY_VERSION_FULL}")
if(VIBEY_VERSION_GIT)
	message(STATUS "Version label: ${VIBEY_VERSION_GIT}")
endif()

# Build date
string(TIMESTAMP VIBEY_BUILD_DATE "%Y-%m-%d")

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
	add_compile_options(/EHsc)
	message(STATUS "working around exceptions not being enabled by default on clang-cl")
endif()

include_directories(src/include)

find_package (Threads)

if (CMAKE_USE_PTHREADS_INIT)
	add_definitions(-DUSE_PTHREADS)
elseif (CMAKE_USE_WIN32_THREADS_INIT)
	add_definitions(-DUSE_WIN32THREADS)
endif ()

if (UNIX)
	add_definitions(-DLINUX)
endif (UNIX)

# set our C/C++ dialects
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF)

if (UNIX AND NOT APPLE)
	set(CMAKE_INSTALL_RPATH "$ORIGIN")
	set(CMAKE_BUILD_WITH_INSTALL_RPATH YES)
endif ()

add_definitions(-DVIBEYMAPTOOLS_VERSION="${VIBEY_VERSION_FULL}")

# Configure version header
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include/version.hh.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/version.hh"
    @ONLY
)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/include")

# MINGW stuff
if(MINGW)
	find_file(LIB_GCC_S_SEH_1_DLL NAMES "libgcc_s_seh-1.dll")
	find_file(LIB_STDCPP_6_DLL NAMES "libstdc++-6.dll")
	find_file(LIB_WINPTHREAD_1_DLL NAMES "libwinpthread-1.dll")

	if(LIB_GCC_S_SEH_1_DLL)
		install(FILES ${LIB_GCC_S_SEH_1_DLL} DESTINATION .)
	endif()
	if(LIB_STDCPP_6_DLL)
		install(FILES ${LIB_STDCPP_6_DLL} DESTINATION .)
	endif()
	if(LIB_WINPTHREAD_1_DLL)
		install(FILES ${LIB_WINPTHREAD_1_DLL} DESTINATION .)
	endif()
endif()

function(copy_mingw_dlls TARGETNAME)
	if (LIB_GCC_S_SEH_1_DLL)
		add_custom_command(TARGET ${TARGETNAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIB_GCC_S_SEH_1_DLL}" "$<TARGET_FILE_DIR:${TARGETNAME}>"
		)
	endif()
	if (LIB_STDCPP_6_DLL)
		add_custom_command(TARGET ${TARGETNAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIB_STDCPP_6_DLL}" "$<TARGET_FILE_DIR:${TARGETNAME}>"
		)
	endif()
	if (LIB_WINPTHREAD_1_DLL)
		add_custom_command(TARGET ${TARGETNAME} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIB_WINPTHREAD_1_DLL}" "$<TARGET_FILE_DIR:${TARGETNAME}>"
		)
	endif()
endfunction()

# so the executable will search for dylib's in the same directory as the executable
function(add_loader_path_to_rpath TARGETNAME)
	if(APPLE)
		add_custom_command(TARGET ${TARGETNAME} POST_BUILD
			COMMAND bash ARGS -c \"install_name_tool -add_rpath @loader_path $<TARGET_FILE:${TARGETNAME}> || true\")
	endif()
endfunction()

if (WIN32)
	set("NO_ITERATOR_DEBUG" FALSE CACHE BOOL "Whether to use MSVC iterator debugging or not")

	if (NO_ITERATOR_DEBUG)
		add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)
		add_definitions(-D_CONTAINER_DEBUG_LEVEL=0)
	endif (NO_ITERATOR_DEBUG)

	# TODO: remove these
	add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

	if (MSVC)
		# request 8MB stack for all .exe's
		set(STACKSIZE 16388608)
		set(CMAKE_EXE_LINKER_FLAGS "/STACK:${STACKSIZE}")

		add_definitions("/wd4244") # disable "conversion from .. to .., possible loss of data" warning
		add_definitions("/wd4018") # disable "signed/unsigned mismatch" warning
		add_definitions("/wd4200") # disable "nonstandard extension used: zero-sized array in struct/union" warning
		add_definitions("/wd4264") # disable "conversion from 'size_t' to 'int', possible loss of data" warning
		add_definitions("/wd4267") # disable "conversion from 'size_t' to 'int', possible loss of data" warning
		add_definitions("/wd4305") # disable "truncation from 'double' to 'float'" warning
		add_definitions("/wd4250") # disable "inherits via dominance' warning (used in trace_embree.cc)
	endif (MSVC)
endif (WIN32)

# Pass -DVIBEYMAPTOOLS_ASAN=YES to enable for all targets
if (VIBEYMAPTOOLS_ASAN)
	message(STATUS "Enabling ASan on all targets")
	add_compile_options(-fsanitize=address)
	add_link_options(-fsanitize=address)
endif()

if (VIBEYMAPTOOLS_TIMETRACE)
	message(STATUS "Enabling -ftime-trace")
	add_compile_options(-ftime-trace)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
	add_compile_options(/EHsc)
endif()

# 10.9: minimum version that supports unordered_map
# 10.14: required by tbb 2021.3.0 (due to use of alignas)
# 10.15: required by std::filesytstem
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15)

find_package(TBB REQUIRED)
find_package(embree 4 REQUIRED)

set(TEST_QUAKE_MAP_EXPORT_DIR "" CACHE PATH "When running unit tests, export Quake maps to this directory (useful for testing in game)")
set(TEST_QUAKE2_MAP_EXPORT_DIR "" CACHE PATH "When running unit tests, export Quake 2 maps to this directory (useful for testing in game)")
set(TEST_HEXEN2_MAP_EXPORT_DIR "" CACHE PATH "When running unit tests, export Hexen 2 maps to this directory (useful for testing in game)")
set(TEST_HALFLIFE_MAP_EXPORT_DIR "" CACHE PATH "When running unit tests, export Half-Life maps to this directory (useful for testing in game)")

add_subdirectory(extern)
add_subdirectory(src/common)
add_subdirectory(src/bspinfo)
add_subdirectory(src/bsputil)
add_subdirectory(src/light)
add_subdirectory(src/qbsp)
add_subdirectory(src/vis)
add_subdirectory(src/maputil)

option(DISABLE_TESTS "Disables Tests" OFF)
option(DISABLE_DOCS "Disables Docs" OFF)
option(ENABLE_HUB "Enable vmt-hub tool" ON)

if (ENABLE_HUB)
	add_subdirectory(src/hub)
endif ()

if(NOT DISABLE_TESTS)
	enable_testing()
	# just creates testmaps.hh with absolute path to the testmaps source directory.
	# include it as #include <testmaps.hh>
	configure_file(tests/testmaps.hh.in testmaps.hh @ONLY)
	include_directories(${CMAKE_CURRENT_BINARY_DIR})
	add_subdirectory(tests)
endif ()

if(NOT DISABLE_DOCS)
	add_subdirectory(docs)
endif ()

install(FILES README.md DESTINATION .)

#CPack configuration

if(WIN32)
	set(CPACK_GENERATOR ZIP)
else()
	set(CPACK_GENERATOR TGZ)
endif()
set(CPACK_PACKAGE_NAME VibeyMapTools)
set(CPACK_PACKAGE_VERSION ${VIBEY_VERSION_PACKAGE})
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY FALSE)
include(CPack)
