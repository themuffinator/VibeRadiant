cmake_minimum_required(VERSION 3.16)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/VibeyVersion.cmake")
vmt_resolve_version("${CMAKE_CURRENT_SOURCE_DIR}")

project (VibeyMapTools VERSION ${VIBEY_VERSION_NUMERIC})

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

message(STATUS "VibeyMapTools version: ${VIBEY_VERSION_FULL}")
if(VIBEY_VERSION_GIT)
	message(STATUS "Version label: ${VIBEY_VERSION_GIT}")
endif()

option(DISABLE_TESTS "Disable tests and their test-only dependencies" ON)
option(DISABLE_DOCS "Disable documentation" ON)
option(ENABLE_HUB "Enable vmt-hub tool" OFF)
option(VIBEYMAPTOOLS_ASAN "Enable AddressSanitizer for all targets" OFF)
option(VIBEYMAPTOOLS_TIMETRACE "Enable Clang time-trace output" OFF)

if(NOT DISABLE_TESTS)
	message(FATAL_ERROR "This VibeRadiant vendor subset does not include the VibeyMapTools test sources; configure with DISABLE_TESTS=ON")
endif()
if(NOT DISABLE_DOCS)
	message(FATAL_ERROR "This VibeRadiant vendor subset does not include the VibeyMapTools documentation sources; configure with DISABLE_DOCS=ON")
endif()

# Build date
string(TIMESTAMP VIBEY_BUILD_DATE "%Y-%m-%d")

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
	add_compile_options(/EHsc)
	message(STATUS "working around exceptions not being enabled by default on clang-cl")
endif()

include_directories(src/include)

find_package(Threads REQUIRED)

if (CMAKE_USE_PTHREADS_INIT)
	add_definitions(-DUSE_PTHREADS)
elseif (CMAKE_USE_WIN32_THREADS_INIT)
	add_definitions(-DUSE_WIN32THREADS)
endif ()

if (UNIX AND NOT APPLE)
	add_definitions(-DLINUX)
endif ()

# set our C/C++ dialects
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(CheckIPOSupported)
check_ipo_supported(RESULT VIBEYMAPTOOLS_IPO_SUPPORTED OUTPUT VIBEYMAPTOOLS_IPO_ERROR)
if(VIBEYMAPTOOLS_IPO_SUPPORTED)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
else()
	message(WARNING "Interprocedural optimization is unavailable: ${VIBEYMAPTOOLS_IPO_ERROR}")
endif()

if (UNIX AND NOT APPLE)
	set(CMAKE_INSTALL_RPATH "$ORIGIN")
	set(CMAKE_BUILD_WITH_INSTALL_RPATH YES)
endif ()

add_definitions(-DVIBEYMAPTOOLS_VERSION="${VIBEY_VERSION_FULL}")

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
		add_link_options("/STACK:${STACKSIZE}")

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
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL OFF)
	if(MSVC)
		add_compile_options(/fsanitize=address)
		add_link_options(/fsanitize=address)
	elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
		add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
		add_link_options(-fsanitize=address)
	else()
		message(FATAL_ERROR "VIBEYMAPTOOLS_ASAN is not supported by ${CMAKE_CXX_COMPILER_ID}")
	endif()
endif()

if (VIBEYMAPTOOLS_TIMETRACE)
	if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		message(STATUS "Enabling -ftime-trace")
		add_compile_options(-ftime-trace)
	else()
		message(FATAL_ERROR "VIBEYMAPTOOLS_TIMETRACE requires a Clang-family compiler")
	endif()
endif()

# 10.9: minimum version that supports unordered_map
# 10.14: required by tbb 2021.3.0 (due to use of alignas)
# 10.15: required by std::filesytstem
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.15)

find_package(TBB REQUIRED)
find_package(embree 4 REQUIRED)

# Configure the public version header only after feature discovery so its
# has_* helpers describe the binaries produced by this configuration.
set(HAVE_EMBREE ${embree_FOUND})
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include/version.hh.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/version.hh"
    @ONLY
)

add_subdirectory(extern)

# CMAKE_BUILD_TYPE is empty for multi-config generators. Keep third-party
# targets untouched while giving each first-party target its selected config.
add_compile_definitions(
	VIBEY_BUILD_TYPE=\"$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,Unknown>\"
)

add_subdirectory(src/common)
add_subdirectory(src/bspinfo)
add_subdirectory(src/bsputil)
add_subdirectory(src/light)
add_subdirectory(src/qbsp)
add_subdirectory(src/vis)
add_subdirectory(src/maputil)

if (ENABLE_HUB)
	add_subdirectory(src/hub)
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
