set(MAPUTIL_SOURCES
	maputil.cc
	../include/maputil/maputil.hh
)

find_package(Lua)

add_library(libmaputil STATIC ${MAPUTIL_SOURCES})

target_link_libraries(libmaputil common TBB::tbb TBB::tbbmalloc fmt::fmt)

if (LUA_LIBRARIES)
	target_link_libraries(libmaputil ${LUA_LIBRARIES})
	target_include_directories(libmaputil PRIVATE ${LUA_INCLUDE_DIR})
endif()

add_executable(vmt-maputil main.cc)
target_link_libraries(vmt-maputil libmaputil)

if (LUA_LIBRARIES)
	target_include_directories(vmt-maputil PRIVATE ${LUA_INCLUDE_DIR})
	add_definitions(-DUSE_LUA)
endif()

# HACK: copy .dll dependencies
add_custom_command(TARGET vmt-maputil POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbb>" "$<TARGET_FILE_DIR:vmt-maputil>"
				   COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:TBB::tbbmalloc>" "$<TARGET_FILE_DIR:vmt-maputil>"
				   )
copy_mingw_dlls(vmt-maputil)
add_loader_path_to_rpath(vmt-maputil)

install(TARGETS vmt-maputil RUNTIME DESTINATION .)
